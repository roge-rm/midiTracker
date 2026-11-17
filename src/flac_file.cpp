#include "flac_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include "shared_ram_arena.h"
#include <string.h>
#include <stdio.h>
#include "dr_flac_config.h"

namespace {

// Arena layout: dr_flac's allocations from the start (a bump pool: its
// state with the read buffer, one decoded block as 32-bit samples per
// channel and the seek table), then the input ring, then one chunk of
// 16-bit output at the end.
//
// The input ring is a single-producer (core 0, fillRing()), single-consumer
// (core 1, ringRead() under dr_flac) byte ring of any size. head and tail
// are byte counts that only go up: head is written by core 0 only, tail by
// core 1 only. Byte n is at n % size.
// - core 0: reads tail, dmb, writes the bytes, dmb, head;
// - core 1: reads eof, dmb, head, dmb, reads the bytes, dmb, then tail.
// Core 1 starts a FLAC frame only when the ring holds need bytes: the
// largest frame plus one more dr_flac read, as dr_flac reads ahead a
// DR_FLAC_BUFFER_SIZE at a time and takes a short read for the end.
const uint32_t PCM_FRAMES = 1152;      // output chunk, as an MP3 frame, so the WAV stream needs the same room
const uint32_t READ_MIN = 2048;        // core 0 reads only when this much is free
const uint32_t READ_MAX = 8192;        // and at most this much per update()
const uint32_t RING_SLACK = 4096;      // ring bytes past need, so core 0 can read while core 1 waits
const uint32_t FRAME_SLACK = 64;       // frame and subframe headers and the CRC, over the samples
const uint32_t STREAMINFO_BYTES = 34;
const uint8_t MAX_FAILURES = 32;       // bad frames in a row before it's the end
const uint32_t MIN_RATE = 4000, MAX_RATE = 192000; // as WavPlayer takes
// A chunk's output at 44.1k stays within half the WAV stream (8192 frames),
// so at low rates a chunk is shorter than PCM_FRAMES.
const uint32_t MAX_OUT_PER_CHUNK = 4096;

#ifdef ARDUINO_ARCH_RP2040
#define FLAC_DMB() __dmb()
#else
#define FLAC_DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

// Between the cores.
struct Shared {
    volatile uint32_t head = 0, tail = 0; // input ring, see above
    volatile bool eof = false;            // core 0: no input will come after head
    volatile bool run = false;            // core 0: core 1 may decode
    volatile bool inService = false;      // core 1: inside core1Push()/core1Decode()
    volatile bool ended = false;          // core 1: data ran out, wavStreamEnd() called
};
Shared g_sh;

// Core 1's decoder state. Core 0 sets it up before it sets g_sh.run and
// doesn't touch it again until stopCore1() returns.
struct Core1 {
    drflac* flac;
    uint8_t* ring;
    uint32_t ringSize, need;
    int16_t* pcm;
    uint32_t pcmFrames, pcmPos, outPerChunk;
    uint32_t chunkFrames; // most frames per chunk, up to PCM_FRAMES
    uint8_t pcmChannels;
    // Linear resampler, as WavPlayer's.
    uint32_t step, frac;
    int16_t lastL, lastR, nextL, nextR;
    bool starving;
    uint8_t failures; // chunks in a row that found no frame
};
Core1 g_c1;

// dr_flac's view of the file. Offsets are from the "fLaC" marker, which is
// dr_flac's 0. In direct mode (load, seek, Convert; core 0) dr_flac reads
// the card; otherwise it reads the input ring (core 1).
struct Io {
    FsFile* file;
    uint32_t base, size; // file offset of the marker, bytes from it to the end
    uint32_t pos;        // direct mode: the next byte
    uint32_t ringStart;  // ring byte count 0 is this offset
    uint32_t hideAt;     // a SEEKTABLE block header read as PADDING, UINT32_MAX for none
    bool direct;
    bool readFailed;
};
Io g_io;

// dr_flac's memory: a bump pool at the arena's start. Nothing is given
// back until the next load, which starts the pool again.
struct Pool {
    uint8_t* base;
    size_t cap, used;
};
Pool g_pool;

void* poolMalloc(size_t sz, void*) {
    size_t at = (g_pool.used + 15) & ~(size_t)15;
    if (at > g_pool.cap || sz > g_pool.cap - at) return nullptr;
    g_pool.used = at + sz;
    return g_pool.base + at;
}

void poolFree(void*, void*) {}

#ifdef TRACKER_PROFILE
// As in mp3_file.cpp: written by core 1 only, they only go up, and the
// worst decode per window through the epoch slots.
volatile uint32_t g_frames = 0, g_decodeUs = 0, g_rejects = 0, g_starved = 0;
volatile uint32_t g_worstEpoch = 0, g_worstSlot[2] = {0, 0};
volatile bool g_worstBusy = false;
FlacPlayer::Profile g_reads; // core 0 only
uint32_t g_frames0 = 0, g_decodeUs0 = 0, g_rejects0 = 0, g_starved0 = 0;

void noteDecode(uint32_t us) {
    if (us > 0xFFFFFF) us = 0xFFFFFF;
    g_worstBusy = true;
    FLAC_DMB(); // busy written before the epoch is read
    uint32_t e = g_worstEpoch & 0xFF;
    uint32_t w = g_worstSlot[e & 1];
    if ((w >> 24) != e || us > (w & 0xFFFFFF)) g_worstSlot[e & 1] = (e << 24) | us;
    FLAC_DMB(); // slot written before busy clears
    g_worstBusy = false;
}
#endif

uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
uint32_t be24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | be24(p + 1); }

// What load() and probe() need from the header blocks.
struct Info {
    uint32_t base, audioStart, fileEnd; // file offsets
    uint32_t rate, maxFrame;
    uint16_t maxBlock;
    uint8_t channels, bits;
    uint64_t frames;
    uint32_t seekHeader, seekBytes; // offset from base of the SEEKTABLE block header, 0 when none
};

// Reads the header blocks after any ID3v2 tags. nullptr when it's a FLAC
// this player takes, else why not.
const char* readInfo(FsFile& f, Info& in) {
    memset(&in, 0, sizeof(in));
    uint32_t size = (uint32_t)f.fileSize();
    in.fileEnd = size;
    uint32_t pos = 0;
    uint8_t h[10];
    for (int n = 0; n < 4; n++) {
        if (!f.seekSet(pos) || f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) break;
        pos += 10 + (((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) | ((uint32_t)(h[8] & 0x7F) << 7) | (h[9] & 0x7F)) +
               ((h[5] & 0x10) ? 10 : 0);
        if (pos >= size) return "not a FLAC file";
    }
    if (!f.seekSet(pos) || f.read(h, 4) != 4 || memcmp(h, "fLaC", 4) != 0) return "not a FLAC file";
    in.base = pos;
    pos += 4;
    bool haveInfo = false;
    for (int n = 0; n < 256; n++) {
        if (!f.seekSet(pos) || f.read(h, 4) != 4) return "not a FLAC file";
        uint32_t type = h[0] & 0x7F, len = be24(h + 1);
        if (type == 127 || len > size - pos - 4) return "not a FLAC file";
        if (n == 0) {
            uint8_t b[STREAMINFO_BYTES];
            if (type != 0 || len < STREAMINFO_BYTES || f.read(b, STREAMINFO_BYTES) != (int)STREAMINFO_BYTES) return "not a FLAC file";
            in.maxBlock = (uint16_t)be16(b + 2);
            in.maxFrame = be24(b + 7);
            in.rate = (be24(b + 10) >> 4);
            in.channels = (uint8_t)(((b[12] >> 1) & 7) + 1);
            in.bits = (uint8_t)((((b[12] & 1) << 4) | (b[13] >> 4)) + 1);
            in.frames = ((uint64_t)(b[13] & 0x0F) << 32) | be32(b + 14);
            haveInfo = true;
        } else if (type == 3 && !in.seekHeader) {
            in.seekHeader = pos - in.base;
            in.seekBytes = len;
        }
        pos += 4 + len;
        if (h[0] & 0x80) break;
    }
    if (!haveInfo || in.rate == 0 || in.maxBlock < 16 || in.bits < 4) return "not a FLAC file";
    if (in.rate < MIN_RATE || in.rate > MAX_RATE) return "sample rate not supported";
    if (in.channels > 2) return "more than 2 channels";
    if (in.bits > 24) return "32-bit not supported";
    in.audioStart = pos;
    return nullptr;
}

// ---- dr_flac's callbacks ----

size_t ringRead(uint8_t* out, size_t n) {
    size_t done = 0;
    while (done < n) {
        bool eof = g_sh.eof;
        FLAC_DMB(); // acquire: eof read before head, so eof covers every byte below head
        uint32_t head = g_sh.head;
        FLAC_DMB(); // acquire: head read before the bytes it covers
        uint32_t tail = g_sh.tail;
        uint32_t avail = head - tail;
        if (avail == 0) {
            if (eof) break;
#ifdef ARDUINO_ARCH_RP2040
            // Only when a frame is bigger than STREAMINFO said: wait for
            // core 0, unless it's stopping core 1.
            if (!g_sh.run) break;
            continue;
#else
            break; // the host harness runs both sides in turn
#endif
        }
        uint32_t pos = tail % g_c1.ringSize;
        uint32_t len = g_c1.ringSize - pos;
        if (len > avail) len = avail;
        if (len > n - done) len = (uint32_t)(n - done);
        memcpy(out + done, g_c1.ring + pos, len);
        FLAC_DMB(); // release: bytes read before core 0 may reuse them
        g_sh.tail = tail + len;
        done += len;
    }
    return done;
}

size_t onRead(void*, void* out, size_t n) {
    if (!g_io.direct) return ringRead((uint8_t*)out, n);
    if (g_io.pos >= g_io.size) return 0;
    if (n > g_io.size - g_io.pos) n = g_io.size - g_io.pos;
    int got = g_io.file->read(out, n);
    if (got < 0) got = 0;
    if ((size_t)got < n) g_io.readFailed = true;
    if (g_io.hideAt >= g_io.pos && g_io.hideAt - g_io.pos < (uint32_t)got) {
        uint8_t* b = (uint8_t*)out + (g_io.hideAt - g_io.pos);
        *b = (uint8_t)((*b & 0x80) | 1); // PADDING, which dr_flac skips
    }
    g_io.pos += (uint32_t)got;
    return (size_t)got;
}

drflac_bool32 onSeek(void*, int offset, drflac_seek_origin origin) {
    if (!g_io.direct) return DRFLAC_FALSE;
    int64_t to = origin == DRFLAC_SEEK_SET ? offset : origin == DRFLAC_SEEK_CUR ? (int64_t)g_io.pos + offset : (int64_t)g_io.size + offset;
    if (to < 0 || to > (int64_t)g_io.size) return DRFLAC_FALSE;
    if (!g_io.file->seekSet(g_io.base + (uint32_t)to)) return DRFLAC_FALSE;
    g_io.pos = (uint32_t)to;
    return DRFLAC_TRUE;
}

drflac_bool32 onTell(void*, drflac_int64* cursor) {
    *cursor = g_io.direct ? g_io.pos : g_io.ringStart + g_sh.tail;
    return DRFLAC_TRUE;
}

} // namespace


// ---- core 1 only, while g_sh.run is set (and core 0 for Convert) ----

namespace {

enum DecodeResult { DECODED, SOURCE_END };

bool takeSrc(int16_t& l, int16_t& r) {
    if (g_c1.pcmPos >= g_c1.pcmFrames) return false;
    const int16_t* s = g_c1.pcm;
    uint32_t i = g_c1.pcmPos++;
    if (g_c1.pcmChannels == 2) {
        l = s[i * 2];
        r = s[i * 2 + 1];
    } else {
        l = r = s[i];
    }
    return true;
}

// Moves the resampler on to the source frame its position needs. false
// when the chunk runs out first, with nothing taken.
bool advance() {
    while (g_c1.frac >= 0x10000) {
        int16_t l, r;
        if (!takeSrc(l, r)) return false;
        g_c1.frac -= 0x10000;
        g_c1.lastL = g_c1.nextL;
        g_c1.lastR = g_c1.nextR;
        g_c1.nextL = l;
        g_c1.nextR = r;
    }
    return true;
}

// Resamples what's left of the chunk into the WAV stream, as far as it has
// room. true when the chunk is used up.
bool pushFrames() {
    const size_t BATCH = 32;
    int16_t out[BATCH * 2];
    for (;;) {
        if (!advance()) return true;
        size_t n = Synth::wavStreamFree();
        if (n == 0) return false;
        if (n > BATCH) n = BATCH;
        size_t k = 0;
        while (k < n) {
            int32_t diffL = (int32_t)g_c1.nextL - (int32_t)g_c1.lastL;
            int32_t diffR = (int32_t)g_c1.nextR - (int32_t)g_c1.lastR;
            int32_t frac15 = (int32_t)((g_c1.frac & 0xFFFF) >> 1);
            out[k * 2] = (int16_t)(g_c1.lastL + ((diffL * frac15) >> 15));
            out[k * 2 + 1] = (int16_t)(g_c1.lastR + ((diffR * frac15) >> 15));
            k++;
            g_c1.frac += g_c1.step;
            if (!advance()) break;
        }
        Synth::wavStreamWrite(out, k);
    }
}

// True when decodeChunk() has what it needs in the ring.
bool inputReady() {
    if (g_c1.flac->currentFLACFrame.pcmFramesRemaining) return true; // no bytes needed
    bool eof = g_sh.eof;
    FLAC_DMB(); // acquire: eof before head
    uint32_t avail = g_sh.head - g_sh.tail;
    return eof || avail >= g_c1.need;
}

// The next chunk into g_c1.pcm: the rest of the current FLAC frame, or a
// new one, at most chunkFrames. A bad frame gives an empty chunk.
DecodeResult decodeChunk() {
    drflac* f = g_c1.flac;
    g_c1.pcmFrames = g_c1.pcmPos = 0;
    uint32_t got = 0;
    if (f->currentFLACFrame.pcmFramesRemaining == 0) {
#ifdef TRACKER_PROFILE
        uint32_t t0 = micros();
#endif
        got = (uint32_t)drflac_read_pcm_frames_s16(f, 1, g_c1.pcm);
#ifdef TRACKER_PROFILE
        if (got) {
            uint32_t d = micros() - t0;
            g_decodeUs = g_decodeUs + d;
            g_frames = g_frames + 1;
            noteDecode(d);
        } else {
            g_rejects = g_rejects + 1;
        }
#endif
        if (!got) {
            // A frame that won't decode (bad data or cut short) ends dr_flac's
            // read; the next call looks for the next frame, while input is left.
            bool more = g_io.direct ? g_io.pos < g_io.size : !g_sh.eof || g_sh.head != g_sh.tail;
            if (!more || ++g_c1.failures > MAX_FAILURES) return SOURCE_END;
            return DECODED;
        }
        g_c1.failures = 0;
    }
    uint32_t take = g_c1.chunkFrames - got, left = f->currentFLACFrame.pcmFramesRemaining;
    if (take > left) take = left;
    if (take) got += (uint32_t)drflac_read_pcm_frames_s16(f, take, g_c1.pcm + got * g_c1.pcmChannels);
    g_c1.pcmFrames = got;
    return DECODED;
}

// Entry and exit for core1Push()/core1Decode(), as Mp3Player's: core 1
// sets inService, then checks run; core 0 clears run, then waits for
// inService to clear.
bool enter() {
    if (!g_sh.run) return false;
    g_sh.inService = true;
    FLAC_DMB();
    if (g_sh.run) return true;
    g_sh.inService = false;
    return false;
}

void leave() {
    FLAC_DMB(); // release: ring, stream and state written before core 0 may take them over
    g_sh.inService = false;
}

} // namespace

bool FlacPlayer::core1Push() {
    if (!enter()) return false;
    bool want = false;
    if (!g_sh.ended && pushFrames()) {
        // A new chunk only when all of its output fits in the stream.
        if (Synth::wavStreamFree() >= g_c1.outPerChunk) {
            want = inputReady();
#ifdef TRACKER_PROFILE
            if (!want && !g_c1.starving) g_starved = g_starved + 1;
#endif
            g_c1.starving = !want;
        }
    }
    leave();
    return want;
}

void FlacPlayer::core1Decode() {
    if (!enter()) return;
    if (!g_sh.ended) {
        if (decodeChunk() == SOURCE_END) {
            Synth::wavStreamEnd();
            FLAC_DMB(); // release: the stream's end flag before ours
            g_sh.ended = true;
        } else {
            pushFrames();
        }
    }
    leave();
}


// ---- core 0 ----

bool FlacPlayer::fail(const char* msg) {
    strncpy(_error, msg, sizeof(_error) - 1);
    _error[sizeof(_error) - 1] = '\0';
    if (_file) _file.close();
    g_c1.flac = nullptr;
    _state = STATE_ERROR;
    return false;
}

void FlacPlayer::refuse(const char* msg) {
    close();
    fail(msg);
}

void FlacPlayer::formatText(char* out, size_t size) const { snprintf(out, size, "%dBIT", _bits); }

uint32_t FlacPlayer::seekPoints() const { return g_c1.flac ? g_c1.flac->seekpointCount : 0; }

bool FlacPlayer::readFailed() const { return g_io.readFailed; }

bool FlacPlayer::probe(const char* path, uint32_t& rate, uint8_t& channels, uint8_t& bits) {
    FsFile f;
    if (!sdOpenPath(f, path, O_RDONLY)) return false;
    Info in;
    bool ok = readInfo(f, in) == nullptr;
    f.close();
    if (!ok) return false;
    rate = in.rate;
    channels = in.channels;
    bits = in.bits;
    return true;
}

void FlacPlayer::fillRing() {
    if (_eof) return;
    uint32_t head = g_sh.head;
    uint32_t free = g_c1.ringSize - (head - g_sh.tail);
    FLAC_DMB(); // acquire: tail read before writing into the bytes it freed
    if (free < READ_MIN) return;
    uint32_t pos = head % g_c1.ringSize;
    uint32_t len = g_c1.ringSize - pos;
    if (len > free) len = free;
    if (len > READ_MAX) len = READ_MAX;
    uint32_t end = (_filePos + len) & ~511u;
    if (end > _filePos) len = end - _filePos; // end on a sector boundary
    if (len > _fileEnd - _filePos) len = _fileEnd - _filePos;
    int got = 0;
    if (len > 0) {
#ifdef TRACKER_PROFILE
        uint32_t t0 = micros();
#endif
        got = _file.read(g_c1.ring + pos, len);
#ifdef TRACKER_PROFILE
        g_reads.readUs += micros() - t0;
        g_reads.reads++;
        g_reads.readBytes += got > 0 ? (uint32_t)got : 0;
#endif
    }
    if (got > 0) {
        FLAC_DMB(); // release: bytes written before head covers them
        g_sh.head = head + (uint32_t)got;
        _filePos += (uint32_t)got;
    }
    if (got <= 0 || _filePos >= _fileEnd) {
        if (len > 0 && got <= 0) g_io.readFailed = true;
        _eof = true;
        FLAC_DMB(); // release: the last head before eof
        g_sh.eof = true;
    }
}

void FlacPlayer::startRing() {
    g_io.direct = false;
    g_io.ringStart = g_io.pos;
    _filePos = g_io.base + g_io.pos;
    _eof = _filePos >= _fileEnd || !_file.seekSet(_filePos);
    g_sh.head = g_sh.tail = 0;
    g_sh.eof = _eof;
    g_sh.ended = false;
    // Full, so core 1 can start a frame at once.
    for (uint32_t i = 0; i < g_c1.ringSize / READ_MIN + 2 && !_eof; i++) fillRing();
}

void FlacPlayer::stopCore1() {
    g_sh.run = false;
    FLAC_DMB(); // run cleared before inService is read, see enter()
    while (g_sh.inService) {
        // at most one frame's decode
    }
    FLAC_DMB(); // acquire: core 1's writes before it left
}

void FlacPlayer::restartClock(uint32_t ms) {
    _startMs = ms;
    _ringBase = Synth::wavStreamWritten();
    _growAt = 0xFFFFFFFFu;
    if (!_totalFrames) {
        // An estimate: update() grows it once the clock gets there.
        uint32_t left = _totalMs > ms ? _totalMs - ms : 0;
        _growAt = (uint32_t)(((uint64_t)left * SAMPLE_RATE + 999) / 1000);
    }
}

bool FlacPlayer::load(const char* path) { return open(path, false); }

bool FlacPlayer::openOffline(const char* path) { return open(path, true); }

bool FlacPlayer::open(const char* path, bool offline) {
    close();
    _offline = offline;
    _error[0] = '\0';
    hashPath(path);
    _totalMs = _rate = _maxFrame = 0;
    _totalFrames = 0;
    _hideSeekTable = false;
    _arenaUsed = _ringBytes = 0;
    g_io = Io{};
    if (!sdOpenPath(_file, path, O_RDONLY)) return fail("could not open file");
    Info in;
    const char* err = readInfo(_file, in);
    if (err) return fail(err);
    _rate = in.rate;
    _channels = in.channels;
    _bits = in.bits;
    _maxBlock = in.maxBlock;
    _maxFrame = in.maxFrame;
    _totalFrames = in.frames;
    _base = in.base;
    _audioStart = in.audioStart;
    _fileEnd = in.fileEnd;

    // The arena: the decoder as dr_flac sizes it, the ring, the output chunk.
    uint32_t frameBound = in.maxFrame ? in.maxFrame : (uint32_t)in.maxBlock * (in.channels * in.bits + 1) / 8 + FRAME_SLACK;
    uint32_t need = frameBound + DR_FLAC_BUFFER_SIZE;
    uint32_t ring = (need + RING_SLACK + 511) & ~511u;
    size_t pcmBytes = PCM_FRAMES * 2 * sizeof(int16_t);
    size_t block = (((size_t)in.maxBlock * 4 + 63) & ~(size_t)63) * in.channels;
    size_t decoder = sizeof(drflac) + block + 64 + 16;
    size_t seekTable = (size_t)(in.seekBytes / 18) * sizeof(drflac_seekpoint);
    if (decoder + ring + pcmBytes > SharedRamArena::SIZE) return fail("blocks too big for RAM");
    if (in.seekHeader && decoder + seekTable + ring + pcmBytes > SharedRamArena::SIZE) _hideSeekTable = true;
    uint8_t* arena = (uint8_t*)SharedRamArena::data();
    g_c1 = Core1{};
    g_c1.pcm = (int16_t*)(arena + SharedRamArena::SIZE - pcmBytes);
    g_c1.ring = (uint8_t*)g_c1.pcm - ring;
    g_c1.ringSize = ring;
    g_c1.need = need;
    g_c1.pcmChannels = _channels;
    g_c1.chunkFrames = (uint32_t)((uint64_t)(MAX_OUT_PER_CHUNK - 2) * in.rate / SAMPLE_RATE);
    if (g_c1.chunkFrames > PCM_FRAMES) g_c1.chunkFrames = PCM_FRAMES;
    g_pool.base = arena;
    g_pool.cap = (size_t)(g_c1.ring - arena);
    g_pool.used = 0;
    _ringBytes = ring;

    g_io.file = &_file;
    g_io.base = in.base;
    g_io.size = in.fileEnd - in.base;
    g_io.hideAt = _hideSeekTable ? in.seekHeader : UINT32_MAX;
    g_io.direct = true;
    if (!_file.seekSet(in.base)) return fail("can't read this file");
    drflac_allocation_callbacks cb = {nullptr, poolMalloc, nullptr, poolFree};
    drflac* f = drflac_open(onRead, onSeek, onTell, nullptr, &cb);
    if (!f) return fail(g_io.readFailed ? "can't read this file" : "not a FLAC file");
    g_c1.flac = f;
    // Seeks use the seek table or a binary search, each a few frame decodes;
    // never a walk through the whole file.
    f->_noBruteForceSeek = DRFLAC_TRUE;
    if (f->channels != _channels || f->bitsPerSample != _bits) return fail("not a FLAC file");
    _arenaUsed = g_pool.used + ring + pcmBytes;

    // The first chunk, which also tells a file that won't decode: bad frames
    // are skipped as in playback, up to MAX_FAILURES in a row.
    DecodeResult first;
    while ((first = decodeChunk()) == DECODED && !g_c1.pcmFrames) {}
    if (first != DECODED) return fail(g_io.readFailed ? "can't read this file" : "can't decode this file");

    if (_totalFrames) {
        _totalMs = (uint32_t)(_totalFrames * 1000 / _rate);
    } else {
        // About 60% of the PCM size, a common FLAC ratio.
        uint64_t bytesPerSec = (uint64_t)_rate * _channels * _bits * 6 / 80;
        _totalMs = (uint32_t)((uint64_t)(_fileEnd - _audioStart) * 1000 / (bytesPerSec ? bytesPerSec : 1));
    }
    g_c1.step = (uint32_t)(((uint64_t)_rate << 16) / SAMPLE_RATE);
    g_c1.frac = 0x20000; // the first two advance() steps fill last and next
    g_c1.outPerChunk = (uint32_t)(((uint64_t)g_c1.chunkFrames * SAMPLE_RATE + _rate - 1) / _rate) + 2;
    if (_offline) {
        _state = STATE_PAUSED; // core 1 stays out, decodeNext() does the work off the card
        return true;
    }
    startRing();
    Synth::wavStreamReset();
    _underrunBase = Synth::wavStreamUnderrunSamples();
    restartClock(0);
    _state = STATE_PAUSED;
    FLAC_DMB(); // release: everything above before core 1 may start
    g_sh.run = true;
    return true;
}

void FlacPlayer::seekTo(uint32_t targetMs) {
    if (_offline || !g_c1.flac) return;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED && _state != STATE_DONE) return;
    // Without the length in STREAMINFO the only bounded seek is to the start.
    if (!_totalFrames && targetMs) return;
    if (targetMs > _totalMs) targetMs = _totalMs;
    bool wasPlaying = _state == STATE_PLAYING;
    stopCore1();
    Synth::wavStreamReset();
    restartClock(targetMs);
    if (targetMs >= _totalMs) {
        _state = STATE_DONE; // core 1 stays out until the next seek or load
        return;
    }
    _state = wasPlaying ? STATE_PLAYING : STATE_PAUSED;
    // dr_flac has taken the ring's bytes up to tail; from here it reads the card.
    if (!g_io.direct) {
        g_io.pos = g_io.ringStart + g_sh.tail;
        g_io.direct = true;
    }
    uint64_t frame = (uint64_t)targetMs * _rate / 1000;
    if (!_file.seekSet(g_io.base + g_io.pos) || !drflac_seek_to_pcm_frame(g_c1.flac, frame)) {
        _state = STATE_DONE; // past what's there (a cut file) or a card error
        return;
    }
    g_c1.pcmFrames = g_c1.pcmPos = 0;
    g_c1.frac = 0x20000;
    g_c1.lastL = g_c1.lastR = g_c1.nextL = g_c1.nextR = 0;
    g_c1.starving = false;
    g_c1.failures = 0;
    startRing();
    Synth::wavStreamSetActive(_state == STATE_PLAYING);
    FLAC_DMB(); // release: everything above before core 1 may start
    g_sh.run = true;
}

bool FlacPlayer::ringDrained() const {
    if (!Synth::wavStreamIsActive() && Synth::wavStreamIsEnded()) return true;
    return (int32_t)(Synth::wavStreamPlayed() - Synth::wavStreamWritten()) >= 0;
}

void FlacPlayer::update() {
    if (_offline) return;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return;
    fillRing();
    if (_state != STATE_PLAYING) return;
    if (Synth::wavStreamPlayed() - _ringBase >= _growAt) {
        // The length was an estimate and the clock got there.
        uint32_t e = elapsedMs();
        if (e > _totalMs) _totalMs = e;
        _growAt = Synth::wavStreamPlayed() - _ringBase + SAMPLE_RATE / 10;
    }
    if (g_sh.ended) {
        FLAC_DMB(); // acquire: ended read before the stream state
        if (ringDrained()) _state = STATE_DONE;
    }
}

void FlacPlayer::play() {
    if (_offline) return;
    if (_state != STATE_PAUSED) return;
    _state = STATE_PLAYING;
    Synth::wavStreamSetActive(true);
}

void FlacPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void FlacPlayer::close() {
    stopCore1();
    if (_file) _file.close();
    // The pool's memory goes with the arena; dr_flac has nothing else to free.
    g_c1.flac = nullptr;
    g_io.file = nullptr;
    if (!_offline && _state != STATE_IDLE) Synth::wavStreamReset();
    _offline = false;
    _state = STATE_IDLE;
}

int FlacPlayer::decodeNext(const int16_t*& pcm, uint8_t& channels) {
    if (!_offline || _state != STATE_PAUSED || !g_c1.flac) return -1;
    // load()'s first chunk, then one per call.
    if (g_c1.pcmPos >= g_c1.pcmFrames && decodeChunk() != DECODED) return -1;
    pcm = g_c1.pcm + g_c1.pcmPos * g_c1.pcmChannels;
    channels = g_c1.pcmChannels;
    int n = (int)(g_c1.pcmFrames - g_c1.pcmPos);
    g_c1.pcmPos = g_c1.pcmFrames;
    return n;
}

void FlacPlayer::inputProgress(uint32_t& done, uint32_t& total) const {
    total = _fileEnd > _audioStart ? _fileEnd - _audioStart : 0;
    uint32_t at = g_io.base + g_io.pos;
    done = at > _audioStart ? at - _audioStart : 0;
    if (done > total) done = total;
}

uint32_t FlacPlayer::elapsedMs() const {
    if (_state == STATE_DONE) return _totalMs;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return 0;
    uint32_t played = Synth::wavStreamPlayed() - _ringBase;
    // played * 1000 / SAMPLE_RATE in 32 bits
    return _startMs + played / SAMPLE_RATE * 1000 + played % SAMPLE_RATE * 1000 / SAMPLE_RATE;
}

uint32_t FlacPlayer::underrunSamples() const { return Synth::wavStreamUnderrunSamples() - _underrunBase; }

#ifdef TRACKER_PROFILE
FlacPlayer::Profile FlacPlayer::profileTake() {
    Profile p = g_reads;
    g_reads = {};
    uint32_t e = g_worstEpoch & 0xFF;
    FLAC_DMB(); // release: the last call's slot reset before the new epoch
    g_worstEpoch = g_worstEpoch + 1;
    FLAC_DMB(); // new epoch written before busy is read
    while (g_worstBusy) {
        // core 1 is a few instructions from done
    }
    FLAC_DMB(); // acquire: the old slot after core 1 is out
    uint32_t w = g_worstSlot[e & 1];
    p.decodeWorstUs = (w >> 24) == e ? (w & 0xFFFFFF) : 0;
    g_worstSlot[e & 1] = ((e + 1) & 0xFF) << 24;
    uint32_t frames = g_frames, us = g_decodeUs, rejects = g_rejects, starved = g_starved;
    p.frames = frames - g_frames0;
    p.decodeUs = us - g_decodeUs0;
    p.rejects = rejects - g_rejects0;
    p.starved = starved - g_starved0;
    g_frames0 = frames;
    g_decodeUs0 = us;
    g_rejects0 = rejects;
    g_starved0 = starved;
    return p;
}
#endif

// In RAM: loop1() asks every sample, FLAC or not.
#ifdef ARDUINO_ARCH_RP2040
__attribute__((section(".time_critical.flac_core1Active")))
#endif
bool FlacPlayer::core1Active() { return g_sh.run; }

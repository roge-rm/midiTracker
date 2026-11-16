#include "mp3_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include "shared_ram_arena.h"
#include <string.h>
#include "mp3dec.h"

namespace {

// Arena layout: decoder state, then the input ring and its mirror, then one
// decoded frame.
//
// The input ring is a single-producer (core 0, fillRing()), single-consumer
// (core 1, decodeOne()) byte ring. head and tail are byte counts that only
// go up: head is written by core 0 only, tail by core 1 only. The first
// MIRROR bytes are copied again after the ring's end, so core 1 can hand
// the decoder any MIRROR bytes from tail in one piece. MIRROR is more than
// the largest layer III frame (1441 bytes, 320 kbps at 32 kHz).
// - core 0: reads tail, dmb, writes the bytes (and the mirror), dmb, head;
// - core 1: reads eof, dmb, head, dmb, reads the bytes, dmb, then tail.
// load() scans for the first frame in the ring's bytes as a plain linear
// buffer of SCAN_SIZE, before core 1 is let in.
const uint32_t RING = 8192; // power of two
const uint32_t MIRROR = 2048;
const uint32_t MIN_AVAIL = 1600; // core 1 decodes only with this much in one piece, until eof
const uint32_t READ_MIN = 2048;  // core 0 reads only when this much is free
const uint32_t SCAN_SIZE = 4608;
const uint32_t PCM_SAMPLES = 1152 * 2;
static_assert(SCAN_SIZE <= RING + MIRROR, "scan buffer is the ring's bytes");

#ifdef ARDUINO_ARCH_RP2040
#define MP3_DMB() __dmb()
#else
#define MP3_DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
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

// Core 1's decoder state. Core 0 sets it up in load() before it sets
// g_sh.run, and doesn't touch it again until stopCore1() returns.
struct Core1 {
    void* dec;
    uint8_t* ring;
    int16_t* pcm;
    uint32_t pcmFrames, pcmPos, outPerFrame;
    uint8_t pcmChannels;
    // Linear resampler, as WavPlayer's.
    uint32_t step, frac;
    int16_t lastL, lastR, nextL, nextR;
    bool starving;
};
Core1 g_c1;

#ifdef TRACKER_PROFILE
// Written by core 1 only, they only go up.
volatile uint32_t g_frames = 0, g_decodeUs = 0, g_rejects = 0, g_starved = 0;
// The worst decode per window. Core 0 starts a window by bumping
// g_worstEpoch. Core 1 keeps the window's worst in g_worstSlot[epoch & 1]
// with the epoch in the top 8 bits and the time in the low 24, and holds
// g_worstBusy while it reads the epoch and writes the slot:
// - core 1: busy, dmb, epoch, slot, dmb, clear busy;
// - core 0: epoch + 1, dmb, wait while busy, dmb, then the old slot.
// After that wait core 1 can't write the old slot again, and a slot whose
// tag isn't the old epoch had no decodes in that window.
volatile uint32_t g_worstEpoch = 0, g_worstSlot[2] = {0, 0};
volatile bool g_worstBusy = false;
Mp3Player::Profile g_reads; // core 0 only
uint32_t g_frames0 = 0, g_decodeUs0 = 0, g_rejects0 = 0, g_starved0 = 0;
#define PROF1(x) x

void noteDecode(uint32_t us) {
    if (us > 0xFFFFFF) us = 0xFFFFFF;
    g_worstBusy = true;
    MP3_DMB(); // busy written before the epoch is read, see above
    uint32_t e = g_worstEpoch & 0xFF;
    uint32_t w = g_worstSlot[e & 1];
    if ((w >> 24) != e || us > (w & 0xFFFFFF)) g_worstSlot[e & 1] = (e << 24) | us;
    MP3_DMB(); // slot written before busy clears
    g_worstBusy = false;
}
#else
#define PROF1(x)
#endif

size_t poolBytes() { return ((size_t)MP3PoolBytes() + 15) & ~(size_t)15; }

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Bytes in a layer III frame from its 4-byte header, 0 if it isn't one.
uint32_t frameBytes(const uint8_t* h, uint32_t& rate, uint32_t& kbps, uint8_t& version, bool& mono) {
    static const uint16_t RATES[3] = {44100, 48000, 32000};
    static const uint16_t KBPS1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const uint16_t KBPS2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    int ver = (h[1] >> 3) & 3; // 3 MPEG1, 2 MPEG2, 0 MPEG2.5
    int layer = (h[1] >> 1) & 3;
    int bi = h[2] >> 4, ri = (h[2] >> 2) & 3;
    if (ver == 1 || layer != 1 || bi == 0 || bi == 15 || ri == 3) return 0;
    rate = RATES[ri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    kbps = ver == 3 ? KBPS1[bi] : KBPS2[bi];
    version = ver == 3 ? 1 : ver == 2 ? 2 : 25;
    mono = (h[3] >> 6) == 3;
    uint32_t pad = (h[2] >> 1) & 1;
    return (ver == 3 ? 144000u : 72000u) * kbps / rate + pad;
}

} // namespace


// ---- core 1 only, while g_sh.run is set ----

namespace {

enum DecodeResult { DECODED, NEED_INPUT, SOURCE_END };

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
// when the decoded frame runs out first, with nothing taken.
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

// Resamples what's left of the decoded frame into the WAV stream, as far as
// it has room. true when the decoded frame is used up.
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

// The input ring as core 1 sees it: bytes ready, and how many of those
// start at p in one piece.
struct Window {
    uint32_t tail, avail, len;
    bool eof;
    uint8_t* p;
};

Window window() {
    Window w;
    w.eof = g_sh.eof;
    MP3_DMB(); // acquire: eof read before head, so eof covers every byte below head
    uint32_t head = g_sh.head;
    MP3_DMB(); // acquire: head read before the bytes it covers
    w.tail = g_sh.tail;
    w.avail = head - w.tail;
    uint32_t pos = w.tail & (RING - 1);
    w.len = RING + MIRROR - pos;
    if (w.len > w.avail) w.len = w.avail;
    w.p = g_c1.ring + pos;
    return w;
}

void consume(uint32_t tail, uint32_t n) {
    MP3_DMB(); // release: bytes read before core 0 may reuse them
    g_sh.tail = tail + n;
}

// True when decodeOne() won't come back with NEED_INPUT straight away.
bool inputReady() {
    Window w = window();
    return w.eof || w.len >= MIN_AVAIL;
}

// One frame into g_c1.pcm.
DecodeResult decodeOne() {
    g_c1.pcmFrames = g_c1.pcmPos = 0;
    int tries = 0;
    while (tries < 16) {
        Window w = window();
        if (!w.eof && w.len < MIN_AVAIL) return NEED_INPUT;
        if (w.avail < 4) return SOURCE_END;
        int off = MP3FindSyncWord(w.p, (int)w.len);
        if (off < 0) {
            consume(w.tail, w.len - 3);
            if (w.eof && w.len == w.avail) return SOURCE_END;
            tries++;
            continue;
        }
        if (off > 0) {
            consume(w.tail, (uint32_t)off); // then look again from the sync word
            continue;
        }
        tries++;
        unsigned char* ip = w.p;
        int left = (int)w.len;
        PROF1(uint32_t t0 = micros());
        int err = MP3Decode((HMP3Decoder)g_c1.dec, &ip, &left, g_c1.pcm, 0);
#ifdef TRACKER_PROFILE
        // Only frames that decode count and are timed, the rest are rejects.
        if (err == 0) {
            uint32_t d = micros() - t0;
            g_decodeUs = g_decodeUs + d;
            g_frames = g_frames + 1;
            noteDecode(d);
        } else {
            g_rejects = g_rejects + 1;
        }
#endif
        if (err == ERR_MP3_INVALID_FRAMEHEADER || err == ERR_MP3_INDATA_UNDERFLOW) {
            // Not a real frame here, or the last one is cut short.
            if (err == ERR_MP3_INDATA_UNDERFLOW && w.eof) return SOURCE_END;
            consume(w.tail, 1);
            continue;
        }
        consume(w.tail, w.len - (uint32_t)left);
        if (err) return DECODED; // no audio: bit reservoir not filled yet, or a bad frame
        MP3FrameInfo fi;
        MP3GetLastFrameInfo((HMP3Decoder)g_c1.dec, &fi);
        g_c1.pcmChannels = (uint8_t)fi.nChans;
        g_c1.pcmFrames = fi.nChans ? (uint32_t)fi.outputSamps / (uint32_t)fi.nChans : 0;
        return DECODED;
    }
    return DECODED;
}

// Entry and exit for core1Push()/core1Decode(). Core 1 sets inService, then
// checks run; core 0 clears run, then waits for inService to clear (see
// stopCore1()). The barriers between each store and the load after it mean
// at least one of them sees the other's store.
bool enter() {
    if (!g_sh.run) return false;
    g_sh.inService = true;
    MP3_DMB();
    if (g_sh.run) return true;
    g_sh.inService = false;
    return false;
}

void leave() {
    MP3_DMB(); // release: ring, stream and state written before core 0 may take them over
    g_sh.inService = false;
}

} // namespace

bool Mp3Player::core1Push() {
    if (!enter()) return false;
    bool want = false;
    if (!g_sh.ended && pushFrames()) {
        // A new frame only when all of its output fits in the stream.
        if (Synth::wavStreamFree() >= g_c1.outPerFrame) {
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

void Mp3Player::core1Decode() {
    if (!enter()) return;
    if (!g_sh.ended) {
        DecodeResult r = decodeOne();
        if (r == SOURCE_END) {
            Synth::wavStreamEnd();
            MP3_DMB(); // release: the stream's end flag before ours
            g_sh.ended = true;
        } else if (r == DECODED) {
            pushFrames();
        }
    }
    leave();
}

// ---- core 0 ----

size_t Mp3Player::decoderBytes() { return (size_t)MP3PoolBytes(); }
size_t Mp3Player::arenaBytes() { return poolBytes() + RING + MIRROR + PCM_SAMPLES * sizeof(int16_t); }

bool Mp3Player::fail(const char* msg) {
    strncpy(_error, msg, sizeof(_error) - 1);
    _error[sizeof(_error) - 1] = '\0';
    if (_file) _file.close();
    _state = STATE_ERROR;
    return false;
}

bool Mp3Player::fillScan() {
    uint32_t have = _inLen - _inPos;
    if (have >= MIN_AVAIL) return true;
    if (_eof) return false;
    uint8_t* buf = g_c1.ring;
    memmove(buf, buf + _inPos, have);
    _inPos = 0;
    _inLen = have;
    uint32_t len = SCAN_SIZE - have;
    uint32_t end = (_filePos + len) & ~511u;
    if (end > _filePos) len = end - _filePos; // end on a sector boundary
    if (len > _dataEnd - _filePos) len = _dataEnd - _filePos;
    int got = 0;
    if (len > 0) got = _file.read(buf + have, len);
    if (got <= 0) {
        _eof = true;
        return false;
    }
    _filePos += (uint32_t)got;
    _inLen += (uint32_t)got;
    if (_filePos >= _dataEnd) _eof = true;
    return _inLen - _inPos >= MIN_AVAIL;
}

void Mp3Player::fillRing() {
    if (_eof) return;
    uint32_t head = g_sh.head;
    uint32_t free = RING - (head - g_sh.tail);
    MP3_DMB(); // acquire: tail read before writing into the bytes it freed
    if (free < READ_MIN) return;
    uint32_t pos = head & (RING - 1);
    uint32_t len = RING - pos;
    if (len > free) len = free;
    uint32_t end = (_filePos + len) & ~511u;
    if (end > _filePos) len = end - _filePos; // end on a sector boundary
    if (len > _dataEnd - _filePos) len = _dataEnd - _filePos;
    uint8_t* buf = g_c1.ring;
    int got = 0;
    if (len > 0) {
#ifdef TRACKER_PROFILE
        uint32_t t0 = micros();
#endif
        got = _file.read(buf + pos, len);
#ifdef TRACKER_PROFILE
        g_reads.readUs += micros() - t0;
        g_reads.reads++;
        g_reads.readBytes += got > 0 ? (uint32_t)got : 0;
#endif
    }
    if (got > 0) {
        if (pos < MIRROR) {
            uint32_t m = MIRROR - pos;
            if (m > (uint32_t)got) m = (uint32_t)got;
            memcpy(buf + RING + pos, buf + pos, m);
        }
        MP3_DMB(); // release: bytes written before head covers them
        g_sh.head = head + (uint32_t)got;
        _filePos += (uint32_t)got;
    }
    if (got <= 0 || _filePos >= _dataEnd) {
        _eof = true;
        MP3_DMB(); // release: the last head before eof
        g_sh.eof = true;
    }
}

void Mp3Player::stopCore1() {
    g_sh.run = false;
    MP3_DMB(); // run cleared before inService is read, see enter()
    while (g_sh.inService) {
        // at most one decode, about 10 ms
    }
    MP3_DMB(); // acquire: core 1's writes before it left
}

bool Mp3Player::load(const char* path) {
    close();
    _error[0] = '\0';
    if (!sdOpenPath(_file, path, O_RDONLY)) return fail("could not open file");
    uint32_t size = (uint32_t)_file.fileSize();

    // ID3v2 tags (there can be more than one) at the start.
    uint32_t start = 0;
    for (int n = 0; n < 4; n++) {
        uint8_t h[10];
        if (!_file.seekSet(start) || _file.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) break;
        uint32_t len = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) | ((uint32_t)(h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        start += 10 + len + ((h[5] & 0x10) ? 10 : 0);
    }
    if (start >= size) return fail("not an MP3 file");
    _id3Bytes = start;

    // An ID3v1 tag at the end.
    _dataEnd = size;
    if (size >= start + 128) {
        uint8_t t[3];
        if (_file.seekSet(size - 128) && _file.read(t, 3) == 3 && memcmp(t, "TAG", 3) == 0) _dataEnd = size - 128;
    }
    if (!_file.seekSet(start)) return fail("not an MP3 file");
    _filePos = start;
    _inPos = _inLen = 0;
    _eof = false;

    if (arenaBytes() > SharedRamArena::SIZE) return fail("decoder doesn't fit");
    MP3SetPool(SharedRamArena::data(), (int)poolBytes());
    g_c1 = Core1{};
    g_c1.ring = (uint8_t*)SharedRamArena::data() + poolBytes();
    g_c1.pcm = (int16_t*)(g_c1.ring + RING + MIRROR);
    _dec = MP3InitDecoder();
    if (!_dec) return fail("decoder init failed");
    g_c1.dec = _dec;

    if (!findFirstFrame()) return fail(_error[0] ? _error : "not an MP3 file");

    // Stream from the first audio frame on, through the ring.
    uint32_t first = _filePos - (_inLen - _inPos);
    if (!_file.seekSet(first)) return fail("seek failed");
    _filePos = first;
    _eof = first >= _dataEnd;
    g_sh.head = g_sh.tail = 0;
    g_sh.eof = _eof;
    g_sh.ended = false;
    for (int i = 0; i < 4 && !_eof; i++) fillRing();

    g_c1.step = (uint32_t)(((uint64_t)_rate << 16) / SAMPLE_RATE);
    g_c1.frac = 0x20000; // the first two advance() steps fill last and next
    g_c1.outPerFrame = (uint32_t)(((uint64_t)_samplesPerFrame * SAMPLE_RATE + _rate - 1) / _rate) + 2;
    Synth::wavStreamReset();
    _underrunBase = Synth::wavStreamUnderrunSamples();
    _ringBase = Synth::wavStreamWritten();
    _state = STATE_PAUSED;
    MP3_DMB(); // release: everything above before core 1 may start
    g_sh.run = true;
    return true;
}

bool Mp3Player::findFirstFrame() {
    // Look up to 64 KB in for two frames in a row.
    uint32_t searched = 0;
    while (searched < 65536) {
        fillScan();
        uint8_t* p = g_c1.ring + _inPos;
        int avail = (int)(_inLen - _inPos);
        if (avail < 4) return false;
        int off = MP3FindSyncWord(p, avail);
        if (off < 0) {
            searched += avail - 3;
            _inPos = _inLen - 3;
            continue;
        }
        _inPos += off;
        searched += off;
        p += off;
        avail -= off;
        if (avail < 4) continue;
        uint32_t rate, kbps;
        uint8_t ver;
        bool mono;
        uint32_t len = frameBytes(p, rate, kbps, ver, mono);
        uint32_t rate2, kbps2;
        uint8_t ver2;
        bool mono2;
        if (len == 0 || (int)len + 4 > avail || frameBytes(p + len, rate2, kbps2, ver2, mono2) == 0 || rate2 != rate) {
            _inPos++;
            searched++;
            continue;
        }
        _rate = rate;
        _version = ver;
        _channels = mono ? 1 : 2;
        _samplesPerFrame = ver == 1 ? 1152 : 576;
        _bitrate = kbps * 1000;

        // A Xing/Info header sits after the side info, a VBRI 32 bytes after the header.
        uint32_t side = ver == 1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        const uint8_t* x = p + 4 + side;
        const uint8_t* v = p + 4 + 32;
        uint32_t frames = 0, bytes = 0;
        _vbrTag[0] = 0;
        if (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) {
            memcpy(_vbrTag, x, 4);
            uint32_t flags = be32(x + 4);
            const uint8_t* q = x + 8;
            if (flags & 1) { frames = be32(q); q += 4; }
            if (flags & 2) bytes = be32(q);
        } else if (!memcmp(v, "VBRI", 4)) {
            memcpy(_vbrTag, v, 4);
            bytes = be32(v + 10);
            frames = be32(v + 14);
        }
        _vbrTag[4] = 0;
        uint32_t audioStart = _filePos - (_inLen - _inPos);
        if (_vbrTag[0]) _inPos += len; // the header frame holds no audio
        if (frames) {
            _totalMs = (uint32_t)((uint64_t)frames * _samplesPerFrame * 1000 / _rate);
            if (bytes && _totalMs) _bitrate = (uint32_t)((uint64_t)bytes * 8000 / _totalMs);
        } else {
            if (_vbrTag[0] && (int)(len * 2 + 4) <= avail) {
                frameBytes(p + len, rate2, kbps2, ver2, mono2); // the first audio frame's bitrate
                _bitrate = kbps2 * 1000;
            }
            _totalMs = _bitrate ? (uint32_t)((uint64_t)(_dataEnd - audioStart) * 8000 / _bitrate) : 0;
        }
        return true;
    }
    return false;
}

bool Mp3Player::ringDrained() const {
    if (!Synth::wavStreamIsActive() && Synth::wavStreamIsEnded()) return true;
    return (int32_t)(Synth::wavStreamPlayed() - Synth::wavStreamWritten()) >= 0;
}

void Mp3Player::update() {
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return;
    fillRing();
    if (_state == STATE_PLAYING && g_sh.ended) {
        MP3_DMB(); // acquire: ended read before the stream state
        if (ringDrained()) _state = STATE_DONE;
    }
}

void Mp3Player::play() {
    if (_state != STATE_PAUSED) return;
    _state = STATE_PLAYING;
    Synth::wavStreamSetActive(true);
}

void Mp3Player::pause() {
    if (_state != STATE_PLAYING) return;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void Mp3Player::close() {
    if (_dec) stopCore1();
    if (_file) _file.close();
    if (_dec) MP3FreeDecoder((HMP3Decoder)_dec);
    _dec = nullptr;
    g_c1.dec = nullptr;
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

uint32_t Mp3Player::elapsedMs() const {
    if (_state == STATE_DONE) return _totalMs;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return 0;
    uint32_t played = Synth::wavStreamPlayed() - _ringBase;
    return (uint32_t)((uint64_t)played * 1000u / SAMPLE_RATE);
}

uint32_t Mp3Player::underrunSamples() const { return Synth::wavStreamUnderrunSamples() - _underrunBase; }

#ifdef TRACKER_PROFILE
Mp3Player::Profile Mp3Player::profileTake() {
    Profile p = g_reads;
    g_reads = {};
    uint32_t e = g_worstEpoch & 0xFF;
    MP3_DMB(); // release: the last call's slot reset before the new epoch
    g_worstEpoch = g_worstEpoch + 1;
    MP3_DMB(); // new epoch written before busy is read, see g_worstBusy
    while (g_worstBusy) {
        // core 1 is a few instructions from done
    }
    MP3_DMB(); // acquire: the old slot after core 1 is out
    uint32_t w = g_worstSlot[e & 1];
    p.decodeWorstUs = (w >> 24) == e ? (w & 0xFFFFFF) : 0;
    // Core 1 next uses this slot at epoch e + 2, so it's core 0's until
    // then; a tag of e + 1 marks it empty.
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

bool Mp3Player::core1Active() { return g_sh.run; }

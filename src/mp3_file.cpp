#include "mp3_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include "shared_ram_arena.h"
#include <string.h>
#include <stdio.h>
#include "mp3dec.h"

// What load() saw while looking for layer III frames, to say why it found
// none: layer I or II frames with the next frame's header where it should
// be, and free format layer III headers (no bitrate, so no length) that
// repeat the first one's version, rate and mode.
struct Mp3OtherFrames {
    uint32_t layer12 = 0, freeFormat = 0;
    uint8_t freeSig[3] = {0, 0, 0};
};

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
const uint32_t SEEK_CHUNK = 16384;      // a seek searches this much per update(), a few ms of card reads
const uint32_t SEEK_AHEAD = 256 * 1024; // then back towards the first frame
const uint32_t DECODER_DELAY = 529;     // samples, before the encoder delay in a LAME tag (ffmpeg's 528 + 1)
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
    volatile uint32_t lostMs = 0;         // core 1: length of the frames decoded without output since the last load or seek
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
    uint8_t skip; // decoded frames still to drop, see Mp3Player::seekTo()
    uint32_t lostSamples, rate; // for g_sh.lostMs: kept samples of the frames that gave no audio, and the file's rate
    // Gapless trim, in samples from the first audio frame's first: the
    // output keeps [keepFrom, keepTo) and anything from dropTo on (frames
    // past the count the LAME tag gave). srcPos is the next frame's first
    // sample, counting frames that gave no audio. After a seek srcPos is
    // only exact from the frame that starts at ring byte anchorTail on,
    // where it becomes anchorSrc; until then endExact is false and the end
    // isn't trimmed.
    uint32_t srcPos, spf, keepFrom, keepTo, dropTo;
    uint32_t anchorTail, anchorSrc;
    bool endExact;
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
uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }

// Bytes in a layer I or II frame, 0 if h isn't one.
uint32_t otherFrameBytes(const uint8_t* h) {
    static const uint16_t L1V1[15] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448};
    static const uint16_t L2V1[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
    static const uint16_t L1V2[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256};
    static const uint16_t L2V2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const uint16_t RATES[3] = {44100, 48000, 32000};
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    int bi = h[2] >> 4, ri = (h[2] >> 2) & 3;
    if (ver == 1 || layer < 2 || bi == 0 || bi == 15 || ri == 3) return 0;
    uint32_t rate = RATES[ri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    uint32_t pad = (h[2] >> 1) & 1;
    if (layer == 3) return (12000u * (ver == 3 ? L1V1[bi] : L1V2[bi]) / rate + pad) * 4;
    return 144000u * (ver == 3 ? L2V1[bi] : L2V2[bi]) / rate + pad;
}

void countOther(const uint8_t* h, int avail, Mp3OtherFrames& o) {
    uint32_t len = otherFrameBytes(h);
    if (len) {
        if ((int)len + 4 <= avail && otherFrameBytes(h + len) && ((h[1] ^ h[len + 1]) & 0x0E) == 0) o.layer12++;
        return;
    }
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return;
    int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3, ri = (h[2] >> 2) & 3;
    if (ver == 1 || layer != 1 || (h[2] >> 4) != 0 || ri == 3) return;
    uint8_t sig[3] = {(uint8_t)(h[1] & 0xFE), (uint8_t)(h[2] & 0xFC), (uint8_t)(h[3] & 0xC0)};
    if (o.freeFormat == 0) memcpy(o.freeSig, sig, 3);
    if (!memcmp(sig, o.freeSig, 3)) o.freeFormat++;
}

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

// The part [from, to) of the n samples from srcPos that the gapless trim
// keeps, and moves srcPos past them.
void trimNext(uint32_t n, uint32_t& from, uint32_t& to) {
    uint32_t p = g_c1.srcPos;
    g_c1.srcPos = p + n;
    from = 0;
    to = n;
    if (p < g_c1.keepFrom) from = g_c1.keepFrom - p < n ? g_c1.keepFrom - p : n;
    // dropTo is on a frame boundary, so the cut runs to the end of this frame.
    if (g_c1.endExact && p + n > g_c1.keepTo && p < g_c1.dropTo) to = g_c1.keepTo > p ? g_c1.keepTo - p : 0;
    if (to < from) to = from;
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
        if (w.tail == g_c1.anchorTail) {
            g_c1.srcPos = g_c1.anchorSrc;
            g_c1.endExact = true;
        }
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
        uint32_t used = w.len - (uint32_t)left;
        consume(w.tail, used);
        if (err || g_c1.skip) {
            // No audio: bit reservoir not filled yet, a bad frame, or one to drop.
            // Only reservoir misses and dropped frames held audio; a false sync
            // in junk didn't, so it doesn't move the clock.
            bool lost = !err || err == ERR_MP3_MAINDATA_UNDERFLOW;
            if (!err) g_c1.skip--;
            if (used && lost) {
                // Only the part the trim would have kept is missing from the clock.
                uint32_t from, to;
                trimNext(g_c1.spf, from, to);
                if (to > from) {
                    g_c1.lostSamples += to - from;
                    g_sh.lostMs = (uint32_t)((uint64_t)g_c1.lostSamples * 1000 / g_c1.rate);
                }
            }
            return DECODED;
        }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo((HMP3Decoder)g_c1.dec, &fi);
        g_c1.pcmChannels = (uint8_t)fi.nChans;
        uint32_t n = fi.nChans ? (uint32_t)fi.outputSamps / (uint32_t)fi.nChans : 0;
        trimNext(n, g_c1.pcmPos, g_c1.pcmFrames);
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

void Mp3Player::refuse(const char* msg) {
    close();
    fail(msg);
}

void Mp3Player::formatText(char* out, size_t size) const {
    snprintf(out, size, "%luK %s", (unsigned long)(_bitrate / 1000), _vbr ? "VBR" : "CBR");
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
        if (len > 0 && got <= 0) _readFailed = true;
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

void Mp3Player::restartClock(uint32_t ms) {
    _startMs = ms;
    _ringBase = Synth::wavStreamWritten();
    _growLost = ~0u; // update() works out _growAt again
}

bool Mp3Player::syncScan(uint32_t maxBytes, uint32_t want, Mp3OtherFrames* other) {
    uint32_t searched = 0;
    while (searched < maxBytes) {
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
        uint32_t rate, kbps, rate2, kbps2;
        uint8_t ver, ver2;
        bool mono, mono2;
        uint32_t len = frameBytes(p, rate, kbps, ver, mono);
        if (len == 0 && other) countOther(p, avail, *other);
        bool ok = len != 0 && (!want || rate == want) && (int)len + 4 <= avail;
        uint32_t len2 = ok ? frameBytes(p + len, rate2, kbps2, ver2, mono2) : 0;
        ok = len2 != 0 && rate2 == rate && ver2 == ver;
        // A third header when the buffer holds it.
        if (ok && (int)(len + len2 + 4) <= avail) ok = frameBytes(p + len + len2, rate2, kbps2, ver2, mono2) != 0 && rate2 == rate;
        if (ok) return true;
        _inPos++;
        searched++;
    }
    return false;
}

bool Mp3Player::findFirstFrame() {
    Mp3OtherFrames other;
    if (!syncScan(65536, 0, &other)) {
        // A stray header or two turns up in any file, a real one has many.
        if (other.layer12 >= 4 && other.layer12 >= other.freeFormat) return fail("layer I/II not supported");
        if (other.freeFormat >= 8) return fail("free format not supported");
        return fail("not an MP3 file");
    }
    uint8_t* p = g_c1.ring + _inPos;
    int avail = (int)(_inLen - _inPos);
    const uint8_t* bufEnd = p + avail;
    uint32_t rate, kbps;
    uint8_t ver;
    bool mono;
    uint32_t len = frameBytes(p, rate, kbps, ver, mono);
    _rate = rate;
    _version = ver;
    _channels = mono ? 1 : 2;
    _samplesPerFrame = ver == 1 ? 1152 : 576;
    _bitrate = kbps * 1000;
    _tagFrame = _filePos - (_inLen - _inPos);

    // A Xing/Info header sits after the side info, a VBRI 32 bytes after the header.
    uint32_t side = ver == 1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const uint8_t* x = p + 4 + side;
    const uint8_t* v = p + 4 + 32;
    uint32_t frames = 0;
    _vbrTag[0] = 0;
    if (x + 8 <= bufEnd && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
        memcpy(_vbrTag, x, 4);
        _vbr = x[0] == 'X';
        uint32_t flags = be32(x + 4);
        const uint8_t* q = x + 8;
        bool fits = true; // every field so far was in the buffer
        if (flags & 1) {
            if (q + 4 <= bufEnd) { frames = be32(q); q += 4; } else fits = false;
        }
        if (flags & 2) {
            if (fits && q + 4 <= bufEnd) { _tagBytes = be32(q); q += 4; } else fits = false;
        }
        if (fits && (flags & 4) && q + 100 <= bufEnd) {
            memcpy(_toc, q, 100);
            _hasToc = true;
            for (int i = 1; i < 100; i++) {
                if (_toc[i] < _toc[i - 1]) _hasToc = false; // not a usable table
            }
            q += 100;
        } else if (flags & 4) {
            fits = false;
        }
        if (flags & 8) q += 4; // quality
        // The LAME tag after them: the encoder's name in 9 bytes, 12 more,
        // then the encoder delay and padding, 12 bits each. ffmpeg's
        // libmp3lame writes one too.
        if (fits && q + 24 <= bufEnd &&
            (!memcmp(q, "LAME", 4) || !memcmp(q, "Lavf", 4) || !memcmp(q, "Lavc", 4))) {
            uint32_t v = ((uint32_t)q[21] << 16) | ((uint32_t)q[22] << 8) | q[23];
            _gapless = true;
            _encDelay = v >> 12;
            _encPad = v & 0xFFF;
        }
    } else if (v + 26 <= bufEnd && !memcmp(v, "VBRI", 4)) {
        memcpy(_vbrTag, v, 4);
        _vbr = true;
        _tagBytes = be32(v + 10);
        frames = be32(v + 14);
        uint32_t entries = be16(v + 18), scale = be16(v + 20), entryBytes = be16(v + 22), perEntry = be16(v + 24);
        if (entries && scale && entryBytes >= 1 && entryBytes <= 4 && perEntry) {
            _vbriTable = _tagFrame + 36 + 26;
            _vbriEntries = (uint16_t)entries;
            _vbriScale = (uint16_t)scale;
            _vbriEntryBytes = (uint16_t)entryBytes;
            _vbriFramesPerEntry = (uint16_t)perEntry;
        }
    }
    _vbrTag[4] = 0;
    if (_vbrTag[0]) _inPos += len; // the header frame holds no audio
    _audioStart = _filePos - (_inLen - _inPos);

    _xingFrames = frames;
    if (_gapless) _skipMs = (uint32_t)((uint64_t)(_encDelay + DECODER_DELAY) * 1000 / _rate);
    if (frames) {
        _tagMs = (uint32_t)((uint64_t)frames * _samplesPerFrame * 1000 / _rate);
        _totalMs = _tagMs;
        if (_gapless) {
            uint64_t all = (uint64_t)frames * _samplesPerFrame, cut = (uint64_t)_encDelay + _encPad;
            _totalMs = all > cut ? (uint32_t)((all - cut) * 1000 / _rate) : 0;
        }
        if (_tagBytes && _totalMs) _bitrate = (uint32_t)((uint64_t)_tagBytes * 8000 / _totalMs);
        // A cut file holds less than the header says.
        uint32_t have = _dataEnd - _tagFrame;
        if (_tagBytes > have + 4096) _totalMs = (uint32_t)((uint64_t)_totalMs * have / _tagBytes);
    } else {
        // The first frames in the buffer: their average bitrate, and VBR
        // when they differ.
        const uint8_t* f = g_c1.ring + _inPos;
        uint32_t sum = 0, n = 0, first = 0;
        while (n < 16 && f + 4 <= bufEnd) {
            uint32_t r, k;
            uint8_t vv;
            bool m;
            uint32_t l = frameBytes(f, r, k, vv, m);
            if (l == 0 || r != _rate) break;
            if (n == 0) first = k;
            else if (k != first) _vbr = true;
            sum += k;
            n++;
            f += l;
        }
        if (n) _bitrate = sum * 1000 / n;
        _totalMs = _bitrate ? (uint32_t)((uint64_t)(_dataEnd - _audioStart) * 8000 / _bitrate) : 0;
    }
    return true;
}

bool Mp3Player::testDecode() {
    // Top the scan buffer up so it holds a few frames.
    uint32_t have = _inLen - _inPos;
    if (!_eof && have < SCAN_SIZE) {
        memmove(g_c1.ring, g_c1.ring + _inPos, have);
        _inPos = 0;
        _inLen = have;
        uint32_t len = SCAN_SIZE - have;
        if (len > _dataEnd - _filePos) len = _dataEnd - _filePos;
        int got = len ? _file.read(g_c1.ring + have, len) : 0;
        if (got > 0) {
            _filePos += (uint32_t)got;
            _inLen += (uint32_t)got;
        }
    }
    unsigned char* p = g_c1.ring + _inPos;
    int left = (int)(_inLen - _inPos);
    int good = 0, bad = 0;
    for (int i = 0; i < 6 && good == 0 && bad < 3; i++) {
        int off = MP3FindSyncWord(p, left);
        if (off < 0) break;
        p += off;
        left -= off;
        unsigned char* before = p;
        int err = MP3Decode((HMP3Decoder)_dec, &p, &left, g_c1.pcm, 0);
        if (err == ERR_MP3_NONE) good++;
        else if (err == ERR_MP3_INDATA_UNDERFLOW) break; // end of the buffer
        else if (err != ERR_MP3_MAINDATA_UNDERFLOW) bad++; // that one is the bit reservoir filling
        if (p == before) { // not moved on, step past this sync word
            p++;
            left--;
        }
    }
    _dec = MP3InitDecoder(); // fresh state for playing
    g_c1.dec = _dec;
    return good > 0 || bad == 0;
}

uint32_t Mp3Player::seekOffset(uint32_t ms) {
    if (!_totalMs) return _audioStart;
    ms += _skipMs; // the tables count from the first frame, the clock from the first kept sample
    if (_hasToc && _tagMs) {
        // The TOC gives the byte position at each whole percent, in 256ths.
        uint32_t q = (uint32_t)((uint64_t)ms * 100000 / _tagMs); // thousandths of a percent
        if (q > 99999) q = 99999;
        uint32_t i = q / 1000, frac = q % 1000;
        uint32_t a = _toc[i], b = i < 99 ? _toc[i + 1] : 256;
        uint32_t x = a * 1000 + (b - a) * frac; // 256000ths of the stream
        uint32_t bytes = _tagBytes ? _tagBytes : _dataEnd - _tagFrame;
        return _tagFrame + (uint32_t)((uint64_t)x * bytes / 256000);
    }
    if (_vbriTable) {
        // Each entry is the bytes of the next _vbriFramesPerEntry frames.
        uint32_t target = (uint32_t)((uint64_t)ms * _rate / 1000 / _samplesPerFrame);
        uint32_t pos = 0, frames = 0;
        uint8_t buf[64];
        uint32_t per = _vbriEntryBytes, inBuf = 0, at = 0;
        if (!_file.seekSet(_vbriTable)) return _audioStart;
        for (uint32_t i = 0; i < _vbriEntries; i++) {
            if (at + per > inBuf) {
                uint32_t want = (_vbriEntries - i) * per;
                if (want > sizeof(buf) / per * per) want = sizeof(buf) / per * per;
                int got = _file.read(buf, want);
                if (got < (int)per) break;
                inBuf = (uint32_t)got;
                at = 0;
            }
            uint32_t e = 0;
            for (uint32_t k = 0; k < per; k++) e = (e << 8) | buf[at + k];
            at += per;
            uint32_t bytes = e * _vbriScale;
            if (frames + _vbriFramesPerEntry > target) {
                pos += (uint32_t)((uint64_t)bytes * (target - frames) / _vbriFramesPerEntry);
                return _tagFrame + pos;
            }
            pos += bytes;
            frames += _vbriFramesPerEntry;
        }
        return _tagFrame + pos;
    }
    // In proportion to the bytes, over the length with the trimmed start.
    return _audioStart + (uint32_t)((uint64_t)(_dataEnd - _audioStart) * ms / (_totalMs + _skipMs));
}

uint32_t Mp3Player::offsetMs(uint32_t pos) {
    if (pos <= _audioStart || !_totalMs) return 0;
    uint32_t ms;
    if (_hasToc && _tagMs) {
        uint32_t bytes = _tagBytes ? _tagBytes : _dataEnd - _tagFrame;
        uint32_t x = bytes ? (uint32_t)((uint64_t)(pos - _tagFrame) * 256000 / bytes) : 0; // 256000ths of the stream
        uint32_t i = 0;
        while (i < 99 && (uint32_t)_toc[i + 1] * 1000 <= x) i++;
        uint32_t a = _toc[i], b = i < 99 ? _toc[i + 1] : 256;
        uint32_t frac = b > a && x > a * 1000 ? (x - a * 1000) / (b - a) : 0;
        if (frac > 999) frac = 999;
        ms = (uint32_t)((uint64_t)(i * 1000 + frac) * _tagMs / 100000);
        ms = ms > _skipMs ? ms - _skipMs : 0;
    } else {
        // VBRI too: its table stays on the card, and this only places a seek that missed.
        ms = (uint32_t)((uint64_t)(pos - _audioStart) * (_totalMs + _skipMs) / (_dataEnd - _audioStart));
        ms = ms > _skipMs ? ms - _skipMs : 0;
    }
    return ms < _totalMs ? ms : _totalMs - 1;
}

bool Mp3Player::findSync(uint32_t pos, uint32_t maxBytes, uint32_t& found) {
    if (pos >= _dataEnd || !_file.seekSet(pos)) return false;
    _filePos = pos;
    _inPos = _inLen = 0;
    _eof = false;
    // Two frames in a row at this file's rate.
    if (!syncScan(maxBytes, _rate, nullptr)) return false;
    found = _filePos - (_inLen - _inPos);
    return true;
}

void Mp3Player::scanEnd() {
    _endScanned = true;
    _endCount = 0;
    if (!_gapless || !_xingFrames) return;
    if (_tagBytes > _dataEnd - _tagFrame + 4096) return; // cut short, the frame count is wrong
    // The last RING + MIRROR bytes, through the ring's bytes.
    uint32_t len = _dataEnd - _audioStart;
    if (len > RING + MIRROR) len = RING + MIRROR;
    uint32_t at = _dataEnd - len;
    uint8_t* buf = g_c1.ring;
    if (!_file.seekSet(at) || _file.read(buf, len) != (int)len) return;
    // Chains of frames at this file's rate and version: a chain of three or
    // more is real, and the search goes on after it ends so the last chain
    // wins. What's after that (APE tags, etc.) isn't frames.
    uint32_t pos[END_FRAMES];
    uint32_t count = 0, chainEnd = 0;
    for (uint32_t i = 0; i + 4 <= len; i++) {
        uint32_t p = i, n = 0, last[END_FRAMES];
        while (p + 4 <= len) {
            uint32_t r, k;
            uint8_t v;
            bool m;
            uint32_t l = frameBytes(buf + p, r, k, v, m);
            if (!l || r != _rate || v != _version) break;
            last[n % END_FRAMES] = p;
            n++;
            p += l;
        }
        if (n < 3) continue;
        count = n;
        chainEnd = p;
        memcpy(pos, last, sizeof(pos));
        i = p - 1; // past the chain
    }
    if (count < 3 || chainEnd > len) return; // none, or the last frame is cut short
    uint32_t keep = count < END_FRAMES ? count : END_FRAMES;
    if (keep > _xingFrames) return;
    for (uint32_t j = 0; j < keep; j++) _endPos[j] = at + pos[(count - keep + j) % END_FRAMES];
    _endCount = (uint8_t)keep;
}

bool Mp3Player::startStream(uint32_t first) {
    if (first >= _dataEnd || !_file.seekSet(first)) return false;
    _filePos = first;
    _inPos = _inLen = 0;
    _eof = false;
    g_sh.head = g_sh.tail = 0;
    g_sh.eof = _eof;
    g_sh.ended = false;
    for (int i = 0; i < 4 && !_eof; i++) fillRing();
    return true;
}

// The ID3v2 tags (there can be more than one) at the start: their bytes, or
// UINT32_MAX when they run past the end.
static uint32_t id3v2Bytes(FsFile& f, uint32_t size) {
    uint32_t start = 0;
    for (int n = 0; n < 4; n++) {
        uint8_t h[10];
        if (!f.seekSet(start) || f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) break;
        uint32_t len = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) | ((uint32_t)(h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        start += 10 + len + ((h[5] & 0x10) ? 10 : 0);
    }
    return start >= size ? UINT32_MAX : start;
}

bool Mp3Player::probe(const char* path, uint32_t& rate, uint8_t& channels) {
    FsFile f;
    if (!sdOpenPath(f, path, O_RDONLY)) return false;
    uint32_t size = (uint32_t)f.fileSize();
    uint32_t start = id3v2Bytes(f, size);
    bool found = false;
    uint8_t buf[512];
    // Windows of 512 bytes that overlap by 3, so a header across a window's end is seen.
    for (uint32_t pos = start; !found && start != UINT32_MAX && pos < size && pos - start < 65536; pos += sizeof(buf) - 3) {
        if (!f.seekSet(pos)) break;
        int got = f.read(buf, sizeof(buf));
        if (got < 4) break;
        for (int i = 0; i + 4 <= got && !found; i++) {
            uint32_t r, k, r2, k2;
            uint8_t v, v2;
            bool mono, m2;
            uint32_t len = frameBytes(buf + i, r, k, v, mono);
            if (!len) continue;
            // The next header, read from the card as it may be past the
            // window. Two in a row is all open() needs when its scan
            // buffer ends after them, so probe() takes that too.
            uint32_t at = pos + (uint32_t)i + len;
            uint8_t h[4];
            if (at + 4 <= size && f.seekSet(at) && f.read(h, 4) == 4 && frameBytes(h, r2, k2, v2, m2) && r2 == r && v2 == v) {
                rate = r;
                channels = mono ? 1 : 2;
                found = true;
            }
        }
    }
    f.close();
    return found;
}

bool Mp3Player::load(const char* path) { return open(path, false); }

bool Mp3Player::openOffline(const char* path) { return open(path, true); }

bool Mp3Player::open(const char* path, bool offline) {
    close();
    _offline = offline;
    _readFailed = false;
    _error[0] = '\0';
    hashPath(path);
    _totalMs = _bitrate = _rate = _tagBytes = _tagMs = _vbriTable = 0;
    _encDelay = _encPad = _xingFrames = _skipMs = 0;
    _vbr = _hasToc = _gapless = _endScanned = false;
    _endCount = 0;
    _vbrTag[0] = 0;
    if (!sdOpenPath(_file, path, O_RDONLY)) return fail("could not open file");
    uint32_t size = (uint32_t)_file.fileSize();

    uint32_t start = id3v2Bytes(_file, size);
    if (start == UINT32_MAX) return fail("not an MP3 file");
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

    if (!findFirstFrame()) return false;
    if (!testDecode()) return fail("can't decode this file");
    if (!startStream(_audioStart)) return fail("can't read this file");

    g_c1.rate = _rate;
    setTrim(0);
    g_sh.lostMs = 0;
    g_c1.step = (uint32_t)(((uint64_t)_rate << 16) / SAMPLE_RATE);
    g_c1.frac = 0x20000; // the first two advance() steps fill last and next
    g_c1.outPerFrame = (uint32_t)(((uint64_t)_samplesPerFrame * SAMPLE_RATE + _rate - 1) / _rate) + 2;
    if (_offline) {
        _state = STATE_PAUSED; // core 1 stays out, decodeNext() does the work
        return true;
    }
    Synth::wavStreamReset();
    _underrunBase = Synth::wavStreamUnderrunSamples();
    restartClock(0);
    _state = STATE_PAUSED;
    MP3_DMB(); // release: everything above before core 1 may start
    g_sh.run = true;
    return true;
}

void Mp3Player::seekTo(uint32_t targetMs) {
    if (_offline) return;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED && _state != STATE_DONE) return;
    if (targetMs > _totalMs) targetMs = _totalMs;
    bool wasPlaying = _state == STATE_PLAYING;
    stopCore1();
    Synth::wavStreamReset();
    _seekPending = false;
    restartClock(targetMs);
    if (targetMs >= _totalMs) {
        _state = STATE_DONE; // core 1 stays out until the next seek or load
        return;
    }
    _state = wasPlaying ? STATE_PLAYING : STATE_PAUSED;
    if (!_endScanned) scanEnd();
    uint32_t pos = seekOffset(targetMs);
    if (pos <= _audioStart) {
        startAt(_audioStart, 0);
        return;
    }
    _seekOrigin = _seekNext = pos;
    _seekBack = 0;
    _seekPending = true;
    seekStep();
}

void Mp3Player::seekStep() {
    uint32_t found;
    if (!_seekBack) {
        if (findSync(_seekNext, SEEK_CHUNK, found)) {
            // Bytes that aren't frames take no time, so the clock stays at the target.
            startAt(found, _startMs);
            return;
        }
        _seekNext += SEEK_CHUNK;
        if (_seekNext >= _dataEnd || _seekNext - _seekOrigin >= SEEK_AHEAD) _seekBack = 1;
        return;
    }
    // Back from the seek point, the nearest frames first. The first frame
    // is always there.
    uint32_t back = _seekBack * SEEK_CHUNK;
    if (_seekOrigin <= _audioStart + back) {
        startAt(_audioStart, 0);
        return;
    }
    uint32_t from = _seekOrigin - back;
    if (findSync(from, SEEK_CHUNK, found) && found < _seekOrigin) {
        startAt(found, offsetMs(found));
        return;
    }
    _seekBack++;
}

void Mp3Player::startAt(uint32_t pos, uint32_t ms) {
    _seekPending = false;
    restartClock(ms);
    if (!startStream(pos)) {
        _state = STATE_DONE; // the card failed; core 1 stays out
        return;
    }
    // A fresh decoder. Past the first frame, the frames before aren't
    // there, so the first ones fail on the missing bit reservoir and give
    // nothing, and the first that decodes is dropped, as its overlap with
    // the frame before is missing. g_sh.lostMs counts them for the clock.
    _dec = MP3InitDecoder();
    g_c1.dec = _dec;
    g_c1.pcmFrames = g_c1.pcmPos = 0;
    g_c1.frac = 0x20000;
    g_c1.lastL = g_c1.lastR = g_c1.nextL = g_c1.nextR = 0;
    g_c1.starving = false;
    g_c1.skip = pos <= _audioStart ? 0 : 1;
    g_c1.lostSamples = 0;
    setTrim(0);
    if (pos > _audioStart) {
        // Past the trimmed start. The clock only places pos to within a TOC
        // step, so the end trim waits for a frame counted from the end.
        g_c1.srcPos = g_c1.keepFrom;
        g_c1.endExact = false;
        uint32_t spf = _samplesPerFrame, first = g_c1.keepTo / spf; // the first frame the end trim cuts
        for (int i = 0; i < _endCount; i++) {
            if (_endPos[i] < pos) continue;
            uint32_t frame = _xingFrames - _endCount + i;
            // An anchor past the first cut frame comes too late, unless the stream starts on it.
            if (frame <= first || _endPos[i] == pos) {
                g_c1.anchorTail = _endPos[i] - pos;
                g_c1.anchorSrc = frame * spf;
            }
            break;
        }
    }
    g_sh.lostMs = 0;
    Synth::wavStreamSetActive(_state == STATE_PLAYING);
    MP3_DMB(); // release: everything above before core 1 may start
    g_sh.run = true;
}

void Mp3Player::setTrim(uint32_t srcPos) {
    g_c1.spf = _samplesPerFrame;
    g_c1.srcPos = srcPos;
    g_c1.keepFrom = 0;
    g_c1.keepTo = g_c1.dropTo = UINT32_MAX;
    g_c1.anchorTail = UINT32_MAX;
    g_c1.endExact = true;
    if (!_gapless) return;
    // As ffmpeg: the encoder delay plus the decoder's, and the padding
    // when the frame count is known.
    g_c1.keepFrom = _encDelay + DECODER_DELAY;
    if (_xingFrames) {
        uint64_t all = (uint64_t)_xingFrames * _samplesPerFrame;
        uint64_t to = all + DECODER_DELAY - _encPad;
        if (to < all && all < UINT32_MAX) {
            g_c1.keepTo = (uint32_t)to;
            g_c1.dropTo = (uint32_t)all;
        }
    }
}

bool Mp3Player::ringDrained() const {
    if (!Synth::wavStreamIsActive() && Synth::wavStreamIsEnded()) return true;
    return (int32_t)(Synth::wavStreamPlayed() - Synth::wavStreamWritten()) >= 0;
}

void Mp3Player::growTotal(uint32_t played, uint32_t lost) {
    uint32_t e = _startMs + lost + played / SAMPLE_RATE * 1000 + played % SAMPLE_RATE * 1000 / SAMPLE_RATE;
    if (e > _totalMs) _totalMs = e; // the estimate was short
    // The clock passes _totalMs once played * 1000 / SAMPLE_RATE > left.
    uint32_t left = _totalMs - _startMs - lost;
    uint64_t at = (((uint64_t)left + 1) * SAMPLE_RATE + 999) / 1000;
    _growAt = at > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)at; // past 27 h, played can't reach it
    _growLost = lost;
}

void Mp3Player::update() {
    if (_offline) return;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return;
    if (_seekPending) {
        seekStep();
        return;
    }
    fillRing();
    if (_state != STATE_PLAYING) return;
    // elapsedMs() > _totalMs, without its divides on every call.
    uint32_t played = Synth::wavStreamPlayed() - _ringBase;
    uint32_t lost = g_sh.lostMs;
    if (played >= _growAt || lost != _growLost) growTotal(played, lost);
    if (g_sh.ended) {
        MP3_DMB(); // acquire: ended read before the stream state
        if (ringDrained()) _state = STATE_DONE;
    }
}

void Mp3Player::play() {
    if (_offline) return;
    if (_state != STATE_PAUSED) return;
    _state = STATE_PLAYING;
    if (!_seekPending) Synth::wavStreamSetActive(true); // else when the seek finds its frame
}

void Mp3Player::pause() {
    if (_state != STATE_PLAYING) return;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void Mp3Player::close() {
    stopCore1();
    _seekPending = false;
    if (_file) _file.close();
    if (_dec) MP3FreeDecoder((HMP3Decoder)_dec);
    _dec = nullptr;
    g_c1.dec = nullptr;
    if (!_offline) Synth::wavStreamReset();
    _offline = false;
    _state = STATE_IDLE;
}

int Mp3Player::decodeNext(const int16_t*& pcm, uint8_t& channels) {
    if (!_offline || _state != STATE_PAUSED) return -1;
    // fillRing() reads up to the ring's end each time, so twice tops it up.
    fillRing();
    fillRing();
    DecodeResult r = decodeOne();
    if (r == NEED_INPUT) {
        fillRing();
        r = decodeOne();
    }
    if (r == SOURCE_END) return -1;
    if (r == NEED_INPUT) return _readFailed ? -1 : 0;
    // The part the gapless trim keeps.
    pcm = g_c1.pcm + g_c1.pcmPos * g_c1.pcmChannels;
    channels = g_c1.pcmChannels;
    return (int)(g_c1.pcmFrames - g_c1.pcmPos);
}

void Mp3Player::inputProgress(uint32_t& done, uint32_t& total) const {
    total = _dataEnd > _audioStart ? _dataEnd - _audioStart : 0;
    done = _filePos > _audioStart ? _filePos - _audioStart : 0;
    if (done > total) done = total;
}

uint32_t Mp3Player::elapsedMs() const {
    if (_state == STATE_DONE) return _totalMs;
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return 0;
    if (_seekPending) return _startMs;
    uint32_t played = Synth::wavStreamPlayed() - _ringBase;
    // played * 1000 / SAMPLE_RATE in 32 bits
    return _startMs + g_sh.lostMs + played / SAMPLE_RATE * 1000 + played % SAMPLE_RATE * 1000 / SAMPLE_RATE;
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

// In RAM: loop1() asks every sample, MP3 or not.
#ifdef ARDUINO_ARCH_RP2040
__attribute__((section(".time_critical.mp3_core1Active")))
#endif
bool Mp3Player::core1Active() { return g_sh.run; }

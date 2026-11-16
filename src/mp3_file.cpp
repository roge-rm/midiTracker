#include "mp3_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include "shared_ram_arena.h"
#include <string.h>
#include "mp3dec.h"

namespace {

// Arena layout: decoder state, then the read buffer, then one decoded frame.
// The read buffer keeps at least MIN_AVAIL bytes ahead while data is left,
// more than the largest layer III frame (1441 bytes, 320 kbps at 32 kHz).
const uint32_t IN_SIZE = 4608;
const uint32_t MIN_AVAIL = 1600;
const uint32_t PCM_SAMPLES = 1152 * 2;

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

size_t Mp3Player::decoderBytes() { return (size_t)MP3PoolBytes(); }
size_t Mp3Player::arenaBytes() { return poolBytes() + IN_SIZE + PCM_SAMPLES * sizeof(int16_t); }

uint8_t* Mp3Player::inBuf() const { return _in; }
int16_t* Mp3Player::pcmBuf() const { return _pcm; }

bool Mp3Player::fail(const char* msg) {
    strncpy(_error, msg, sizeof(_error) - 1);
    _error[sizeof(_error) - 1] = '\0';
    if (_file) _file.close();
    _state = STATE_ERROR;
    return false;
}

bool Mp3Player::fillInput() {
    uint32_t have = _inLen - _inPos;
    if (have >= MIN_AVAIL) return true;
    if (_eof) return false;
    uint8_t* buf = inBuf();
    memmove(buf, buf + _inPos, have);
    _inPos = 0;
    _inLen = have;
    uint32_t len = IN_SIZE - have;
    uint32_t end = (_filePos + len) & ~511u;
    if (end > _filePos) len = end - _filePos; // end on a sector boundary
    if (len > _dataEnd - _filePos) len = _dataEnd - _filePos;
    int got = 0;
    if (len > 0) {
#ifdef TRACKER_PROFILE
        uint32_t t0 = micros();
#endif
        got = _file.read(buf + have, len);
#ifdef TRACKER_PROFILE
        profile.readUs += micros() - t0;
        profile.reads++;
        profile.readBytes += got > 0 ? (uint32_t)got : 0;
#endif
    }
    if (got <= 0) {
        _eof = true;
        return false;
    }
    _filePos += (uint32_t)got;
    _inLen += (uint32_t)got;
    if (_filePos >= _dataEnd) _eof = true;
    return _inLen - _inPos >= MIN_AVAIL;
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
    _in = (uint8_t*)SharedRamArena::data() + poolBytes();
    _pcm = (int16_t*)(_in + IN_SIZE);
    _dec = MP3InitDecoder();
    if (!_dec) return fail("decoder init failed");

    if (!findFirstFrame()) return fail(_error[0] ? _error : "not an MP3 file");

    _pcmFrames = _pcmPos = 0;
    _sourceEnded = false;
    _resampleStep = (uint32_t)(((uint64_t)_rate << 16) / SAMPLE_RATE);
    _resampleFrac = 0x20000; // the first two advance() steps fill _last and _next
    _lastL = _lastR = _nextL = _nextR = 0;
    Synth::wavStreamReset();
    _underrunBase = Synth::wavStreamUnderrunSamples();
    _ringBase = Synth::wavStreamWritten();
    _state = STATE_PAUSED;
    return true;
}

bool Mp3Player::findFirstFrame() {
    // Look up to 64 KB in for two frames in a row.
    uint32_t searched = 0;
    while (searched < 65536) {
        fillInput();
        uint8_t* p = inBuf() + _inPos;
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

bool Mp3Player::decodeFrame() {
    _pcmFrames = _pcmPos = 0;
    for (int tries = 0; tries < 16; tries++) {
        fillInput();
        uint8_t* p = inBuf() + _inPos;
        int avail = (int)(_inLen - _inPos);
        if (avail < 4) return false;
        int off = MP3FindSyncWord(p, avail);
        if (off < 0) {
            _inPos = _inLen - 3;
            if (_eof) return false;
            continue;
        }
        _inPos += off;
        p += off;
        avail -= off;
        unsigned char* ip = p;
        int left = avail;
#ifdef TRACKER_PROFILE
        uint32_t t0 = micros();
#endif
        int err = MP3Decode((HMP3Decoder)_dec, &ip, &left, pcmBuf(), 0);
#ifdef TRACKER_PROFILE
        uint32_t d = micros() - t0;
        profile.decodeUs += d;
        if (d > profile.decodeWorstUs) profile.decodeWorstUs = d;
        profile.frames++;
#endif
        if (err == ERR_MP3_INVALID_FRAMEHEADER || err == ERR_MP3_INDATA_UNDERFLOW) {
            // Not a real frame here, or the last one is cut short.
            if (err == ERR_MP3_INDATA_UNDERFLOW && _eof) return false;
            _inPos++;
#ifdef TRACKER_PROFILE
            profile.errors++;
#endif
            continue;
        }
        _inPos += (uint32_t)(avail - left);
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) return true; // bit reservoir not filled yet, no audio
        if (err) {
#ifdef TRACKER_PROFILE
            profile.errors++;
#endif
            return true;
        }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo((HMP3Decoder)_dec, &fi);
        _pcmChannels = (uint8_t)fi.nChans;
        _pcmFrames = fi.nChans ? (uint32_t)fi.outputSamps / (uint32_t)fi.nChans : 0;
        return true;
    }
    return true;
}

bool Mp3Player::takeSrc(int16_t& l, int16_t& r) {
    if (_pcmPos >= _pcmFrames) return false;
    const int16_t* s = pcmBuf();
    if (_pcmChannels == 2) {
        l = s[_pcmPos * 2];
        r = s[_pcmPos * 2 + 1];
    } else {
        l = r = s[_pcmPos];
    }
    _pcmPos++;
    return true;
}

void Mp3Player::refill() {
    const size_t MAX_FRAMES_PER_TICK = 2048;
    const size_t BATCH = 32;
    int16_t out[BATCH * 2];
    size_t budget = MAX_FRAMES_PER_TICK;
    bool decoded = false;

    // Moves the resampler on to the source frame its position needs. false
    // when the decoded frame runs out first, with nothing taken.
    auto advance = [&]() {
        while (_resampleFrac >= 0x10000) {
            int16_t l, r;
            if (!takeSrc(l, r)) return false;
            _resampleFrac -= 0x10000;
            _lastL = _nextL;
            _lastR = _nextR;
            _nextL = l;
            _nextR = r;
        }
        return true;
    };

    while (budget > 0 && !_sourceEnded) {
        if (!advance()) {
            // At most one frame decoded per update(), and only when the ring has room.
            if (decoded || Synth::wavStreamFree() == 0) break;
            decoded = true;
            if (!decodeFrame()) {
                Synth::wavStreamEnd();
                _sourceEnded = true;
            }
            continue;
        }
        size_t n = Synth::wavStreamFree();
        if (n > BATCH) n = BATCH;
        if (n > budget) n = budget;
        if (n == 0) break;
        size_t k = 0;
        while (k < n) {
            int32_t diffL = (int32_t)_nextL - (int32_t)_lastL;
            int32_t diffR = (int32_t)_nextR - (int32_t)_lastR;
            int32_t frac15 = (int32_t)((_resampleFrac & 0xFFFF) >> 1);
            out[k * 2] = (int16_t)(_lastL + ((diffL * frac15) >> 15));
            out[k * 2 + 1] = (int16_t)(_lastR + ((diffR * frac15) >> 15));
            k++;
            _resampleFrac += _resampleStep;
            if (!advance()) break;
        }
        Synth::wavStreamWrite(out, k);
        budget -= k;
    }
}

bool Mp3Player::ringDrained() const {
    if (!Synth::wavStreamIsActive() && Synth::wavStreamIsEnded()) return true;
    return (int32_t)(Synth::wavStreamPlayed() - Synth::wavStreamWritten()) >= 0;
}

void Mp3Player::update() {
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return;
    refill();
    if (_state == STATE_PLAYING && _sourceEnded && ringDrained()) _state = STATE_DONE;
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
    if (_file) _file.close();
    if (_dec) MP3FreeDecoder((HMP3Decoder)_dec);
    _dec = nullptr;
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
Mp3Player::Profile Mp3Player::profile;
#endif

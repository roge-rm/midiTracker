#include "convert.h"
#include "sd_card.h"
#include "wav_file.h"
#include "mp3_file.h"
#include <Arduino.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

namespace {

const uint32_t OUT_RATE_44K = 44100;
const size_t HEADER_BYTES = 44;
// Past this a WAV's 32-bit sizes (and FAT32) run out.
const uint32_t MAX_DATA_BYTES = 0xFFFFFFFFu - HEADER_BYTES - 2 * Converter::OUT_BYTES;
// Frames per read() call in step(), on core 0's 4 KB stack.
const int READ_FRAMES = 32;

const char* const MSG_WRITE = "Card full or write failed";

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

// Characters FAT won't take in a name become '_'.
char safeChar(char c) {
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) return '_';
    return strchr("\\/:*?\"<>|", c) ? '_' : c;
}

} // namespace

const char* convertTargetLabel(ConvertTarget t) {
    switch (t) {
        case CONVERT_WAV:            return "WAV 16-bit";
        case CONVERT_WAV_44K_STEREO: return "WAV 16-bit 44.1k stereo";
        case CONVERT_WAV_44K_MONO:   return "WAV 16-bit 44.1k mono";
    }
    return "";
}

bool convertProbe(FileKind kind, const char* path, WavPlayer& wav, ConvertFormat& fmt) {
    fmt = ConvertFormat();
    if (kind != FILE_WAV) return false;
    bool ok = wav.load(path);
    if (ok) {
        fmt.rate = wav.sampleRate();
        fmt.channels = wav.channels();
        fmt.pcm16 = wav.isPcm16();
    }
    wav.close();
    return ok;
}

int convertTargets(FileKind kind, const ConvertFormat* fmt, ConvertTarget* out, int max) {
    int n = 0;
    auto add = [&](ConvertTarget t) {
        if (n < max) out[n++] = t;
    };
    switch (kind) {
        case FILE_MP3:
            add(CONVERT_WAV);
            add(CONVERT_WAV_44K_MONO);
            break;
        case FILE_WAV: {
            if (!fmt || !fmt->rate) break; // unreadable
            bool same44 = fmt->pcm16 && fmt->rate == OUT_RATE_44K;
            if (!(same44 && fmt->channels == 2)) add(CONVERT_WAV_44K_STEREO);
            if (!(same44 && fmt->channels == 1)) add(CONVERT_WAV_44K_MONO);
            break;
        }
        default:
            break;
    }
    return n;
}

bool isConvertTempName(const char* leaf) {
    size_t len = strlen(leaf);
    return len > 5 && leaf[0] == '.' && strcmp(leaf + len - 4, ".cvt") == 0;
}

// ---- sources ------------------------------------------------------------------

bool WavConvertSource::open(const char* path) {
    if (!_p.load(path)) return false;
    _p.rewindSource();
    return true;
}
const char* WavConvertSource::error() const { return _p.errorMessage(); }
uint32_t WavConvertSource::rate() const { return _p.sampleRate(); }
uint8_t WavConvertSource::channels() const { return _p.channels(); }

int WavConvertSource::read(int16_t* out, int max) {
    size_t n = _p.readSource(out, (size_t)max);
    if (n) return (int)n;
    return _p.readFailed() ? READ_FAILED : READ_END;
}

void WavConvertSource::progress(uint32_t& done, uint32_t& total) const {
    done = _p.sourceFramesRead();
    total = _p.sourceFrames();
}

void WavConvertSource::close() { _p.close(); }

bool Mp3ConvertSource::open(const char* path) {
    _pcm = nullptr;
    _frames = _pos = 0;
    return _p.openOffline(path);
}
const char* Mp3ConvertSource::error() const { return _p.errorMessage(); }
uint32_t Mp3ConvertSource::rate() const { return _p.sampleRate(); }
uint8_t Mp3ConvertSource::channels() const { return _p.channels(); }

int Mp3ConvertSource::read(int16_t* out, int max) {
    if (_pos >= _frames) {
        int n = _p.decodeNext(_pcm, _pcmChannels);
        if (n < 0) return _p.readFailed() ? READ_FAILED : READ_END;
        _frames = n;
        _pos = 0;
        if (n == 0) return 0; // a frame with no audio
    }
    int n = _frames - _pos;
    if (n > max) n = max;
    const int16_t* s = _pcm + (size_t)_pos * _pcmChannels;
    if (_pcmChannels == 2) {
        memcpy(out, s, (size_t)n * 4);
    } else {
        for (int i = 0; i < n; i++) out[i * 2] = out[i * 2 + 1] = s[i];
    }
    _pos += n;
    return n;
}

void Mp3ConvertSource::progress(uint32_t& done, uint32_t& total) const { _p.inputProgress(done, total); }

void Mp3ConvertSource::close() { _p.close(); }

// ---- converter ------------------------------------------------------------------

const char* Converter::outputName() const {
    const char* slash = strrchr(_outPath, '/');
    return slash ? slash + 1 : _outPath;
}

uint32_t Converter::elapsedMs() const {
    if (_state == STATE_IDLE) return 0;
    return (_state == STATE_RUNNING ? millis() : _endMs) - _startMs;
}

uint32_t Converter::leftMs() const {
    if (_state != STATE_RUNNING) return 0;
    uint32_t e = elapsedMs();
    if (_permille < 10 || e < 1000) return UINT32_MAX;
    return (uint32_t)((uint64_t)e * (uint32_t)(1000 - _permille) / (uint32_t)_permille);
}

int Converter::progressPermille() const { return _state == STATE_DONE ? 1000 : _permille; }

void Converter::closeSource() {
    if (_src) _src->close();
    _src = nullptr;
}

void Converter::fail(const char* msg) {
    if (_out) _out.close();
    if (_tmpPath[0]) sdRemove(_tmpPath); // a leftover goes when the folder is next listed
    closeSource();
    strncpy(_msg, msg, sizeof(_msg) - 1);
    _msg[sizeof(_msg) - 1] = '\0';
    _endMs = millis();
    _state = STATE_FAILED;
}

void Converter::cancel() {
    if (_state != STATE_RUNNING) return;
    fail("Cancelled");
    _state = STATE_CANCELLED;
}

void Converter::refuse(ConvertTarget target, const char* msg) {
    _target = target;
    _tmpPath[0] = _outPath[0] = '\0';
    _src = nullptr;
    _startMs = millis();
    _permille = 0;
    fail(msg);
}

bool Converter::pickNames(const char* srcPath, const char* displayName) {
    const char* slash = strrchr(srcPath, '/');
    int dirLen = slash ? (int)(slash - srcPath) + 1 : 0;
    char stem[41]; // short enough that the temp name stays under 64
    const char* dot = strrchr(displayName, '.');
    size_t len = dot && dot != displayName ? (size_t)(dot - displayName) : strlen(displayName);
    if (len >= sizeof(stem)) len = sizeof(stem) - 1;
    for (size_t i = 0; i < len; i++) stem[i] = safeChar(displayName[i]);
    while (len > 0 && (stem[len - 1] == ' ' || stem[len - 1] == '.')) len--;
    stem[len] = '\0';
    if (!len) strcpy(stem, "convert");
    for (int n = 1; n <= 999; n++) {
        char suffix[8] = "";
        if (n > 1) snprintf(suffix, sizeof(suffix), "-%d", n);
        int k = snprintf(_outPath, sizeof(_outPath), "%.*s%s%s.wav", dirLen, srcPath, stem, suffix);
        if (k <= 0 || (size_t)k >= sizeof(_outPath)) return false;
        if (sdExists(_outPath)) continue;
        k = snprintf(_tmpPath, sizeof(_tmpPath), "%.*s.%s.cvt", dirLen, srcPath, _outPath + dirLen);
        if (k <= 0 || (size_t)k >= sizeof(_tmpPath)) {
            _tmpPath[0] = '\0';
            return false;
        }
        return true;
    }
    return false;
}

void Converter::writeHeader(uint8_t* h, uint32_t dataBytes) const {
    uint16_t align = (uint16_t)(_outChannels * 2);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1); // PCM
    put16(h + 22, _outChannels);
    put32(h + 24, _outRate);
    put32(h + 28, _outRate * align);
    put16(h + 32, align);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
}

bool Converter::begin(ConvertSource& src, const char* srcPath, const char* displayName, ConvertTarget target) {
    _state = STATE_IDLE;
    _msg[0] = '\0';
    _tmpPath[0] = _outPath[0] = '\0';
    _target = target;
    _startMs = _endMs = millis();
    _dataBytes = 0;
    _bufLen = 0;
    _permille = 0;
    _src = nullptr;

    if (!pickNames(srcPath, displayName)) {
        fail("No free name for the new file");
        return false;
    }
    if (!src.open(srcPath)) {
        char m[sizeof(_msg)];
        snprintf(m, sizeof(m), "%s", src.error());
        m[0] = (char)toupper((unsigned char)m[0]); // the players' messages start in lower case
        src.close();
        _tmpPath[0] = '\0'; // nothing written
        fail(m[0] ? m : "Can't read this file");
        return false;
    }
    _src = &src;

    uint32_t srcRate = src.rate();
    if (target == CONVERT_WAV) {
        _outRate = srcRate;
        _outChannels = src.channels() == 1 ? 1 : 2;
    } else {
        _outRate = OUT_RATE_44K;
        _outChannels = target == CONVERT_WAV_44K_MONO ? 1 : 2;
    }
    if (!srcRate) {
        _tmpPath[0] = '\0';
        fail("Can't read this file");
        return false;
    }
    _resample = srcRate != _outRate;
    uint64_t step = ((uint64_t)srcRate << 32) / _outRate;
    _stepInt = (uint32_t)(step >> 32);
    _stepFrac = (uint32_t)step;
    _frac = 0;
    _need = 3; // x[-1], x0, x1, x2 from the first three frames
    _taken = _pos0 = 0;
    _primed = false;

    // A leftover temp of the same name is from an earlier run.
    if (sdExists(_tmpPath)) sdRemove(_tmpPath);
    if (!sdOpenPath(_out, _tmpPath, O_RDWR | O_CREAT | O_TRUNC)) {
        _tmpPath[0] = '\0';
        fail(MSG_WRITE);
        return false;
    }
    // Sizes are filled in by finish().
    writeHeader(_buf, 0);
    _bufLen = HEADER_BYTES;
    _state = STATE_RUNNING;
    return true;
}

bool Converter::flush() {
    if (!_bufLen) return true;
    if (_out.write(_buf, _bufLen) != _bufLen) return false;
    _bufLen = 0;
    return true;
}

bool Converter::putFrame(int32_t l, int32_t r) {
    size_t bytes = _outChannels * 2u;
    // OUT_BYTES - HEADER_BYTES and OUT_BYTES are whole frames, so this
    // only flushes full buffers.
    if (_bufLen + bytes > OUT_BYTES && !flush()) return false;
    uint8_t* p = _buf + _bufLen;
    if (_outChannels == 1) {
        put16(p, (uint16_t)clamp16((l + r) >> 1));
    } else {
        put16(p, (uint16_t)clamp16(l));
        put16(p + 2, (uint16_t)clamp16(r));
    }
    _bufLen += bytes;
    _dataBytes += bytes;
    return true;
}

void Converter::shiftIn(int16_t l, int16_t r) {
    if (!_primed) {
        for (int i = 0; i < 4; i++) {
            _hist[i][0] = l;
            _hist[i][1] = r;
        }
        _primed = true;
    } else {
        memmove(_hist[0], _hist[1], sizeof(_hist[0]) * 3);
        _hist[3][0] = l;
        _hist[3][1] = r;
    }
}

// One output frame at the current position, then the position moves on.
bool Converter::emitOne() {
    // Hermite through x[-1], x0, x1, x2 at t = _frac, with the
    // coefficients doubled so they stay whole numbers. t is 16 bits.
    int64_t t = _frac >> 16;
    int32_t y[2];
    for (int c = 0; c < 2; c++) {
        int32_t xm = _hist[0][c], x0 = _hist[1][c], x1 = _hist[2][c], x2 = _hist[3][c];
        int64_t v = (x2 - xm) + 3 * (x0 - x1);
        v = ((v * t) >> 16) + (2 * xm - 5 * x0 + 4 * x1 - x2);
        v = ((v * t) >> 16) + (x1 - xm);
        v = ((v * t) >> 16) + 2 * x0;
        y[c] = (int32_t)((v + 1) >> 1);
    }
    if (!putFrame(y[0], y[1])) return false;
    uint64_t f = (uint64_t)_frac + _stepFrac;
    _frac = (uint32_t)f;
    _need = _stepInt + (uint32_t)(f >> 32);
    return true;
}

bool Converter::feed(const int16_t* in, int n) {
    if (!_resample) {
        for (int i = 0; i < n; i++) {
            if (!putFrame(in[i * 2], in[i * 2 + 1])) return false;
        }
        _taken += (uint32_t)n;
        return true;
    }
    for (int i = 0; i < n; i++) {
        shiftIn(in[i * 2], in[i * 2 + 1]);
        _taken++;
        if (_taken > 3) _pos0++;
        _need--;
        while (_need == 0) {
            if (!emitOne()) return false;
        }
    }
    return true;
}

// Past the source's end x1 and x2 repeat the last frame, until the
// position passes the last real frame.
bool Converter::drainEnd() {
    if (!_resample || _taken == 0) return true;
    uint32_t shifts = _taken;
    for (;;) {
        while (_need > 0) {
            shiftIn(_hist[3][0], _hist[3][1]);
            shifts++;
            if (shifts > 3) _pos0++;
            _need--;
        }
        if (_pos0 >= _taken) return true;
        if (!emitOne()) return false;
    }
}

void Converter::step(uint32_t budgetUs) {
    if (_state != STATE_RUNNING) return;
    int16_t in[READ_FRAMES * 2];
    uint32_t t0 = micros(), last = 0;
    for (;;) {
        uint32_t t1 = micros();
        int n = _src->read(in, READ_FRAMES);
        if (n == ConvertSource::READ_FAILED) {
            fail("Read failed");
            return;
        }
        if (n == ConvertSource::READ_END) {
            finish();
            return;
        }
        if (n > 0 && !feed(in, n)) {
            fail(MSG_WRITE);
            return;
        }
        if (_dataBytes > MAX_DATA_BYTES) {
            fail("Too long for a WAV");
            return;
        }
        uint32_t now = micros();
        last = now - t1;
        // Stops before a read like the last one would run past the budget.
        if (now - t0 + last >= budgetUs) break;
    }
    uint32_t done, total;
    _src->progress(done, total);
    _permille = total ? (int)((uint64_t)done * 1000 / total) : 0;
    if (_permille > 999) _permille = 999;
}

bool Converter::finish() {
    if (_state != STATE_RUNNING) return _state == STATE_DONE;
    if (!drainEnd()) {
        fail(MSG_WRITE);
        return false;
    }
    if (_dataBytes == 0) {
        fail("No audio in this file");
        return false;
    }
    uint8_t h[HEADER_BYTES];
    writeHeader(h, _dataBytes);
    if (!flush() || !_out.seekSet(0) || _out.write(h, HEADER_BYTES) != HEADER_BYTES || !_out.sync()) {
        fail(MSG_WRITE);
        return false;
    }
    _out.close();
    closeSource();
    // The name was free at begin(); take the next one if it isn't now.
    if (sdExists(_outPath)) {
        char tmp[sizeof(_tmpPath)], from[sizeof(_outPath)];
        strcpy(tmp, _tmpPath);
        strcpy(from, _outPath);
        bool named = pickNames(from, outputName());
        strcpy(_tmpPath, tmp); // still where the audio is
        if (!named) {
            fail("No free name for the new file");
            return false;
        }
    }
    if (!sdRename(_tmpPath, _outPath)) {
        fail("Rename failed");
        return false;
    }
    _tmpPath[0] = '\0';
    _permille = 1000;
    _endMs = millis();
    _state = STATE_DONE;
    return true;
}

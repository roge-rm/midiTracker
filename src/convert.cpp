#include "convert.h"
#include "sd_card.h"
#include "wav_file.h"
#include "mp3_file.h"
#include "flac_file.h"
#include <Arduino.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <new>

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

// The resampler's low-pass, see Converter: sinc(x) * Kaiser window, x in
// zero crossings, KERNEL_L entries per zero crossing up to KERNEL_ZC, in
// Q15, with zeros past the end for the interpolation.
constexpr int KERNEL_ZC = 32;
constexpr int KERNEL_L = 64;
constexpr int KERNEL_N = KERNEL_ZC * KERNEL_L + 2;
constexpr double KERNEL_BETA = 8.0; // about 80 dB down in the stop band
constexpr double KERNEL_CUTOFF = 0.46; // of the lower rate, so the stop band starts just under half of it

constexpr double PI_D = 3.14159265358979323846;

// sin(PI * j / KERNEL_L) for j in 0..2L, for the table below.
constexpr double sinPiFrac(int j) {
    double x = PI_D * (j % (2 * KERNEL_L)) / KERNEL_L; // 0..2pi
    double sign = 1;
    if (x > PI_D) { x -= PI_D; sign = -1; }
    if (x > PI_D / 2) x = PI_D - x;
    double term = x, sum = x;
    for (int n = 1; n < 12; n++) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sign * sum;
}

// Modified Bessel function of the first kind, order 0.
constexpr double besselI0(double x) {
    double term = 1, sum = 1;
    for (int k = 1; k < 40; k++) {
        term *= (x / 2) / k;
        sum += term * term;
    }
    return sum;
}

constexpr double sqrtD(double v) {
    if (v <= 0) return 0;
    double r = v > 1 ? v : 1;
    for (int i = 0; i < 40; i++) r = (r + v / r) / 2;
    return r;
}

struct Kernel {
    int16_t v[KERNEL_N];
};

constexpr Kernel makeKernel() {
    Kernel k{};
    double i0b = besselI0(KERNEL_BETA);
    for (int i = 0; i < KERNEL_ZC * KERNEL_L; i++) {
        double sinc = i == 0 ? 1.0 : sinPiFrac(i) / (PI_D * i / KERNEL_L);
        double r = (double)i / (KERNEL_ZC * KERNEL_L);
        double w = besselI0(KERNEL_BETA * sqrtD(1 - r * r)) / i0b;
        double q = sinc * w * 32767;
        k.v[i] = (int16_t)(q < 0 ? q - 0.5 : q + 0.5);
    }
    return k;
}

constexpr Kernel KERNEL = makeKernel();

// "44.1k", "48k", "22.05k".
void formatRate(uint32_t rate, char* out, size_t size) {
    uint32_t whole = rate / 1000, part = rate % 1000;
    if (!part) {
        snprintf(out, size, "%luk", (unsigned long)whole);
        return;
    }
    char frac[4] = {(char)('0' + part / 100), (char)('0' + part / 10 % 10), (char)('0' + part % 10), '\0'};
    for (int i = 2; i > 0 && frac[i] == '0'; i--) frac[i] = '\0';
    snprintf(out, size, "%lu.%sk", (unsigned long)whole, frac);
}

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
    if (kind == FILE_MID || kind == FILE_MOD || kind == FILE_S3M || kind == FILE_XM || kind == FILE_IT) {
        // Played at the synth's and trackers' own rate; a broken file shows when it opens.
        fmt.rate = OUT_RATE_44K;
        fmt.channels = 2;
        fmt.pcm16 = true;
        fmt.playedBy = kind == FILE_MID ? "synth" : "tracker";
        return true;
    }
    if (kind == FILE_MP3) {
        bool ok = Mp3Player::probe(path, fmt.rate, fmt.channels);
        fmt.pcm16 = true; // what it decodes to
        return ok;
    }
    if (kind == FILE_FLAC) {
        uint8_t bits = 0;
        bool ok = FlacPlayer::probe(path, fmt.rate, fmt.channels, bits);
        fmt.pcm16 = bits == 16;
        return ok;
    }
    if (kind != FILE_WAV && kind != FILE_AIFF) return false;
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
            if (!fmt || !fmt->rate) break; // not an MP3 the player can read
            add(CONVERT_WAV);
            // A 44.1k mono MP3 to WAV is already that.
            if (!(fmt->rate == OUT_RATE_44K && fmt->channels == 1)) add(CONVERT_WAV_44K_MONO);
            break;
        case FILE_WAV:
        case FILE_AIFF:
        case FILE_FLAC: {
            if (!fmt || !fmt->rate) break; // unreadable
            // An AIFF or FLAC is still worth converting when it's already 44.1k 16-bit, for the container.
            bool same44 = kind == FILE_WAV && fmt->pcm16 && fmt->rate == OUT_RATE_44K;
            if (!(same44 && fmt->channels == 2)) add(CONVERT_WAV_44K_STEREO);
            if (!(same44 && fmt->channels == 1)) add(CONVERT_WAV_44K_MONO);
            break;
        }
        case FILE_MID:
        case FILE_MOD:
        case FILE_S3M:
        case FILE_XM:
        case FILE_IT:
            // The synth and trackers play 44.1k stereo.
            add(CONVERT_WAV_44K_STEREO);
            add(CONVERT_WAV_44K_MONO);
            break;
        default:
            break;
    }
    return n;
}

void convertTargetText(ConvertTarget t, const ConvertFormat* fmt, char* title, size_t titleSize, char* note, size_t noteSize) {
    bool known = fmt && fmt->rate;
    char rate[12] = "";
    if (known) formatRate(fmt->rate, rate, sizeof(rate));
    switch (t) {
        case CONVERT_WAV:
            if (known) {
                snprintf(title, titleSize, "WAV %s %s", rate, fmt->channels == 1 ? "MONO" : "STEREO");
            } else {
                snprintf(title, titleSize, "WAV 16-BIT");
            }
            break;
        case CONVERT_WAV_44K_STEREO: snprintf(title, titleSize, "WAV 44.1k STEREO"); break;
        case CONVERT_WAV_44K_MONO:   snprintf(title, titleSize, "WAV 44.1k MONO"); break;
    }
    if (known && fmt->playedBy) {
        snprintf(note, noteSize, "16-bit \x7f played by the %s", fmt->playedBy);
    } else if (known) {
        snprintf(note, noteSize, "16-bit \x7f file is %s %s", rate, fmt->channels == 1 ? "mono" : "stereo");
    } else {
        snprintf(note, noteSize, "16-bit");
    }
}

bool isConvertTempName(const char* leaf) {
    size_t len = strlen(leaf);
    return len > 9 && leaf[0] == '.' && strcmp(leaf + len - 8, ".wav.cvt") == 0;
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

template <class P> bool DecoderConvertSource<P>::open(const char* path) {
    _pcm = nullptr;
    _frames = _pos = 0;
    return _p.openOffline(path);
}
template <class P> const char* DecoderConvertSource<P>::error() const { return _p.errorMessage(); }
template <class P> uint32_t DecoderConvertSource<P>::rate() const { return _p.sampleRate(); }
template <class P> uint8_t DecoderConvertSource<P>::channels() const { return _p.channels(); }

template <class P> int DecoderConvertSource<P>::read(int16_t* out, int max) {
    if (_pos >= _frames) {
        int n = _p.decodeNext(_pcm, _pcmChannels);
        if (n < 0) return _p.readFailed() ? READ_FAILED : READ_END;
        _frames = n;
        _pos = 0;
        if (n == 0) return 0; // a frame with no audio, or a FLAC frame that didn't decode
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

template <class P> void DecoderConvertSource<P>::progress(uint32_t& done, uint32_t& total) const { _p.inputProgress(done, total); }

template <class P> void DecoderConvertSource<P>::close() { _p.close(); }

template class DecoderConvertSource<Mp3Player>;
template class DecoderConvertSource<FlacPlayer>;

// ---- converter ------------------------------------------------------------------

Converter::~Converter() {
    if (_state == STATE_RUNNING) cancel();
    delete[] _ring;
}

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
    delete[] _ring;
    _ring = nullptr;
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

bool Converter::setStem(const char* srcPath, const char* displayName) {
    const char* slash = strrchr(srcPath, '/');
    _dirLen = slash ? (int)(slash - srcPath) + 1 : 0;
    _outPath[0] = '\0';
    if (_dirLen >= (int)sizeof(_outPath)) return false;
    memcpy(_outPath, srcPath, (size_t)_dirLen); // the folder, for pickNames()
    _outPath[_dirLen] = '\0';
    // Short enough that the temp name stays under 64.
    const char* dot = strrchr(displayName, '.');
    size_t len = dot && dot != displayName ? (size_t)(dot - displayName) : strlen(displayName);
    if (len >= sizeof(_stem)) len = sizeof(_stem) - 1;
    for (size_t i = 0; i < len; i++) _stem[i] = safeChar(displayName[i]);
    while (len > 0 && (_stem[len - 1] == ' ' || _stem[len - 1] == '.')) len--;
    _stem[len] = '\0';
    if (!len) strcpy(_stem, "convert");
    return true;
}

const char* Converter::pickNames() {
    char dir[sizeof(_outPath)];
    memcpy(dir, _outPath, (size_t)_dirLen);
    dir[_dirLen] = '\0';
    for (int n = 1; n <= 999; n++) {
        char suffix[8] = "";
        if (n > 1) snprintf(suffix, sizeof(suffix), "-%d", n);
        int k = snprintf(_outPath, sizeof(_outPath), "%s%s%s.wav", dir, _stem, suffix);
        int t = snprintf(_tmpPath, sizeof(_tmpPath), "%s.%s%s.wav.cvt", dir, _stem, suffix);
        if (k <= 0 || (size_t)k >= sizeof(_outPath) || t <= 0 || (size_t)t >= sizeof(_tmpPath)) {
            _tmpPath[0] = '\0';
            memcpy(_outPath, dir, (size_t)_dirLen + 1);
            return "Path too long";
        }
        if (!sdExists(_outPath)) return nullptr;
    }
    _tmpPath[0] = '\0';
    memcpy(_outPath, dir, (size_t)_dirLen + 1);
    return "No free name for the new file";
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
    delete[] _ring;
    _ring = nullptr;

    const char* nameErr = setStem(srcPath, displayName) ? pickNames() : "Path too long";
    if (nameErr) {
        fail(nameErr);
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
    if (_resample) {
        // Rounded up, so the last output never lands past the source's end.
        uint64_t step = (((uint64_t)srcRate << 32) + _outRate - 1) / _outRate;
        _stepInt = (uint32_t)(step >> 32);
        _stepFrac = (uint32_t)step;
        // The low-pass in source frames: cut off at KERNEL_CUTOFF of the
        // lower rate, so its zero crossings are 1 / (2 * fc) frames apart.
        uint32_t lower = srcRate < _outRate ? srcRate : _outRate;
        double fc = KERNEL_CUTOFF * lower / srcRate; // cycles per source frame
        _kStep = (uint32_t)(2 * fc * KERNEL_L * 65536 + 0.5);
        _gain = (uint32_t)(2 * fc * 65536 + 0.5);
        _taps = (int)(KERNEL_ZC / (2 * fc)) + 2;
        uint32_t r = 16;
        while (r < (uint32_t)(2 * _taps + 2)) r <<= 1;
        _ringMask = r - 1;
        size_t n = (size_t)r * 2 * _outChannels;
        _ring = new (std::nothrow) int16_t[n];
        if (!_ring) {
            _tmpPath[0] = '\0';
            fail("Not enough RAM to convert");
            return false;
        }
        memset(_ring, 0, n * sizeof(int16_t));
        // The first output sits on source frame 0, with _taps silent frames before it.
        _pad = (uint32_t)_taps;
        _pos0 = _next = _pad;
        _frac = 0;
    }
    _taken = 0;

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

// One output frame at the current position, then the position moves on.
// The low-pass is evaluated at each tap's distance d from the position:
// left of it d = frac + m, right of it m - frac, in kernel table steps.
bool Converter::emitOne() {
    const int16_t* k = KERNEL.v;
    const uint32_t end = (uint32_t)(KERNEL_ZC * KERNEL_L) << 16;
    uint32_t fk = (uint32_t)(((uint64_t)(_frac >> 16) * _kStep) >> 16);
    uint32_t slot = _pos0 & _ringMask, r = _ringMask + 1;
    // Each Q15 tap times a sample is under 2^30; shifted down 4 the sums
    // stay in 32 bits up to about a 400 kHz source, and the 4 bits lost are
    // far under the output's last bit.
    int32_t accL = 0, accR = 0;
    if (_outChannels == 1) {
        const int16_t* p = _ring + slot + r; // the copy, so it can go back r - 1 frames
        for (uint32_t idx = fk; idx < end; idx += _kStep, p--) {
            uint32_t i = idx >> 16;
            int32_t c = k[i] + (((k[i + 1] - k[i]) * (int32_t)((idx >> 1) & 0x7FFF)) >> 15);
            accL += (c * p[0]) >> 4;
        }
        p = _ring + slot + 1;
        for (uint32_t idx = _kStep - fk; idx < end; idx += _kStep, p++) {
            uint32_t i = idx >> 16;
            int32_t c = k[i] + (((k[i + 1] - k[i]) * (int32_t)((idx >> 1) & 0x7FFF)) >> 15);
            accL += (c * p[0]) >> 4;
        }
        accR = accL;
    } else {
        const int16_t* p = _ring + (slot + r) * 2;
        for (uint32_t idx = fk; idx < end; idx += _kStep, p -= 2) {
            uint32_t i = idx >> 16;
            int32_t c = k[i] + (((k[i + 1] - k[i]) * (int32_t)((idx >> 1) & 0x7FFF)) >> 15);
            accL += (c * p[0]) >> 4;
            accR += (c * p[1]) >> 4;
        }
        p = _ring + (slot + 1) * 2;
        for (uint32_t idx = _kStep - fk; idx < end; idx += _kStep, p += 2) {
            uint32_t i = idx >> 16;
            int32_t c = k[i] + (((k[i + 1] - k[i]) * (int32_t)((idx >> 1) & 0x7FFF)) >> 15);
            accL += (c * p[0]) >> 4;
            accR += (c * p[1]) >> 4;
        }
    }
    // Q11 sums times the 16.16 gain.
    int32_t yl = (int32_t)(((int64_t)accL * _gain + (1LL << 26)) >> 27);
    int32_t yr = (int32_t)(((int64_t)accR * _gain + (1LL << 26)) >> 27);
    if (!putFrame(yl, yr)) return false;
    uint64_t f = (uint64_t)_frac + _stepFrac;
    _frac = (uint32_t)f;
    _pos0 += _stepInt + (uint32_t)(f >> 32);
    return true;
}

bool Converter::pushIn(int16_t l, int16_t r) {
    uint32_t slot = _next & _ringMask, rs = _ringMask + 1;
    if (_outChannels == 1) {
        int16_t m = (int16_t)(((int32_t)l + r) >> 1);
        _ring[slot] = _ring[slot + rs] = m;
    } else {
        _ring[slot * 2] = _ring[(slot + rs) * 2] = l;
        _ring[slot * 2 + 1] = _ring[(slot + rs) * 2 + 1] = r;
    }
    _next++;
    // An output needs the frames up to _taps past its position.
    while (_pos0 + (uint32_t)_taps < _next) {
        if (!emitOne()) return false;
    }
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
        if (!pushIn(in[i * 2], in[i * 2 + 1])) return false;
    }
    _taken += (uint32_t)n;
    return true;
}

// Silence past the source's end, until every output whose position is
// before the end is out.
bool Converter::drainEnd() {
    if (!_resample || _taken == 0) return true;
    uint32_t endPos = _pad + _taken;
    while (_pos0 < endPos) {
        uint32_t slot = _next & _ringMask, rs = _ringMask + 1;
        if (_outChannels == 1) {
            _ring[slot] = _ring[slot + rs] = 0;
        } else {
            _ring[slot * 2] = _ring[slot * 2 + 1] = 0;
            _ring[(slot + rs) * 2] = _ring[(slot + rs) * 2 + 1] = 0;
        }
        _next++;
        while (_pos0 < endPos && _pos0 + (uint32_t)_taps < _next) {
            if (!emitOne()) return false;
        }
    }
    return true;
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
    // The name was free at begin(); take the next free one from the same
    // stem if it isn't now. The audio stays in the temp file begin() named.
    if (sdExists(_outPath)) {
        char tmp[sizeof(_tmpPath)];
        strcpy(tmp, _tmpPath);
        const char* err = pickNames();
        strcpy(_tmpPath, tmp);
        if (err) {
            fail(err);
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

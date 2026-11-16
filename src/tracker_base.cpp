#include "tracker_base.h"
#include "synth.h"
#include "sd_card.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <stdlib.h>

namespace {

// Soft knee above 24576, hard ceiling at 32000, same shape as Synth's
// softLimit() in synth.cpp.
inline int32_t softClampMix(int32_t mix) {
    int32_t sign = (mix < 0) ? -1 : 1;
    int32_t mag = mix * sign;
    if (mag > 24576) mag = 24576 + (mag - 24576) / 4;
    if (mag > 32000) mag = 32000;
    return mag * sign;
}

} // namespace

#ifdef TRACKER_PROFILE
TrackerPlayer::Profile TrackerPlayer::profile = {};
#endif

void TrackerPlayer::attachVoices(TrackerVoice* first, size_t stride, int count, int16_t* chunkStore, uint32_t chunkFrames) {
    _voices = first;
    _voiceStride = stride;
    _voiceCount = count;
    for (int i = 0; i < count; i++) voiceAt(i).chunkBuf = chunkStore + (size_t)i * chunkFrames;
}

bool TrackerPlayer::loadFail(const char* msg) {
    if (_file) _file.close();
    if (_patternFile) _patternFile.close();
    strncpy(_error, msg, sizeof(_error) - 1);
    _error[sizeof(_error) - 1] = '\0';
    _state = STATE_ERROR;
    return false;
}

void TrackerPlayer::resetSequencer(uint8_t speed, uint16_t tempo) {
    _speed = speed;
    _tempo = tempo;
    _orderPos = 0;
    // -1 so the first advanceRow() lands on row 0.
    _row = -1;
    // The first update() runs a tick at once and it ends the row cycle,
    // so row 0 plays at time 0.
    _tick = _speed - 1;
    _samplesUntilNextTick = 0;
    _patternDelayRepeatsLeft = 0;
    _patternBreakPending = false;
    _positionJumpPending = false;
    _elapsedMsFrozen = 0;
    _lastResumeMicros = micros();
    _state = STATE_PAUSED;
}

void TrackerPlayer::close() {
    _sampleFiles[0].f = _sampleFiles[1].f = nullptr;
    if (_file) _file.close();
    if (_patternFile) _patternFile.close();
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

void TrackerPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _lastResumeMicros = micros();
    _state = STATE_PLAYING;
    // Fill the ring before the consumer starts pulling from it, so the
    // first SD reads for row 0 can't cause a gap. update() stops once full.
    for (int i = 0; i < 128 && Synth::wavStreamFree() >= BLOCK_FRAMES; i++) update();
    Synth::wavStreamSetActive(true);
}

void TrackerPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _elapsedMsFrozen += (micros() - _lastResumeMicros) / 1000;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void TrackerPlayer::stop() {
    close();
}

uint32_t TrackerPlayer::elapsedMs() const {
    uint32_t extra = (_state == STATE_PLAYING) ? (micros() - _lastResumeMicros) / 1000 : 0;
    return _elapsedMsFrozen + extra;
}

uint32_t TrackerPlayer::ticksToSamples(uint16_t tempo) {
    if (tempo == 0) tempo = 125;
    return (uint32_t)((double)SAMPLE_RATE * 2.5 / (double)tempo);
}

uint32_t TrackerPlayer::stepForHz(float hz) {
    return limitStep((uint32_t)((hz / (float)SAMPLE_RATE) * 65536.0f));
}

void TrackerPlayer::stepPosition(int rowCount, int maxBreakRow) {
    if (_patternBreakPending || _positionJumpPending) {
        int nextOrderPos = _positionJumpPending ? _positionJumpTarget : (_orderPos + 1);
        int nextRow = _patternBreakPending ? _patternBreakRow : 0;
        if (nextRow < 0 || nextRow > maxBreakRow) nextRow = 0;
        _orderPos = nextOrderPos;
        _row = nextRow;
        _patternBreakPending = false;
        _positionJumpPending = false;
    } else {
        _row++;
        if (_row >= rowCount) {
            _row = 0;
            _orderPos++;
        }
    }
}

void TrackerPlayer::patternLoop(uint8_t& loopRow, uint8_t& loopCount, uint8_t val) {
    if (val == 0) {
        loopRow = (uint8_t)_row;
        return;
    }
    if (loopCount == 0) loopCount = val;
    else loopCount--;
    if (loopCount > 0) {
        // Jump back to the loop row within this pattern, not the next order.
        _patternBreakPending = true;
        _patternBreakRow = loopRow;
        _positionJumpPending = true;
        _positionJumpTarget = _orderPos;
    }
}

void TrackerPlayer::useSampleFile(FsFile& f) {
    int slot = -1;
    for (int i = 0; i < 2; i++) if (_sampleFiles[i].f == &f) slot = i;
    for (int i = 0; i < 2 && slot < 0; i++) if (!_sampleFiles[i].f) slot = i;
    if (slot < 0) return;
    uint32_t first = 0, last = 0;
    SampleFile& sf = _sampleFiles[slot];
    sf.f = &f;
    sf.size = (uint32_t)f.fileSize();
    sf.firstSector = (sf.size > 0 && f.contiguousRange(&first, &last)) ? first : 0;
}

const uint8_t* __not_in_flash_func(TrackerPlayer::readSampleBytes)(FsFile& f, uint32_t pos, uint32_t len, int& got) {
#ifdef TRACKER_PROFILE
    uint32_t t0 = micros();
    profile.reads++;
#endif
    const SampleFile* sf = _sampleFiles[0].f == &f ? &_sampleFiles[0] : _sampleFiles[1].f == &f ? &_sampleFiles[1] : nullptr;
    const uint8_t* data = _rawScratch;
    got = 0;
    if (sf && sf->firstSector) {
        if (pos < sf->size) {
            if (len > sf->size - pos) len = sf->size - pos;
            uint32_t first = pos >> 9, count = ((pos + len - 1) >> 9) - first + 1;
            if (len && sd.card()->readSectors(sf->firstSector + first, _rawScratch, count)) {
                got = (int)len;
                data = _rawScratch + (pos & 511);
            }
        }
#ifdef TRACKER_PROFILE
        uint32_t d = micros() - t0;
        profile.readUs += d;
        int b = d < 200 ? 0 : d < 500 ? 1 : d < 1000 ? 2 : d < 2000 ? 3 : d < 5000 ? 4 : 5;
        profile.hist[b]++;
        profile.histUs[b] += d;
#endif
        return data;
    }
    if (f.seekSet(pos)) {
#ifdef TRACKER_PROFILE
        uint32_t t1 = micros();
        profile.seekUs += t1 - t0;
        t0 = t1;
#endif
        int n = f.read(_rawScratch, len);
        got = n > 0 ? n : 0;
    }
#ifdef TRACKER_PROFILE
    profile.readUs += micros() - t0;
#endif
    return data;
}

void TrackerPlayer::startVoiceAt(TrackerVoice& v, uint32_t index) {
    if (!_formatGains) {
        // The note playing fades out in a tail, the new one ramps up.
        if (v.curL || v.curR) spawnTail(v);
        v.curL = v.curR = 0;
        v.rampLeft = 0;
    }
    v.samplePos16 = 0;
    v.srcIndex = index;
    v.havePair = false;
    if (v.smp && v.smp->length > 0) {
        v.lastSrcSample = readRawSample(v, index);
        v.nextSrcSample = readRawSample(v, index + 1);
        v.havePair = true;
    }
}

void TrackerPlayer::advanceTick() {
    tickStart();
    _tick++;
    if (_tick >= _speed) {
        _tick = 0;
        // A pattern delay holds the row for more full tick cycles: tick
        // effects keep running, notes don't retrigger.
        if (_patternDelayRepeatsLeft > 0) _patternDelayRepeatsLeft--;
        else advanceRow();
    } else {
        processTickEffects();
    }
    tickEnd();
}

void TrackerPlayer::attachTails(TrackerTail* tails, int count, int16_t* pool, uint32_t poolFrames) {
    _tails = tails;
    _maxTails = count;
    _tailPool = pool;
    _tailPoolFrames = poolFrames;
    _tailCount = 0;
    _tailPoolUsed = 0;
}

void __not_in_flash_func(TrackerPlayer::setRamp)(TrackerVoice& v, int32_t tgtL, int32_t tgtR) {
    int32_t tl = tgtL << 12, tr = tgtR << 12;
    int32_t dl = tl - v.curL, dr = tr - v.curR;
    // Magnitudes: a surround voice's right gain is negative.
    int len = (abs(tl) > abs(v.curL) || abs(tr) > abs(v.curR)) ? RAMP_UP : RAMP_DOWN;
    if ((dl < len && dl > -len) && (dr < len && dr > -len)) {
        v.curL = tl;
        v.curR = tr;
        v.rampLeft = 0;
        return;
    }
    v.stepL = dl / len;
    v.stepR = dr / len;
    v.rampLeft = (uint16_t)len;
}

// Copies the frames v needs for a RAMP_DOWN fade from its cache into the
// tail pool. The cache holds the sample v was playing (chunkSmp), even when
// the format has already pointed v at a new one.
void __not_in_flash_func(TrackerPlayer::spawnTail)(TrackerVoice& v) {
    if (_tailCount >= _maxTails || !v.chunkSmp || v.sampleStep16 == 0) return;
    uint32_t room = _tailPoolFrames - _tailPoolUsed;
    uint32_t need = (((v.samplePos16 & 0xFFFF) + v.sampleStep16 * (uint32_t)RAMP_DOWN) >> 16) + 2;
    if (need > room) need = room;
    if (need < 2) return;
    const TrackerSample& s = *v.chunkSmp;
    int16_t* buf = _tailPool + _tailPoolUsed;
    buf[0] = v.lastSrcSample;
    buf[1] = v.nextSrcSample;
    uint32_t got = 2;
    bool loop = v.looping && s.loopEnd > s.loopStart;
    uint32_t j = v.srcIndex + 1;
    if (loop && j >= s.loopEnd) j = s.loopStart + ((j - s.loopEnd) % (s.loopEnd - s.loopStart));
    while (got < need) {
        j++;
        if (loop && j >= s.loopEnd) j = s.loopStart;
        if (j >= s.length || j - v.chunkStart >= v.chunkLen) break;
        buf[got++] = v.chunkBuf[j - v.chunkStart];
    }
    TrackerTail& t = _tails[_tailCount++];
    _tailPoolUsed += got;
    t.buf = buf;
    t.len = (uint16_t)got;
    t.pos = v.samplePos16 & 0xFFFF;
    t.step = v.sampleStep16;
    t.curL = v.curL;
    t.curR = v.curR;
    t.stepL = -v.curL / RAMP_DOWN;
    t.stepR = -v.curR / RAMP_DOWN;
    t.left = RAMP_DOWN;
}

// After every tick (MOD, S3M, XM): ramps to voiceGains(), and fades
// out a voice the tick stopped.
void __not_in_flash_func(TrackerPlayer::applyGains)() {
    for (int i = 0; i < _voiceCount; i++) {
        TrackerVoice& v = voiceAt(i);
        if (!v.havePair || v.sampleStep16 == 0 || !v.smp) {
            if (v.curL || v.curR) spawnTail(v);
            v.curL = v.curR = 0;
            v.rampLeft = 0;
            continue;
        }
        int32_t l, r;
        voiceGains(v, l, r);
        setRamp(v, l, r);
    }
}

void __not_in_flash_func(TrackerPlayer::voiceGains)(const TrackerVoice& v, int32_t& l, int32_t& r) {
    int32_t vol = v.mixVolume > 64 ? 64 : v.mixVolume;
    l = vol * (255 - v.mixPan) * 2;
    r = vol * v.mixPan * 2;
}

#if defined(__not_in_flash)
#define TRACKER_MIX_RAM __not_in_flash("trkmix")
#else
#define TRACKER_MIX_RAM
#endif

// How far past chunkStart a voice's frame index can go with the frame
// after it still cached and before the loop end or sample end.
static inline __attribute__((always_inline)) uint32_t fastSpan(const TrackerVoice& v, const TrackerSample& smp, bool loops) {
    if (v.chunkSmp != v.smp) return 0;
    uint32_t cs = v.chunkStart;
    uint32_t lim = smp.length;
    if (loops && smp.loopEnd < lim) lim = smp.loopEnd;
    if (cs + v.chunkLen < lim) lim = cs + v.chunkLen;
    return lim > cs + 1 ? lim - cs - 1 : 0;
}

// Resamples up to n frames of v into out with linear interpolation and
// returns how many (fewer when the sample ends). While the next source
// frames are in the cache and before the loop end, they're read straight
// from it; anything else goes through readRawSample(). Kept apart from the
// gains so the loop fits the M0+'s few registers.
TRACKER_MIX_RAM uint32_t TrackerPlayer::resample(TrackerVoice& v, int32_t* out, uint32_t n) {
    const TrackerSample& smp = *v.smp;
    const bool loops = v.looping && smp.loopEnd > smp.loopStart;
    uint32_t pos = v.samplePos16, step = v.sampleStep16;
    int32_t last = v.lastSrcSample, next = v.nextSrcSample;
    const int16_t* buf = v.chunkBuf;
    // r is the frame index less chunkStart; frames r and r + 1 are cached
    // and need no wrap while r < fast.
    uint32_t r = v.srcIndex - v.chunkStart, fast = fastSpan(v, smp, loops);
    int32_t* o = out;
    int32_t* end = out + n;
    while (o < end) {
        // frac >> 1 keeps diff * frac inside int32 for any pair of int16s.
        *o++ = last + (((next - last) * (int32_t)((pos & 0xFFFF) >> 1)) >> 15);
        pos += step;
        if (pos < 0x10000) continue;
        uint32_t nr = r + (pos >> 16);
        if (nr < fast) {
            last = buf[nr];
            next = buf[nr + 1];
            r = nr;
            pos &= 0xFFFF;
            continue;
        }
        v.srcIndex = v.chunkStart + r;
        bool ended = false;
        while (pos >= 0x10000) {
            pos -= 0x10000;
            v.srcIndex++;
            if (loops && v.srcIndex >= smp.loopEnd) {
                v.srcIndex = smp.loopStart + ((v.srcIndex - smp.loopEnd) % (smp.loopEnd - smp.loopStart));
            } else if (!v.looping && v.srcIndex >= smp.length) {
                v.havePair = false;
                ended = true;
                break;
            }
            last = next;
            next = readRawSample(v, v.srcIndex + 1);
        }
        r = v.srcIndex - v.chunkStart;
        if (ended) break;
        fast = fastSpan(v, smp, loops);
    }
    v.srcIndex = v.chunkStart + r;
    v.samplePos16 = pos;
    v.lastSrcSample = (int16_t)last;
    v.nextSrcSample = (int16_t)next;
    return (uint32_t)(o - out);
}

// n frames of one voice added into acc: resampled, through the filter if
// FILTER, then the gains (ramping if RAMP). All 32-bit: the filter's Q24
// products are split into 12-bit halves.
template <bool FILTER, bool RAMP>
TRACKER_MIX_RAM void TrackerPlayer::mixVoice(TrackerVoice& v, TrackerFilter* f, int32_t* acc, uint32_t n) {
    int32_t tmp[BLOCK_FRAMES];
    uint32_t got = resample(v, tmp, n);
    if (FILTER) {
        int32_t y1 = f->y1, y2 = f->y2;
        const int32_t a0h = f->a0 >> 12, a0l = f->a0 & 4095, b0h = f->b0 >> 12, b0l = f->b0 & 4095;
        const int32_t b1h = f->b1 >> 12, b1l = f->b1 & 4095;
        for (uint32_t k = 0; k < got; k++) {
            int32_t smpl = tmp[k];
            int32_t hi = smpl * a0h + y1 * b0h + y2 * b1h;
            int32_t lo = smpl * a0l + y1 * b0l + y2 * b1l;
            int32_t val = (hi + (lo >> 12) + 2048) >> 12;
            if (val > 65534) val = 65534;
            if (val < -65536) val = -65536;
            y2 = y1;
            y1 = val;
            tmp[k] = val;
        }
        f->y1 = y1;
        f->y2 = y2;
    }
    const int32_t* t = tmp;
    const int32_t* end = tmp + got;
    if (RAMP) {
        int32_t curL = v.curL, curR = v.curR;
        const int32_t stepL = v.stepL, stepR = v.stepR;
        while (t < end) {
            int32_t smpl = *t++;
            curL += stepL;
            curR += stepR;
            acc[0] += (smpl * (curL >> 12)) >> 15;
            acc[1] += (smpl * (curR >> 12)) >> 15;
            acc += 2;
        }
        v.curL = curL;
        v.curR = curR;
        v.rampLeft = (uint16_t)(v.rampLeft > got ? v.rampLeft - got : 0);
    } else {
        const int32_t gl = v.curL >> 12, gr = v.curR >> 12;
        while (t < end) {
            int32_t smpl = *t++;
            acc[0] += (smpl * gl) >> 15;
            acc[1] += (smpl * gr) >> 15;
            acc += 2;
        }
    }
    if (!v.havePair) { v.curL = v.curR = 0; v.rampLeft = 0; } // ran off the end, nothing to fade
}

void __not_in_flash_func(TrackerPlayer::mixVoiceBlock)(TrackerVoice& v, TrackerFilter* f, int32_t* acc, uint32_t n, bool stopAfterRamp) {
    uint32_t r = v.rampLeft < n ? v.rampLeft : n;
    if (r) {
        if (f) mixVoice<true, true>(v, f, acc, r);
        else mixVoice<false, true>(v, nullptr, acc, r);
    }
    if (r < n && v.havePair && !(stopAfterRamp && v.rampLeft == 0)) {
        if (f) mixVoice<true, false>(v, f, acc + 2 * r, n - r);
        else mixVoice<false, false>(v, nullptr, acc + 2 * r, n - r);
    }
}

void __not_in_flash_func(TrackerPlayer::mixTail)(TrackerTail& t, int32_t* acc, uint32_t n) {
    if (n > t.left) n = t.left;
    uint32_t pos = t.pos;
    int32_t curL = t.curL, curR = t.curR;
    uint32_t k = 0;
    for (; k < n; k++) {
        uint32_t i = pos >> 16;
        if (i + 1 >= t.len) { k = t.left; break; }
        int32_t a = t.buf[i], b = t.buf[i + 1];
        int32_t smpl = a + (((b - a) * (int32_t)((pos & 0xFFFF) >> 1)) >> 15);
        curL += t.stepL;
        curR += t.stepR;
        acc[2 * k] += (smpl * (curL >> 12)) >> 15;
        acc[2 * k + 1] += (smpl * (curR >> 12)) >> 15;
        pos += t.step;
    }
    t.pos = pos;
    t.curL = curL;
    t.curR = curR;
    t.left = (uint16_t)(t.left > k ? t.left - k : 0);
}

const int16_t* __not_in_flash_func(TrackerPlayer::mixBlock)(uint32_t n) {
    int32_t* acc = _mix.acc;
    memset(acc, 0, n * 2 * sizeof(int32_t));
    for (int i = 0; i < _voiceCount; i++) {
        TrackerVoice& v = voiceAt(i);
        if (!v.havePair || v.sampleStep16 == 0 || !v.smp) continue;
        mixVoiceBlock(v, nullptr, acc, n, false);
    }
    for (int i = 0; i < _tailCount; i++) if (_tails[i].left) mixTail(_tails[i], acc, n);
    // In place: out[i] only overwrites acc entries already read.
    for (uint32_t i = 0; i < n * 2; i++) _mix.out[i] = finishMix(acc[i], _globalVolume);
    return _mix.out;
}

int16_t __not_in_flash_func(TrackerPlayer::finishMix)(int32_t mix, uint8_t globalVolume) {
    mix = (mix * (int32_t)globalVolume) >> 6;
    mix += mix >> 2;
    return (int16_t)softClampMix(mix);
}

void __not_in_flash_func(TrackerPlayer::update)() {
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;
#ifdef TRACKER_PROFILE
    uint32_t profStart = micros();
#endif
    size_t produced = 0;
    while (produced < MAX_FRAMES_PER_UPDATE) {
        // Waits for room for a whole block: each block has a fixed cost per
        // voice, and update() is called far more often than a block plays.
        uint32_t space = (uint32_t)Synth::wavStreamFree();
        if (space < BLOCK_FRAMES) break;
        if (_samplesUntilNextTick == 0) {
            // Last tick's tails are done: a tick is longer than RAMP_DOWN.
            _tailCount = 0;
            _tailPoolUsed = 0;
            advanceTick();
            if (_state == STATE_DONE) break;
            if (!_formatGains) applyGains();
            _samplesUntilNextTick = tickLength(); // tempo may have just changed
        }
        uint32_t n = (uint32_t)(MAX_FRAMES_PER_UPDATE - produced);
        if (n > space) n = space;
        if (n > _samplesUntilNextTick) n = _samplesUntilNextTick;
        if (n > BLOCK_FRAMES) n = BLOCK_FRAMES;
        const int16_t* frames = mixBlock(n);
        uint32_t wrote = (uint32_t)Synth::wavStreamWrite(frames, n);
        _samplesUntilNextTick -= wrote;
        produced += wrote;
        if (wrote < n) break;
    }
#ifdef TRACKER_PROFILE
    if (produced) {
        uint32_t d = micros() - profStart;
        profile.updateUs += d;
        profile.frames += produced;
        if (d > profile.worstUs) profile.worstUs = d;
    }
#endif
}

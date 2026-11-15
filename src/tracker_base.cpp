#include "tracker_base.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>

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
    for (int i = 0; i < 128 && Synth::wavStreamFree() > 0; i++) update();
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

void TrackerPlayer::startVoiceAt(TrackerVoice& v, uint32_t index) {
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

inline __attribute__((always_inline)) int16_t TrackerPlayer::readVoiceSample(TrackerVoice& v) {
    const TrackerSample& s = *v.smp;
    uint16_t frac16 = (uint16_t)(v.samplePos16 & 0xFFFF);
    int32_t diff = (int32_t)v.nextSrcSample - (int32_t)v.lastSrcSample;
    // diff * frac16 overflows int32 once |diff| > 32767, so halve both sides
    // there (off by at most one step). 64-bit multiply would be a flash call.
    int32_t lerp = (diff >= -32768 && diff <= 32767)
        ? (diff * (int32_t)frac16) >> 16
        : ((diff >> 1) * (int32_t)frac16) >> 15;
    int16_t interpolated = (int16_t)(v.lastSrcSample + lerp);

    v.samplePos16 += v.sampleStep16;
    while (v.havePair && v.samplePos16 >= 0x10000) {
        v.samplePos16 -= 0x10000;
        v.srcIndex++;
        if (v.looping && s.loopEnd > s.loopStart && v.srcIndex >= s.loopEnd) {
            v.srcIndex = s.loopStart + ((v.srcIndex - s.loopEnd) % (s.loopEnd - s.loopStart));
        } else if (!v.looping && v.srcIndex >= s.length) {
            v.havePair = false; // ran off the end, silent until restarted
            break;
        }
        v.lastSrcSample = v.nextSrcSample;
        v.nextSrcSample = readRawSample(v, v.srcIndex + 1);
    }
    // Shifts, not divides: the M0+ has no divide instruction.
    return (int16_t)(((int32_t)interpolated * (int32_t)v.mixVolume) >> 6);
}

void __not_in_flash_func(TrackerPlayer::mixOneSample)(int16_t& outL, int16_t& outR) {
    int32_t mixL = 0, mixR = 0;
    for (int i = 0; i < _voiceCount; i++) {
        TrackerVoice& v = voiceAt(i);
        if (!v.havePair || v.sampleStep16 == 0 || !v.smp) continue;
        int16_t s = readVoiceSample(v);
        mixL += (s * (int32_t)(255 - v.mixPan)) >> 8;
        mixR += (s * (int32_t)v.mixPan) >> 8;
    }
    mixL = (mixL * (int32_t)_globalVolume) >> 6;
    mixR = (mixR * (int32_t)_globalVolume) >> 6;
    // +25% makeup gain so trackers sit level with WAV and MIDI playback;
    // the soft clamp takes the rare peaks this pushes over.
    mixL += mixL >> 2;
    mixR += mixR >> 2;
    outL = (int16_t)softClampMix(mixL);
    outR = (int16_t)softClampMix(mixR);
}

void __not_in_flash_func(TrackerPlayer::update)() {
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;
    size_t produced = 0;
    while (produced < MAX_FRAMES_PER_UPDATE) {
        if (Synth::wavStreamFree() == 0) break;
        if (_samplesUntilNextTick == 0) {
            advanceTick();
            if (_state == STATE_DONE) break;
            _samplesUntilNextTick = tickLength(); // tempo may have just changed
        }
        int16_t frame[2];
        mixOneSample(frame[0], frame[1]);
        if (Synth::wavStreamWrite(frame, 1) == 0) break;
        _samplesUntilNextTick--;
        produced++;
    }
}

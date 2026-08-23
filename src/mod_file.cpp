#include "mod_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>

namespace {

// Amiga period table, finetune 0, 3 octaves (36 notes) -- the standard
// ProTracker constants. Other finetune values are derived from this base
// row via the exponential pitch formula (see ModPlayer::applyFinetune())
// rather than a second historical lookup table per finetune step --
// musically equivalent, just not bit-for-bit identical to original Amiga
// hardware period rounding.
const uint16_t BASE_PERIOD_TABLE[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

// Amiga hardware clock constant used to convert a period into a playback
// rate -- the PAL value, the conventional choice essentially every MOD
// player/tool uses regardless of the file's own origin.
const double AMIGA_PAL_CLOCK = 7093789.2;

bool matchesFormatTag(const char* tag, int& outChannels) {
    if (strncmp(tag, "M.K.", 4) == 0 || strncmp(tag, "M!K!", 4) == 0 || strncmp(tag, "FLT4", 4) == 0) {
        outChannels = 4;
        return true;
    }
    if (strncmp(tag, "6CHN", 4) == 0) { outChannels = 6; return true; }
    if (strncmp(tag, "8CHN", 4) == 0 || strncmp(tag, "FLT8", 4) == 0 ||
        strncmp(tag, "CD81", 4) == 0 || strncmp(tag, "OKTA", 4) == 0) {
        outChannels = 8;
        return true;
    }
    // Generic "xCHN" (1 digit) and "xxCH" (2 digit) numeric conventions.
    if (tag[0] >= '0' && tag[0] <= '9') {
        if (tag[1] >= '0' && tag[1] <= '9' && tag[2] == 'C' && tag[3] == 'H') {
            outChannels = (tag[0] - '0') * 10 + (tag[1] - '0');
            return outChannels > 0 && outChannels <= ModPlayer::MAX_MOD_CHANNELS;
        }
        if (tag[1] == 'C' && tag[2] == 'H' && tag[3] == 'N') {
            outChannels = tag[0] - '0';
            return outChannels > 0;
        }
    }
    return false;
}

// _mixShift budgets headroom for this many simultaneous full-volume
// channels, capped well below MAX_MOD_CHANNELS (32, reachable via the
// numeric "xxCH"/"xCHN" tag convention above) -- see _mixShift's own
// comment for why a literal all-channels-at-once budget was needlessly
// quiet for real music, and softClampMix() below for what absorbs the
// (rare) moments actual content exceeds this budget. Same cap, same
// reasoning, duplicated in S3mPlayer::load()/XmPlayer::load().
//
// No longer actually used to compute _mixShift (see load()) -- capping at
// 8 channels only ever reduced the shift for files with *more* than 8
// channels, so a classic 4-channel MOD (or any <=8-channel file) got zero
// benefit from it and stayed shifted by its full literal channel count.
// Real-hardware listening found tracker playback still noticeably quieter
// than WAV/MIDI even after that fix -- by roughly the same factor a
// 4-channel MOD's unchanged shift=2 (divide by 4) would predict. Kept
// here only as a reminder of that dead end; _mixShift is unconditionally
// 0 now, relying entirely on softClampMix() for safety (see its own
// comment -- it hard-caps output regardless of how large the pre-clamp
// sum gets, so this is safe at any channel count, just a tradeoff between
// baseline loudness and how often busy/high-channel files lean on that
// compression).
const int MIX_SHIFT_TYPICAL_CHANNELS = 8;

// Soft knee above SOFT_KNEE_START, hard ceiling at HARD_LIMIT -- same
// shape and same constants as Synth's own softLimit() in synth.cpp (kept
// independent here rather than shared/exported, matching this file's
// existing convention of parallel, cross-referenced per-engine mixing
// code rather than a shared utility). Replaces mixOneSample()'s previous
// bare +-32767 clamp -- now that MIX_SHIFT_TYPICAL_CHANNELS budgets less
// headroom than a literal all-channels-at-once bound, that clamp is more
// likely to actually trigger, so it needs to compress gracefully instead
// of cutting off flat.
int32_t softClampMix(int32_t mix) {
    int32_t sign = (mix < 0) ? -1 : 1;
    int32_t mag = mix * sign;
    if (mag > 24576) mag = 24576 + (mag - 24576) / 4;
    if (mag > 32000) mag = 32000;
    return mag * sign;
}

} // namespace

bool ModPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!_file.open(path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    _file.seekSet(20); // skip song name
    for (int i = 0; i < MAX_SAMPLES; i++) {
        uint8_t hdr[30];
        if (_file.read(hdr, 30) != 30) {
            strncpy(_error, "invalid MOD file", sizeof(_error) - 1);
            _file.close();
            _state = STATE_ERROR;
            return false;
        }
        ModSample& s = _samples[i];
        s.length = (((uint32_t)hdr[22] << 8) | hdr[23]) * 2;
        int8_t rawFinetune = (int8_t)(hdr[24] & 0x0F) << 4; // low nibble, sign-extend via shift
        s.finetune = rawFinetune >> 4;
        s.volume = hdr[25] > 64 ? 64 : hdr[25];
        // "No loop" is conventionally encoded as a loop length of 1 WORD
        // (not 0), essentially universal in real files -- must be checked
        // against the raw word value, before the *2 byte conversion.
        // Checking the already-converted byte value (<=1) missed this
        // near-every-file case entirely (word 1 -> byte 2, which passed
        // the check), silently turning every such sample into an
        // infinite 2-byte loop -- a stuck, near-silent buzz instead of
        // the actual sample content on essentially any real-world file.
        uint16_t rawLoopStart = ((uint16_t)hdr[26] << 8) | hdr[27];
        uint16_t rawLoopLen = ((uint16_t)hdr[28] << 8) | hdr[29];
        if (rawLoopLen <= 1) {
            s.loopStart = 0;
            s.loopLength = 0;
        } else {
            s.loopStart = (uint32_t)rawLoopStart * 2;
            s.loopLength = (uint32_t)rawLoopLen * 2;
        }
        if (s.length > 0) _numInstruments++;
    }

    uint8_t songLenByte, restartByte;
    if (_file.read(&songLenByte, 1) != 1 || _file.read(&restartByte, 1) != 1) {
        strncpy(_error, "invalid MOD file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }
    _songLength = songLenByte > 128 ? 128 : songLenByte;
    _restartPos = restartByte;

    if (_file.read(_orderTable, 128) != 128) {
        strncpy(_error, "invalid MOD file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    char tag[4];
    if (_file.read(tag, 4) != 4 || !matchesFormatTag(tag, _numChannels)) {
        strncpy(_error, "unsupported format", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    // See _mixShift's declaration and MIX_SHIFT_TYPICAL_CHANNELS's comment --
    // no channel-count-based headroom shift at all anymore, relying
    // entirely on softClampMix() for safety.
    _mixShift = 0;

    int maxPattern = 0;
    for (int i = 0; i < _songLength; i++) {
        if (_orderTable[i] > maxPattern) maxPattern = _orderTable[i];
    }
    _numPatterns = maxPattern + 1;

    _patternDataStart = 20 + (uint32_t)MAX_SAMPLES * 30 + 2 + 128 + 4;
    uint32_t sampleDataStart = _patternDataStart + (uint32_t)_numPatterns * _numChannels * 64 * 4;
    uint32_t offset = sampleDataStart;
    for (int i = 0; i < MAX_SAMPLES; i++) {
        _samples[i].fileOffset = offset;
        offset += _samples[i].length;
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        c = ModChannel();
        // Classic hard-panned Amiga convention: channels alternate
        // L,R,R,L (indices 0,3,4,7,... hard left; 1,2,5,6,... hard right).
        c.pan = ((ch & 3) == 0 || (ch & 3) == 3) ? 0 : 255;
    }

    _orderPos = 0;
    // -1, not 0: advanceRow() (see its own comment) only ever reaches row
    // 0 via `_row++` from -1 on the very first call -- starting at 0
    // meant that first call incremented straight past it to row 1,
    // silently skipping row 0 of the whole song every time it's freshly
    // opened (though not on subsequent loop-arounds, which set _row=0
    // directly). Root cause of reported "missing"/"no sound" files where
    // the skipped row happened to carry volume/instrument setup.
    _row = -1;
    _tick = 0;
    _speed = 6;
    _tempo = 125;
    _samplesUntilNextTick = ticksToSamples(_tempo);
    _patternBreakPending = false;
    _positionJumpPending = false;

    _elapsedMsFrozen = 0;
    _lastResumeMicros = micros();
    _state = STATE_PAUSED;
    return true;
}

void ModPlayer::close() {
    if (_file) _file.close();
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

void ModPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _lastResumeMicros = micros();
    _state = STATE_PLAYING;
    // Pre-fill the ring buffer before the I2S consumer starts pulling
    // from it -- same fix XmPlayer::play()/S3mPlayer::play() already
    // needed (see S3mPlayer::play()'s comment for the full reasoning).
    for (int i = 0; i < 128 && Synth::wavStreamFree() > 0; i++) update();
    Synth::wavStreamSetActive(true);
}

void ModPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _elapsedMsFrozen += (micros() - _lastResumeMicros) / 1000;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void ModPlayer::stop() {
    close();
}

uint32_t ModPlayer::elapsedMs() const {
    uint32_t extra = (_state == STATE_PLAYING) ? (micros() - _lastResumeMicros) / 1000 : 0;
    return _elapsedMsFrozen + extra;
}

uint32_t ModPlayer::ticksToSamples(uint16_t tempo) {
    if (tempo == 0) tempo = 125;
    return (uint32_t)((double)SAMPLE_RATE * 2.5 / (double)tempo);
}

uint16_t ModPlayer::periodForNote(int noteIndex) {
    if (noteIndex < 0) noteIndex = 0;
    if (noteIndex > 35) noteIndex = 35;
    return BASE_PERIOD_TABLE[noteIndex];
}

int ModPlayer::nearestNoteIndexForPeriod(uint16_t period) {
    int best = 0;
    int bestDiff = abs((int)period - (int)BASE_PERIOD_TABLE[0]);
    for (int i = 1; i < 36; i++) {
        int diff = abs((int)period - (int)BASE_PERIOD_TABLE[i]);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

uint16_t ModPlayer::applyFinetune(uint16_t basePeriod, int8_t finetune) {
    if (finetune == 0) return basePeriod;
    double factor = pow(2.0, -(double)finetune / 96.0); // finetune is in 1/8-semitone units, 96 == 12*8
    double result = (double)basePeriod * factor;
    if (result < 1.0) result = 1.0;
    return (uint16_t)(result + 0.5);
}

void ModPlayer::recomputeStep(ModChannel& c) {
    if (c.period == 0) { c.sampleStep16 = 0; return; }
    double hz = AMIGA_PAL_CLOCK / ((double)c.period * 2.0);
    c.sampleStep16 = (uint32_t)((hz / (double)SAMPLE_RATE) * 65536.0);
}

int16_t ModPlayer::readRawSample(ModChannel& c, const ModSample& s, uint32_t index) {
    if (c.looping && index >= s.loopStart + s.loopLength && s.loopLength > 0) {
        index = s.loopStart + ((index - s.loopStart) % s.loopLength);
    }
    if (index >= s.length) return 0; // ran off the end of a non-looping sample -- silence

    if (c.chunkLen == 0 || index < c.chunkStart || index >= c.chunkStart + c.chunkLen) {
        uint32_t readLen = ModChannel::CHUNK_SIZE;
        if (index + readLen > s.length) readLen = s.length - index;
        _file.seekSet(s.fileOffset + index);
        int n = _file.read(c.chunkBuf, readLen);
        c.chunkStart = index;
        c.chunkLen = (n > 0) ? (uint32_t)n : 0;
        if (c.chunkLen == 0) return 0;
    }
    return (int16_t)((int8_t)c.chunkBuf[index - c.chunkStart]) << 8;
}

void ModPlayer::triggerNote(int ch) {
    ModChannel& c = _channels[ch];
    c.samplePos16 = 0;
    c.srcIndex = 0;
    c.chunkLen = 0; // invalidate the read-ahead cache -- new position
    c.havePair = false;
    recomputeStep(c);

    if (c.sample >= 1 && c.sample <= MAX_SAMPLES) {
        const ModSample& s = _samples[c.sample - 1];
        c.looping = s.loopLength > 1;
        if (s.length > 0) {
            c.lastSrcSample = readRawSample(c, s, 0);
            c.nextSrcSample = readRawSample(c, s, 1);
            c.havePair = true;
        }
    }
}

int16_t ModPlayer::readChannelSample(ModChannel& c) {
    if (!c.havePair || c.sampleStep16 == 0 || c.sample < 1) return 0;
    const ModSample& s = _samples[c.sample - 1];

    uint16_t frac16 = (uint16_t)(c.samplePos16 & 0xFFFF);
    int32_t diff = (int32_t)c.nextSrcSample - (int32_t)c.lastSrcSample;
    int16_t interpolated = (int16_t)(c.lastSrcSample + ((diff * (int32_t)frac16) >> 16));

    c.samplePos16 += c.sampleStep16;
    while (c.havePair && c.samplePos16 >= 0x10000) {
        c.samplePos16 -= 0x10000;
        c.srcIndex++;
        if (c.looping && s.loopLength > 0 && c.srcIndex >= s.loopStart + s.loopLength) {
            c.srcIndex = s.loopStart + ((c.srcIndex - (s.loopStart + s.loopLength)) % s.loopLength);
        } else if (!c.looping && c.srcIndex >= s.length) {
            c.havePair = false; // ran off the end -- channel goes silent until re-triggered
            break;
        }
        c.lastSrcSample = c.nextSrcSample;
        c.nextSrcSample = readRawSample(c, s, c.srcIndex + 1);
    }

    // >>6 instead of /64 -- volume is always clamped to 0-64, same
    // real-hardware-motivated fix as S3mPlayer::readChannelSample() (see
    // its comment) -- a genuine signed division at the hottest call site
    // in the whole player, on hardware with no divide instruction at all.
    return (int16_t)(((int32_t)interpolated * (int32_t)c.volume) >> 6);
}

void ModPlayer::mixOneSample(int16_t& outL, int16_t& outR) {
    int32_t mixL = 0, mixR = 0;
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        if (c.period == 0 || !c.havePair) continue;
        int16_t s = readChannelSample(c);
        // >>8 instead of /255 -- see S3mPlayer::mixOneSample()'s identical
        // comment (no hardware divide on the Cortex-M0+, and /255 isn't
        // even a power of 2 -- a genuine full software division routine,
        // twice, every active channel, every sample).
        mixL += (s * (int32_t)(255 - c.pan)) >> 8;
        mixR += (s * (int32_t)c.pan) >> 8;
    }
    // _mixShift is always 0 now (see its declaration) -- this shift is a
    // no-op, kept only so the field still does something if a future
    // channel-count-based case ever needs it back. Even with the shift
    // removed entirely, real-hardware A/B listening against WAV/MIDI still
    // found tracker playback a bit quiet, so a further +25% makeup gain
    // here closes most of that remaining gap. softClampMix() below absorbs
    // the rare moments this pushes things over budget.
    mixL = mixL >> _mixShift;
    mixR = mixR >> _mixShift;
    mixL += mixL >> 2;
    mixR += mixR >> 2;
    mixL = softClampMix(mixL);
    mixR = softClampMix(mixR);
    outL = (int16_t)mixL;
    outR = (int16_t)mixR;
}

void ModPlayer::advanceRow() {
    // Per-row-only effect memory resets before reprocessing this row's
    // cells -- these are scoped to the row that specifies them, unlike
    // porta/vibrato/volume-slide speed, which persist until changed.
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        c.arpeggioParam = 0;
        c.retrigParam = 0;
        c.noteDelayParam = 0;
        c.noteCutParam = 0xFF;
    }

    if (_patternBreakPending || _positionJumpPending) {
        int nextOrderPos = _positionJumpPending ? _positionJumpTarget : (_orderPos + 1);
        int nextRow = _patternBreakPending ? _patternBreakRow : 0;
        if (nextRow < 0 || nextRow > 63) nextRow = 0;
        _orderPos = nextOrderPos;
        _row = nextRow;
        _patternBreakPending = false;
        _positionJumpPending = false;
    } else {
        _row++;
        if (_row >= 64) {
            _row = 0;
            _orderPos++;
        }
    }

    if (_orderPos >= _songLength || _orderPos < 0) {
        if (_restartPos >= 0 && _restartPos < _songLength) {
            _orderPos = _restartPos;
            _row = 0;
        } else {
            _state = STATE_DONE;
            return;
        }
    }

    int patternNum = _orderTable[_orderPos];
    if (patternNum >= _numPatterns) patternNum = 0; // defensive -- shouldn't happen for a valid file

    uint8_t rowBuf[MAX_MOD_CHANNELS * 4];
    uint32_t rowOffset = _patternDataStart + (uint32_t)patternNum * _numChannels * 64 * 4 + (uint32_t)_row * _numChannels * 4;
    _file.seekSet(rowOffset);
    int need = _numChannels * 4;
    if (_file.read(rowBuf, need) != need) {
        _state = STATE_DONE; // truncated/corrupt file mid-song -- stop cleanly rather than mis-play garbage
        return;
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        processCellTriggers(ch, &rowBuf[ch * 4]);
    }
}

void ModPlayer::processCellTriggers(int ch, const uint8_t cell[4]) {
    ModChannel& c = _channels[ch];

    uint8_t sampleNum = (uint8_t)((cell[0] & 0xF0) | (cell[2] >> 4));
    uint16_t period = (uint16_t)(((cell[0] & 0x0F) << 8) | cell[1]);
    uint8_t effect = cell[2] & 0x0F;
    uint8_t param = cell[3];

    bool isTonePorta = (effect == 0x3 || effect == 0x5);
    bool isNoteDelay = (effect == 0xE && (param >> 4) == 0xD && (param & 0x0F) != 0);

    // Instrument change (volume/finetune default) applies immediately
    // regardless of a pending note delay -- only the note trigger itself
    // is deferred.
    if (sampleNum >= 1 && sampleNum <= MAX_SAMPLES) {
        c.sample = sampleNum;
        c.volume = _samples[sampleNum - 1].volume;
    }

    if (isNoteDelay) {
        c.noteDelayParam = param & 0x0F;
        c.noteDelayPeriod = period; // 0 if this cell has no note of its own
    } else if (period != 0) {
        if (isTonePorta) {
            c.portaTarget = period;
        } else {
            c.period = period;
            triggerNote(ch);
        }
    }

    switch (effect) {
        case 0x0: // Arpeggio (param 00 with a bare note is "no arpeggio")
            c.arpeggioParam = param;
            break;
        case 0x1: // Portamento up
            if (param != 0) c.portaUpSpeed = param;
            break;
        case 0x2: // Portamento down
            if (param != 0) c.portaDownSpeed = param;
            break;
        case 0x3: // Tone portamento
            if (param != 0) c.tonePortaSpeed = param;
            break;
        case 0x4: // Vibrato
            if (param >> 4) c.vibratoSpeed = param >> 4;
            if (param & 0x0F) c.vibratoDepth = param & 0x0F;
            break;
        case 0x5: // Tone portamento + volume slide
        case 0x6: // Vibrato + volume slide
        case 0xA: // Volume slide
            if (param != 0) c.volSlideParam = param;
            break;
        case 0x7: // Tremolo
            if (param >> 4) c.tremoloSpeed = param >> 4;
            if (param & 0x0F) c.tremoloDepth = param & 0x0F;
            break;
        case 0x8: // Set panning -- see this class's header comment on scope
            c.pan = param;
            break;
        case 0x9: { // Sample offset
            if (param != 0) c.sampleOffsetParam = param;
            if (period != 0 && !isTonePorta && c.sample >= 1 && c.sample <= MAX_SAMPLES) {
                uint32_t off = (uint32_t)c.sampleOffsetParam << 8;
                const ModSample& s = _samples[c.sample - 1];
                if (off < s.length) {
                    c.srcIndex = off;
                    c.samplePos16 = 0;
                    c.chunkLen = 0;
                    c.lastSrcSample = readRawSample(c, s, off);
                    c.nextSrcSample = readRawSample(c, s, off + 1);
                    c.havePair = true;
                }
            }
            break;
        }
        case 0xB: // Position jump
            _positionJumpPending = true;
            _positionJumpTarget = param;
            break;
        case 0xC: // Set volume
            c.volume = (param > 64) ? 64 : param;
            break;
        case 0xD: // Pattern break (row is BCD-encoded)
            _patternBreakPending = true;
            _patternBreakRow = (param >> 4) * 10 + (param & 0x0F);
            break;
        case 0xE:
            if ((param >> 4) != 0xD) processExtendedTickZero(ch, param); // 0xD (note delay) already handled above
            break;
        case 0xF: // Set speed/tempo
            if (param == 0) _speed = 1; // technically "stop" in the strict spec; treated as speed=1, a safe simplification
            else if (param < 32) _speed = param;
            else _tempo = param;
            break;
        default:
            break;
    }
}

void ModPlayer::processExtendedTickZero(int ch, uint8_t param) {
    ModChannel& c = _channels[ch];
    uint8_t sub = param >> 4;
    uint8_t val = param & 0x0F;

    switch (sub) {
        case 0x1: // Fine portamento up (applied once, this tick only)
            if (c.period > val) c.period -= val;
            recomputeStep(c);
            break;
        case 0x2: // Fine portamento down
            c.period += val;
            recomputeStep(c);
            break;
        case 0x3: // Glissando control
            c.glissando = (val != 0);
            break;
        case 0x4: // Set vibrato waveform -- only sine is implemented, no-op otherwise
        case 0x7: // Set tremolo waveform -- same
            break;
        case 0x5: // Set finetune
            if (c.sample >= 1 && c.sample <= MAX_SAMPLES) {
                int8_t ft = (int8_t)(val << 4) >> 4; // sign-extend 4-bit value
                _samples[c.sample - 1].finetune = ft;
            }
            break;
        case 0x6: // Pattern loop
            if (val == 0) {
                c.patternLoopRow = (uint8_t)_row;
            } else {
                if (c.patternLoopCount == 0) c.patternLoopCount = val;
                else c.patternLoopCount--;
                if (c.patternLoopCount > 0) {
                    _patternBreakPending = true;
                    _patternBreakRow = c.patternLoopRow;
                    // Stay on the current pattern -- a loop-back is a row
                    // jump within THIS pattern, not a position advance,
                    // so cancel the implicit "advance to next order
                    // position" pattern break would otherwise imply.
                    _positionJumpPending = true;
                    _positionJumpTarget = _orderPos;
                }
            }
            break;
        case 0x8: // Set panning, coarse (0-15 -> 0-255)
            c.pan = (uint8_t)(val * 17);
            break;
        case 0x9: // Retrigger note every `val` ticks
            if (val > 0) c.retrigParam = val;
            break;
        case 0xA: // Fine volume slide up
            c.volume = (uint8_t)min(64, c.volume + val);
            break;
        case 0xB: // Fine volume slide down
            c.volume = (uint8_t)max(0, (int)c.volume - val);
            break;
        case 0xC: // Note cut
            c.noteCutParam = val;
            break;
        case 0xE: // Pattern delay -- hold the current row for `val` extra
                  // full tick-cycles before advancing (see advanceTick()),
                  // not a permanent speed change.
            _patternDelayRepeatsLeft = val;
            break;
        case 0x0: // Set filter -- no hardware filter to toggle here, no-op
        case 0xF: // Invert Loop / Funk Repeat -- explicitly unsupported, see header comment
        default:
            break;
    }
}

void ModPlayer::processTickEffects() {
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];

        if (c.noteDelayParam != 0 && _tick == c.noteDelayParam) {
            if (c.noteDelayPeriod != 0) c.period = c.noteDelayPeriod;
            triggerNote(ch);
            c.noteDelayParam = 0;
        }
        if (c.noteCutParam != 0xFF && _tick == c.noteCutParam) {
            c.volume = 0;
            c.noteCutParam = 0xFF;
        }
        if (c.retrigParam > 0 && (_tick % c.retrigParam) == 0) {
            triggerNote(ch);
        }

        if (c.period == 0) continue;

        // Volume slide (Axy, and the volume-slide half of 5xy/6xy) --
        // x is slide-up amount, y is slide-down; only one is normally
        // nonzero per ProTracker convention, but both are honored if set.
        uint8_t slideUp = c.volSlideParam >> 4;
        uint8_t slideDown = c.volSlideParam & 0x0F;
        if (slideUp) c.volume = (uint8_t)min(64, c.volume + slideUp);
        else if (slideDown) c.volume = (uint8_t)max(0, (int)c.volume - slideDown);

        // Portamento up/down (1xx/2xx) -- clamped to the table's own
        // period range, same as real ProTracker (unlike tone portamento
        // below, plain portamento has no target to stop it, so without a
        // clamp it can slide arbitrarily far: up into silence/aliasing
        // as the period approaches 0, or -- worse -- down past 65535 and
        // wrap back around to a small (very high-pitched) period, both
        // plausible causes of notes that seem to randomly go silent or
        // squeal on files that lean on this effect).
        if (c.portaUpSpeed) {
            int p = (int)c.period - c.portaUpSpeed;
            if (p < BASE_PERIOD_TABLE[35]) p = BASE_PERIOD_TABLE[35];
            c.period = (uint16_t)p;
            recomputeStep(c);
        }
        if (c.portaDownSpeed) {
            int p = (int)c.period + c.portaDownSpeed;
            if (p > BASE_PERIOD_TABLE[0]) p = BASE_PERIOD_TABLE[0];
            c.period = (uint16_t)p;
            recomputeStep(c);
        }

        // Tone portamento (3xx, and the portamento half of 5xy) -- slides
        // the current period toward portaTarget at tonePortaSpeed/tick.
        bool doTonePorta = (c.portaTarget != 0);
        if (doTonePorta && c.tonePortaSpeed) {
            if (c.period < c.portaTarget) {
                c.period = (uint16_t)min((int)c.portaTarget, c.period + c.tonePortaSpeed);
            } else if (c.period > c.portaTarget) {
                c.period = (uint16_t)max((int)c.portaTarget, c.period - c.tonePortaSpeed);
            }
            recomputeStep(c);
        }

        // Vibrato (4xy, and the vibrato half of 6xy) -- a transient period
        // offset recomputed fresh each tick, not accumulated into c.period
        // itself (so it doesn't drift the "real" pitch).
        uint16_t effectivePeriod = c.period;
        if (c.vibratoDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.vibratoPos / 64.0f;
            int16_t delta = (int16_t)(sinf(angle) * c.vibratoDepth * 4.0f);
            effectivePeriod = (uint16_t)max(1, (int)c.period + delta);
            c.vibratoPos = (uint8_t)((c.vibratoPos + c.vibratoSpeed) & 0x3F);
        }
        if (effectivePeriod != c.period) {
            uint16_t savedPeriod = c.period;
            c.period = effectivePeriod;
            recomputeStep(c);
            c.period = savedPeriod;
        }

        // Tremolo (7xy) -- same idea as vibrato, transient volume offset.
        if (c.tremoloDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.tremoloPos / 64.0f;
            int delta = (int)(sinf(angle) * c.tremoloDepth * 4.0f);
            int v = (int)c.volume + delta;
            if (v < 0) v = 0; else if (v > 64) v = 64;
            c.volume = (uint8_t)v; // transient -- next tick recomputes from the same base c.volume plus slide, see note below
            c.tremoloPos = (uint8_t)((c.tremoloPos + c.tremoloSpeed) & 0x3F);
        }

        // Arpeggio (0xy) -- cycles the sounding pitch between the base
        // note, +x semitones, and +y semitones every tick within the row.
        if (c.arpeggioParam != 0) {
            int cycle = _tick % 3;
            int semis = (cycle == 1) ? (c.arpeggioParam >> 4) : (cycle == 2) ? (c.arpeggioParam & 0x0F) : 0;
            if (semis != 0) {
                int baseNote = nearestNoteIndexForPeriod(c.period);
                uint16_t arpPeriod = periodForNote(baseNote - semis); // higher pitch == lower period == earlier table index
                uint16_t savedPeriod = c.period;
                c.period = arpPeriod;
                recomputeStep(c);
                c.period = savedPeriod;
            }
        }
    }
}

void ModPlayer::advanceTick() {
    _tick++;
    if (_tick >= _speed) {
        _tick = 0;
        if (_patternDelayRepeatsLeft > 0) {
            // EEy: hold on the current row for one more full tick-cycle
            // instead of advancing -- per-tick effects (vibrato/
            // portamento/etc, via processTickEffects()) keep applying
            // through these extra cycles same as any other row, notes
            // just don't re-trigger.
            _patternDelayRepeatsLeft--;
        } else {
            advanceRow();
        }
    } else {
        processTickEffects();
    }
}

void ModPlayer::update() {
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;

    // Was 512 -- lowered to match XmPlayer/S3mPlayer::update()'s identical
    // fix (see S3mPlayer::update()'s comment for the full real-hardware
    // reasoning: bounds the worst-case single-call latency a burst of
    // several channels needing a refill at once can cost).
    const size_t MAX_FRAMES_PER_TICK_CALL = 128;
    size_t produced = 0;
    while (produced < MAX_FRAMES_PER_TICK_CALL) {
        if (Synth::wavStreamFree() == 0) break;
        if (_samplesUntilNextTick == 0) {
            advanceTick();
            if (_state == STATE_DONE) break;
            _samplesUntilNextTick = ticksToSamples(_tempo); // recompute in case Fxx just changed tempo
        }
        int16_t l, r;
        mixOneSample(l, r);
        int16_t frame[2] = { l, r };
        if (Synth::wavStreamWrite(frame, 1) == 0) break;
        _samplesUntilNextTick--;
        produced++;
    }
}

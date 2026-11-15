#include "mod_file.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>

namespace {

// Amiga period table, finetune 0, 3 octaves (36 notes). Other finetunes
// are derived with the exponential pitch formula, see applyFinetune().
const uint16_t BASE_PERIOD_TABLE[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

// PAL Amiga clock, period -> playback rate.
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

} // namespace

bool ModPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!_file.open(path, O_RDONLY)) return loadFail("could not open file");

    _numInstruments = 0;
    _file.seekSet(20); // skip song name
    for (int i = 0; i < MAX_SAMPLES; i++) {
        uint8_t hdr[30];
        if (_file.read(hdr, 30) != 30) return loadFail("invalid MOD file");
        ModSample& s = _samples[i];
        s.length = (((uint32_t)hdr[22] << 8) | hdr[23]) * 2;
        int8_t rawFinetune = (int8_t)(hdr[24] & 0x0F) << 4; // low nibble, sign-extend via shift
        s.finetune = rawFinetune >> 4;
        s.volume = hdr[25] > 64 ? 64 : hdr[25];
        // "No loop" is a loop length of 1 word (or 0), checked on the raw
        // word value before the *2 byte conversion.
        uint16_t rawLoopStart = ((uint16_t)hdr[26] << 8) | hdr[27];
        uint16_t rawLoopLen = ((uint16_t)hdr[28] << 8) | hdr[29];
        if (rawLoopLen <= 1) {
            s.loopStart = 0;
            s.loopEnd = 0;
        } else {
            s.loopStart = (uint32_t)rawLoopStart * 2;
            s.loopEnd = s.loopStart + (uint32_t)rawLoopLen * 2;
        }
        if (s.length > 0) _numInstruments++;
    }

    uint8_t songLenByte, restartByte;
    if (_file.read(&songLenByte, 1) != 1 || _file.read(&restartByte, 1) != 1) return loadFail("invalid MOD file");
    _songLength = songLenByte > 128 ? 128 : songLenByte;
    _restartPos = restartByte;

    if (_file.read(_orderTable, 128) != 128) return loadFail("invalid MOD file");

    char tag[4];
    if (_file.read(tag, 4) != 4 || !matchesFormatTag(tag, _numChannels)) return loadFail("unsupported format");

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
        // Amiga hard panning: channels go L,R,R,L.
        c.pan = ((ch & 3) == 0 || (ch & 3) == 3) ? 0 : 255;
        c.mixPan = c.pan;
    }
    attachVoices(&_channels[0], sizeof(ModChannel), _numChannels, &_chunks[0][0], CHUNK_FRAMES);

    _globalVolume = 64;
    resetSequencer(6, 125);
    return true;
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
    double factor = pow(2.0, -(double)finetune / 96.0); // finetune is in 1/8 semitones, 96 == 12*8
    double result = (double)basePeriod * factor;
    if (result < 1.0) result = 1.0;
    return (uint16_t)(result + 0.5);
}

void ModPlayer::recomputeStep(ModChannel& c) {
    if (c.period == 0) { c.sampleStep16 = 0; return; }
    double hz = AMIGA_PAL_CLOCK / ((double)c.period * 2.0);
    c.sampleStep16 = limitStep((uint32_t)((hz / (double)SAMPLE_RATE) * 65536.0));
}

// Reads the CHUNK_FRAMES-aligned block holding `index`.
int16_t ModPlayer::refillChunk(TrackerVoice& v, uint32_t index) {
    const TrackerSample& s = *v.smp;
    uint32_t start = index & ~(uint32_t)(CHUNK_FRAMES - 1);
    uint32_t len = CHUNK_FRAMES;
    if (start + len > s.length) len = s.length - start;
    v.chunkSmp = v.smp;
    v.chunkStart = start;
    // Frames past a short read or a failed seek stay zero and cached, so a
    // truncated sample doesn't hit the SD card on every frame.
    v.chunkLen = len;
    int n = 0;
    if (_file.seekSet(s.fileOffset + start)) n = _file.read(_rawScratch, len);
    uint32_t got = n > 0 ? (uint32_t)n : 0;
    for (uint32_t i = 0; i < got; i++) v.chunkBuf[i] = (int16_t)((int8_t)_rawScratch[i]) << 8;
    for (uint32_t i = got; i < len; i++) v.chunkBuf[i] = 0;
    return v.chunkBuf[index - start];
}

void ModPlayer::setSample(ModChannel& c, uint8_t sampleNum) {
    c.sample = sampleNum;
    c.smp = &_samples[sampleNum - 1];
}

void ModPlayer::triggerNote(int ch) {
    ModChannel& c = _channels[ch];
    recomputeStep(c);
    if (c.smp) c.looping = (c.smp->loopEnd - c.smp->loopStart) > 1;
    startVoiceAt(c, 0);
}

void ModPlayer::tickEnd() {
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        c.mixVolume = c.volume;
        c.mixPan = c.pan;
    }
}

void ModPlayer::advanceRow() {
    // Per-row effect state resets before this row's cells; porta/vibrato/
    // slide speeds persist until changed.
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        c.arpeggioParam = 0;
        c.retrigParam = 0;
        c.noteDelayParam = 0;
        c.noteCutParam = 0xFF;
    }

    stepPosition(64, 63);

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
    if (patternNum >= _numPatterns) patternNum = 0;

    uint8_t rowBuf[MAX_MOD_CHANNELS * 4];
    uint32_t rowOffset = _patternDataStart + (uint32_t)patternNum * _numChannels * 64 * 4 + (uint32_t)_row * _numChannels * 4;
    _file.seekSet(rowOffset);
    int need = _numChannels * 4;
    if (_file.read(rowBuf, need) != need) {
        _state = STATE_DONE; // truncated file mid-song
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
        setSample(c, sampleNum);
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
                if (off < c.smp->length) startVoiceAt(c, off);
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
            patternLoop(c.patternLoopRow, c.patternLoopCount, val);
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


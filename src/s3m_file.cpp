#include "sd_card.h"
#include "s3m_file.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>

namespace {

// Same Amiga clock MOD uses. ST3 periods are 4x Amiga periods, so a
// period p plays at AMIGA_PAL_CLOCK * 2 / p Hz.
// float, not double: the M0+ has no FPU and this runs on every trigger and slide tick.
const float AMIGA_PAL_CLOCK = 7093789.2f;
const uint32_t MAX_PERIOD = 65535 * 4; // highest period notes and slides reach, about 54 Hz

// S3M file pointers are stored as offset/16.
inline uint32_t paraToOffset(uint32_t para) { return para * 16; }

} // namespace

bool S3mPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!sdOpenPath(_file, path, O_RDONLY)) return loadFail("could not open file");
    if (!sdOpenPath(_patternFile, path, O_RDONLY)) return loadFail("could not open file");

    // The fixed header is 96 bytes: name, type, counts and flags, "SCRM"
    // at 44, global volume/speed/tempo/default pan at 48-53, and the
    // 32-byte channel settings at 64-95. The order table follows at 96.
    uint8_t hdr[96];
    if (_file.read(hdr, 96) != 96) return loadFail("invalid S3M file");
    if (hdr[29] != 16 || memcmp(hdr + 44, "SCRM", 4) != 0) return loadFail("unsupported format");

    uint16_t ordNumReal = (uint16_t)hdr[32] | ((uint16_t)hdr[33] << 8);
    uint16_t insNumReal = (uint16_t)hdr[34] | ((uint16_t)hdr[35] << 8);
    uint16_t patNumReal = (uint16_t)hdr[36] | ((uint16_t)hdr[37] << 8);
    uint16_t ffi = (uint16_t)hdr[42] | ((uint16_t)hdr[43] << 8);
    bool samplesSigned = (ffi == 1); // 1 = old-format signed samples (rare); anything else = unsigned (common)

    uint8_t globalVol = hdr[48];
    uint8_t initialSpeed = hdr[49];
    uint8_t initialTempo = hdr[50];
    uint8_t defaultPan = hdr[53]; // 0xFC means a 32-byte pan table follows the pattern parapointers

    // Sanity bound against a corrupt file claiming an absurd count --
    // real .s3m files are always far under this (Scream Tracker 3 itself
    // caps instruments at 99, patterns at 100).
    if (ordNumReal > 4096 || insNumReal > 4096 || patNumReal > 4096) return loadFail("invalid S3M file");

    _numChannels = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        _channelEnabled[i] = (hdr[64 + i] != 0xFF);
        if (_channelEnabled[i]) _numChannels++;
    }

    // Order table: bulk-read what fits in _orderTable, skip the rest
    // (still needed to keep the file position correct for what follows).
    int ordNum = (int)(ordNumReal < (uint16_t)MAX_ORDERS ? ordNumReal : (uint16_t)MAX_ORDERS);
    if (_file.read(_orderTable, ordNum) != ordNum) return loadFail("invalid S3M file");
    if (ordNumReal > ordNum) _file.seekCur(ordNumReal - ordNum);
    _songLength = ordNum;

    // Instrument parapointers (2 bytes each, LE).
    uint16_t insParapointers[MAX_INSTRUMENTS] = {0};
    int insNum = (int)(insNumReal < (uint16_t)MAX_INSTRUMENTS ? insNumReal : (uint16_t)MAX_INSTRUMENTS);
    for (int i = 0; i < insNum; i++) {
        uint8_t pb[2];
        if (_file.read(pb, 2) != 2) break;
        insParapointers[i] = (uint16_t)pb[0] | ((uint16_t)pb[1] << 8);
    }
    if (insNumReal > insNum) _file.seekCur((uint32_t)(insNumReal - insNum) * 2);

    // Pattern parapointers (2 bytes each, LE).
    uint16_t patParapointers[MAX_PATTERNS] = {0};
    int patNum = (int)(patNumReal < (uint16_t)MAX_PATTERNS ? patNumReal : (uint16_t)MAX_PATTERNS);
    for (int i = 0; i < patNum; i++) {
        uint8_t pb[2];
        if (_file.read(pb, 2) != 2) break;
        patParapointers[i] = (uint16_t)pb[0] | ((uint16_t)pb[1] << 8);
    }
    if (patNumReal > patNum) _file.seekCur((uint32_t)(patNumReal - patNum) * 2);
    for (int i = 0; i < MAX_PATTERNS; i++) {
        _patternOffsets[i] = (i < patNum) ? paraToOffset(patParapointers[i]) : 0;
    }

    // Optional default-pan table: 32 bytes, immediately after the
    // pattern parapointers, present only when defaultPan == 0xFC. Each
    // byte: bit 0x20 set means "use this value" (bits 0-3, scaled to
    // 0-255), else this channel keeps the center default set below.
    uint8_t panTable[MAX_CHANNELS];
    bool havePanTable = false;
    if (defaultPan == 0xFC) {
        if (_file.read(panTable, MAX_CHANNELS) == MAX_CHANNELS) havePanTable = true;
    }

    // Instrument headers: each a fixed 80 bytes, located via its own
    // parapointer (not read sequentially -- they aren't necessarily
    // contiguous/ordered in the file).
    _numInstruments = 0;
    for (int i = 0; i < insNum; i++) {
        S3mSample& s = _samples[i];
        s = S3mSample();
        if (insParapointers[i] == 0) continue; // empty instrument slot

        uint32_t insOffset = paraToOffset(insParapointers[i]);
        if (!_file.seekSet(insOffset)) continue;
        uint8_t ib[36]; // only the fields we need, offsets 0-35 of the 80-byte record
        if (_file.read(ib, 36) != 36) continue;

        uint8_t type = ib[0];
        if (type != 1) continue; // not a PCM sample (0 = empty, 2 = OPL2/Adlib -- not supported)

        uint32_t memSeg = ((uint32_t)ib[13] << 16) | ((uint32_t)ib[14]) | ((uint32_t)ib[15] << 8);
        s.fileOffset = paraToOffset(memSeg);
        s.length = (uint32_t)ib[16] | ((uint32_t)ib[17] << 8) | ((uint32_t)ib[18] << 16) | ((uint32_t)ib[19] << 24);
        uint32_t loopBegin = (uint32_t)ib[20] | ((uint32_t)ib[21] << 8) | ((uint32_t)ib[22] << 16) | ((uint32_t)ib[23] << 24);
        uint32_t loopEnd = (uint32_t)ib[24] | ((uint32_t)ib[25] << 8) | ((uint32_t)ib[26] << 16) | ((uint32_t)ib[27] << 24);
        s.volume = ib[28] > 64 ? 64 : ib[28];
        uint8_t flags = ib[31];
        s.c2spd = (uint32_t)ib[32] | ((uint32_t)ib[33] << 8) | ((uint32_t)ib[34] << 16) | ((uint32_t)ib[35] << 24);
        if (s.c2spd == 0) s.c2spd = 8363; // fall back to the ST3 default rather than divide-by-zero later

        bool loopFlag = (flags & 0x01) != 0;
        s.isStereo = (flags & 0x02) != 0;
        s.is16Bit = (flags & 0x04) != 0;
        s.isSigned = samplesSigned;

        if (loopFlag && loopEnd > loopBegin) {
            s.loopStart = loopBegin;
            s.loopEnd = loopEnd;
        } else {
            s.loopStart = 0;
            s.loopEnd = 0;
        }

        if (s.length > 0) _numInstruments++;
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        S3mChannel& c = _channels[ch];
        c = S3mChannel();
        if (havePanTable && (panTable[ch] & 0x20)) {
            c.pan = (uint8_t)((panTable[ch] & 0x0F) * 17);
        } else {
            c.pan = 128; // centre
        }
        c.mixPan = c.pan;
    }
    attachVoices(&_channels[0], sizeof(S3mChannel), MAX_CHANNELS, &_chunks[0][0], CHUNK_FRAMES);
    useSampleFile(_file);
    attachTails(_tails, (int)(sizeof(_tails) / sizeof(_tails[0])), _tailPool, TAIL_POOL_FRAMES);

    _globalVolume = globalVol > 64 ? 64 : globalVol;
    _openPatternNum = -1;
    _rowOffsetsValid = 0;
    resetSequencer(initialSpeed > 0 ? initialSpeed : 6, initialTempo > 0 ? initialTempo : 125);
    return true;
}

uint32_t S3mPlayer::s3mNoteToPeriod(uint8_t note, uint32_t c2spd) {
    int octave = note >> 4;
    int noteInOctave = note & 0x0F;
    int semitoneFromRef = (octave - 4) * 12 + noteInOctave; // reference: octave 4, note C
    float hz = (float)c2spd * powf(2.0f, (float)semitoneFromRef / 12.0f);
    if (hz < 1.0f) hz = 1.0f;
    float period = AMIGA_PAL_CLOCK * 2.0f / hz;
    if (period < 1.0f) period = 1.0f;
    if (period > (float)MAX_PERIOD) period = (float)MAX_PERIOD;
    return (uint32_t)(period + 0.5f);
}

void S3mPlayer::recomputeStep(S3mChannel& c, uint32_t period) {
    c.stepPeriod = period;
    if (period == 0) { c.sampleStep16 = 0; return; }
    float hz = AMIGA_PAL_CLOCK * 2.0f / (float)period;
    c.sampleStep16 = stepForHz(hz);
}

// Reads the CHUNK_FRAMES-aligned block holding `index` and converts it to
// signed 16-bit.
int16_t __not_in_flash_func(S3mPlayer::refillChunk)(TrackerVoice& v, uint32_t index) {
    const S3mSample& s = static_cast<const S3mSample&>(*v.smp);
    uint32_t bytesPerSample = s.is16Bit ? 2 : 1;
    uint32_t start = index & ~(uint32_t)(CHUNK_FRAMES - 1);
    uint32_t len = CHUNK_FRAMES;
    // A loop that fits in the cache is cached whole, so a wrap doesn't refill.
    if (v.looping && index >= s.loopStart && index < s.loopEnd && s.loopEnd - s.loopStart <= (uint32_t)CHUNK_FRAMES) {
        start = s.loopStart;
        len = s.loopEnd - s.loopStart;
    }
    if (start + len > s.length) len = s.length - start;
    v.chunkSmp = v.smp;
    v.chunkStart = start;
    // Frames past a short read or a failed seek stay zero and cached, so a
    // truncated sample doesn't hit the SD card on every frame.
    v.chunkLen = len;
    int16_t* out = v.chunkBuf;
    uint32_t got = 0;
    while (got < len) {
        uint32_t at = s.fileOffset + (start + got) * bytesPerSample;
        uint32_t piece = (len - got) * bytesPerSample;
        if (piece > rawSpan(at)) piece = rawSpan(at) & ~(bytesPerSample - 1);
        int n = 0;
        const uint8_t* raw = readSampleBytes(_file, at, piece, n);
        uint32_t frames = n > 0 ? (uint32_t)n / bytesPerSample : 0;
        int16_t* o = out + got;
        if (s.is16Bit) {
            for (uint32_t i = 0; i < frames; i++) {
                // Unbias in a wide type before narrowing.
                uint16_t u = (uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8);
                o[i] = s.isSigned ? (int16_t)u : (int16_t)((int32_t)u - 32768);
            }
        } else {
            for (uint32_t i = 0; i < frames; i++) {
                uint8_t b = raw[i];
                int16_t centred = s.isSigned ? (int8_t)b : (int16_t)((int)b - 128);
                o[i] = (int16_t)(centred << 8);
            }
        }
        got += frames;
        if (frames * bytesPerSample < piece) break;
    }
    for (uint32_t i = got; i < len; i++) out[i] = 0;
    return out[index - start];
}

void S3mPlayer::setSample(S3mChannel& c, uint8_t sample) {
    c.sample = sample;
    c.smp = &_samples[sample - 1];
}

void S3mPlayer::restartSamplePosition(int ch) {
    S3mChannel& c = _channels[ch];
    // The cache is kept: a retrigger of the same sample reuses it.
    if (c.smp) c.looping = c.smp->loopEnd > c.smp->loopStart;
    startVoiceAt(c, 0);
}

void S3mPlayer::triggerNote(int ch, uint8_t note) {
    S3mChannel& c = _channels[ch];
    if (c.sample >= 1 && c.sample <= MAX_INSTRUMENTS) {
        c.c2spd = (uint16_t)(_samples[c.sample - 1].c2spd > 65535 ? 65535 : _samples[c.sample - 1].c2spd);
    }
    c.period = s3mNoteToPeriod(note, c.c2spd);
    recomputeStep(c);
    restartSamplePosition(ch);
}

void S3mPlayer::openPatternForScan(int num) {
    _openPatternNum = num;
    _rowOffsetsValid = 0;
    _scanCursor = (num >= 0 && num < MAX_PATTERNS) ? _patternOffsets[num] + 2 : 0; // +2 skips the pattern's own length prefix
}

void S3mPlayer::advanceToRow(int targetRow) {
    if (targetRow < 0 || targetRow > 63) return;

    if (targetRow < _rowOffsetsValid) {
        // Already cached from an earlier visit to this pattern.
        _patternFile.seekSet(_rowOffsets[targetRow]);
        decodeRow(true);
        return;
    }

    while (_rowOffsetsValid <= targetRow && _rowOffsetsValid < 64) {
        _rowOffsets[_rowOffsetsValid] = _scanCursor;
        _patternFile.seekSet(_scanCursor);
        bool isTarget = (_rowOffsetsValid == targetRow);
        decodeRow(isTarget); // earlier rows: parsed only, to find their length; the target row: applied
        _scanCursor = _patternFile.curPosition();
        _rowOffsetsValid++;
    }
}

void S3mPlayer::decodeRow(bool apply) {
    if (_openPatternNum < 0 || _openPatternNum >= MAX_PATTERNS || _patternOffsets[_openPatternNum] == 0) {
        return; // empty/missing pattern slot -- a silent row, nothing to decode
    }
    // A row has at most one entry per channel (32) before its terminator,
    // so a corrupt file without a 0x00 can't make this read on and on.
    for (int guard = 0; guard <= 32; guard++) {
        uint8_t flag;
        if (_patternFile.read(&flag, 1) != 1) return; // truncated/EOF -- stop
        if (flag == 0) return; // row terminator

        int ch = flag & 0x1F;
        uint8_t note = 0xFF, sample = 0;
        bool hasNote = false;
        uint8_t volume = 0;
        bool hasVolume = false;
        uint8_t command = 0, param = 0;
        bool hasCommand = false;

        if (flag & 0x20) {
            uint8_t nb[2];
            if (_patternFile.read(nb, 2) != 2) return;
            note = nb[0];
            sample = nb[1];
            hasNote = true;
        }
        if (flag & 0x40) {
            uint8_t vb;
            if (_patternFile.read(&vb, 1) != 1) return;
            volume = vb;
            hasVolume = true;
        }
        if (flag & 0x80) {
            uint8_t cb[2];
            if (_patternFile.read(cb, 2) != 2) return;
            command = cb[0];
            param = cb[1];
            hasCommand = true;
        }

        if (apply && ch < MAX_CHANNELS && _channelEnabled[ch]) {
            applyCell(ch, note, sample, hasNote, volume, hasVolume, command, param, hasCommand);
        }
    }
}

void S3mPlayer::applyNoteColumns(int ch, uint8_t note, uint8_t sample, uint8_t volume, bool isTonePorta) {
    S3mChannel& c = _channels[ch];
    if (sample != 0) {
        setSample(c, sample);
        c.volume = _samples[sample - 1].volume;
        c.mixVolume = c.volume;
    }
    if (volume != 0xFF) {
        c.volume = (volume > 64) ? 64 : volume;
        c.mixVolume = c.volume;
    }
    // 0xFF is no note, 0xFE is note cut (^^), 0xFD and notes 12-15 of an
    // octave are invalid and ignored.
    if (note == 0xFE) {
        c.havePair = false;
    } else if (note < 0xFE && (note & 0x0F) <= 11) {
        if (isTonePorta) {
            c.portaTarget = s3mNoteToPeriod(note, c.c2spd);
        } else {
            triggerNote(ch, note);
        }
    }
}

void S3mPlayer::applyCell(int ch, uint8_t note, uint8_t sample, bool hasNote,
                            uint8_t volume, bool hasVolume, uint8_t command, uint8_t param, bool hasCommand) {
    S3mChannel& c = _channels[ch];

    // Command letters are stored 1-based (1=A .. 26=Z). Tone portamento
    // (G) and portamento+volslide (L) both mean "slide toward a target"
    // rather than "jump to it immediately", same distinction MOD's 3xx/
    // 5xy make from a plain note trigger.
    bool isTonePorta = hasCommand && (command == 7 /* G */ || command == 12 /* L */);
    bool isNoteDelay = hasCommand && command == 19 /* S */ && (param >> 4) == 0xD && (param & 0x0F) != 0;

    uint8_t cellNote = hasNote ? note : 0xFF;
    uint8_t cellSample = (hasNote && sample >= 1 && sample <= MAX_INSTRUMENTS) ? sample : 0;
    uint8_t cellVolume = hasVolume ? volume : 0xFF;
    if (isNoteDelay) {
        // SDy holds the whole cell (instrument, volume and note) until tick y.
        c.noteDelayParam = param & 0x0F;
        c.pendingNote = cellNote;
        c.pendingSample = cellSample;
        c.pendingVolume = cellVolume;
    } else {
        applyNoteColumns(ch, cellNote, cellSample, cellVolume, isTonePorta);
    }

    if (!hasCommand) return;

    switch (command) {
        case 1: // A - Set speed
            if (param > 0) _speed = param;
            break;
        case 2: // B - Position jump
            _positionJumpPending = true;
            _positionJumpTarget = param;
            break;
        case 3: // C - Pattern break (BCD row number, same convention MOD's Dxx uses)
            _patternBreakPending = true;
            _patternBreakRow = (param >> 4) * 10 + (param & 0x0F);
            break;
        case 4: { // D - Volume slide: regular (applied per-tick, see processTickEffects(),
                  // gated by volSlideActiveRow so it stops the row after this
                  // one unless re-specified), or one-shot "fine" via a 0xF
                  // nibble (applied once, here, needs no gating).
            c.volSlideActiveRow = true;
            if (param != 0) c.volSlideParam = param;
            uint8_t hi = c.volSlideParam >> 4, lo = c.volSlideParam & 0x0F;
            if (lo == 0x0F && hi != 0 && hi != 0x0F) {
                c.volume = (uint8_t)min(64, (int)c.volume + hi);
                c.mixVolume = c.volume;
            } else if (hi == 0x0F && lo != 0) {
                c.volume = (uint8_t)max(0, (int)c.volume - lo);
                c.mixVolume = c.volume;
            }
            break;
        }
        case 5: // E - Portamento down: Exx slides xx*4 period units a tick
                // (ticks 1+), EFx slides x*4 once and EEx (extra fine) x once.
            c.portaDownActiveRow = true;
            if (param != 0) c.portaDownSpeed = param;
            if (c.period != 0 && (c.portaDownSpeed & 0xF0) >= 0xE0) {
                uint32_t amount = (c.portaDownSpeed & 0x0F) * ((c.portaDownSpeed & 0xF0) == 0xF0 ? 4u : 1u);
                c.period = min(MAX_PERIOD, c.period + amount);
                recomputeStep(c);
            }
            break;
        case 6: // F - Portamento up (mirror of E)
            c.portaUpActiveRow = true;
            if (param != 0) c.portaUpSpeed = param;
            if (c.period != 0 && (c.portaUpSpeed & 0xF0) >= 0xE0) {
                uint32_t amount = (c.portaUpSpeed & 0x0F) * ((c.portaUpSpeed & 0xF0) == 0xF0 ? 4u : 1u);
                c.period = c.period > amount + 1 ? c.period - amount : 1;
                recomputeStep(c);
            }
            break;
        case 7: // G - Tone portamento (target already set above)
            c.tonePortaActiveRow = true;
            if (param != 0) c.tonePortaSpeed = param;
            break;
        case 8: // H - Vibrato
            c.vibratoActiveRow = true;
            if (param >> 4) c.vibratoSpeed = param >> 4;
            if (param & 0x0F) c.vibratoDepth = param & 0x0F;
            break;
        case 9: // I - Tremor: on for (high nibble + 1) ticks, off for (low nibble + 1) ticks, repeating
            c.tremorActiveRow = true;
            if (param != 0) {
                c.tremorOnParam = (param >> 4) + 1;
                c.tremorOffParam = (param & 0x0F) + 1;
            }
            break;
        case 10: // J - Arpeggio (reset each row, see advanceRow())
            c.arpeggioParam = param;
            break;
        case 11: // K - Vibrato + volume slide -- both components active this row
            c.vibratoActiveRow = true;
            c.volSlideActiveRow = true;
            if (param != 0) c.volSlideParam = param;
            break;
        case 12: // L - Tone portamento + volume slide (target already set above if a note was given)
            c.tonePortaActiveRow = true;
            c.volSlideActiveRow = true;
            if (param != 0) c.volSlideParam = param;
            break;
        case 17: // Q - Retrigger + volume slide
            c.retrigActiveRow = true;
            if (param != 0) {
                c.retrigVolSlide = param >> 4;
                c.retrigParam = param & 0x0F;
            }
            break;
        case 18: // R - Tremolo
            c.tremoloActiveRow = true;
            if (param >> 4) c.tremoloSpeed = param >> 4;
            if (param & 0x0F) c.tremoloDepth = param & 0x0F;
            break;
        case 19: // S - Special (sub-commands; SDy note-delay already handled above)
            if (!isNoteDelay) processExtendedTickZero(ch, command, param);
            break;
        case 20: // T - Set tempo
            if (param > 0) _tempo = param;
            break;
        case 21: // U - Fine vibrato (same shape as H, quarter depth -- see processTickEffects())
            c.vibratoActiveRow = true;
            if (param >> 4) c.vibratoSpeed = param >> 4;
            if (param & 0x0F) c.vibratoDepth = 0x80 | (param & 0x0F); // high bit flags "fine" for processTickEffects()
            break;
        case 22: // V - Set global volume
            _globalVolume = (param > 64) ? 64 : param;
            break;
        case 23: // W - Global volume slide -- own memory (_globalVolSlideParam),
                 // not a per-channel effect despite being attached to one channel's row slot
            _globalVolSlideActiveRow = true;
            if (param != 0) _globalVolSlideParam = param;
            break;
        case 24: // X - Set panning
            c.pan = param;
            break;
        case 25: // Y - Panbrello
            c.panbrelloActiveRow = true;
            if (param >> 4) c.panbrelloSpeed = param >> 4;
            if (param & 0x0F) c.panbrelloDepth = param & 0x0F;
            break;
        default:
            break; // M/N/O/P reserved/unused in classic ST3; Z (MIDI macro) out of scope, see header comment
    }
}

void S3mPlayer::processExtendedTickZero(int ch, uint8_t command, uint8_t param) {
    S3mChannel& c = _channels[ch];
    uint8_t sub = param >> 4;
    uint8_t val = param & 0x0F;
    (void)command; // always 19 (S) here, kept as a parameter for symmetry with the call site

    switch (sub) {
        case 0x1: // S1y - Glissando control
            c.glissando = (val != 0);
            break;
        case 0x8: // S8y - Set panning, coarse (0-15 -> 0-255)
            c.pan = (uint8_t)(val * 17);
            break;
        case 0xB: // SBy - Pattern loop
            patternLoop(c.patternLoopRow, c.patternLoopCount, val);
            break;
        case 0xC: // SCy - Note cut
            c.noteCutParam = val;
            break;
        case 0xE: // SEy - Pattern delay: hold the current row for `val` extra tick-cycles
            _patternDelayRepeatsLeft = val;
            break;
        case 0x3: // S3y - Set vibrato waveform -- only sine implemented, no-op otherwise
        case 0x4: // S4y - Set tremolo waveform -- same
        case 0x0: // S0y - Set filter -- no hardware filter to toggle here
        default:
            break;
    }
}

void S3mPlayer::advanceRow() {
    _globalVolSlideActiveRow = false;
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (!_channelEnabled[ch]) continue;
        S3mChannel& c = _channels[ch];
        c.arpeggioParam = 0;
        c.noteDelayParam = 0;
        c.noteCutParam = 0xFF;
        // These gate whether processTickEffects() actually applies the
        // corresponding continuing effect -- see their declarations in
        // S3mChannel for why this is separate from the remembered param
        // values (volSlideParam etc.), which stay untouched here.
        c.volSlideActiveRow = false;
        c.portaDownActiveRow = false;
        c.portaUpActiveRow = false;
        c.tonePortaActiveRow = false;
        c.vibratoActiveRow = false;
        c.tremoloActiveRow = false;
        c.retrigActiveRow = false;
        c.tremorActiveRow = false;
        c.panbrelloActiveRow = false;
        c.panbrelloOffset = 0;
    }

    stepPosition(64, 63);

    // Order table: 0xFF ends the song outright. 0xFE marks a skipped
    // position -- silently advance past it. S3M has no dedicated
    // "restart position" byte the way MOD does; a trailing position-jump
    // (B) effect serves the identical looping purpose in practice, and
    // is already handled generically above (it's just another jump).
    while (true) {
        if (_orderPos < 0 || _orderPos >= _songLength) {
            _state = STATE_DONE;
            return;
        }
        uint8_t marker = _orderTable[_orderPos];
        if (marker == 0xFF) {
            _state = STATE_DONE;
            return;
        }
        if (marker == 0xFE) {
            _orderPos++;
            _row = 0;
            continue;
        }
        break;
    }

    int patternNum = _orderTable[_orderPos];
    if (patternNum != _openPatternNum) openPatternForScan(patternNum);
    advanceToRow(_row);

    // A row starts at the base pitch and volume, dropping last tick's
    // vibrato, arpeggio, tremolo or tremor; tremor counts this tick too.
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (!_channelEnabled[ch]) continue;
        S3mChannel& c = _channels[ch];
        if (c.period == 0) continue;
        if (c.stepPeriod != c.period) recomputeStep(c);
        c.mixVolume = tremorStep(c) ? 0 : c.volume;
    }
}

// Tremor (I), on the rows that give it, row tick included: true for the
// off part of each on/off cycle, where mixVolume is muted. The sample keeps
// playing underneath.
bool S3mPlayer::tremorStep(S3mChannel& c) {
    if (!c.tremorActiveRow) return false;
    uint8_t cycleLen = c.tremorOnParam + c.tremorOffParam;
    if (cycleLen == 0) return false;
    bool muted = (c.tremorCounter % cycleLen) >= c.tremorOnParam;
    c.tremorCounter++;
    return muted;
}

void S3mPlayer::processTickEffects() {
    // Global volume slide (W), on the rows that give it. Same shape as a
    // channel's D without the fine variants.
    if (_globalVolSlideActiveRow) {
        uint8_t hi = _globalVolSlideParam >> 4, lo = _globalVolSlideParam & 0x0F;
        if (hi != 0x0F && lo != 0x0F) {
            if (hi) _globalVolume = (uint8_t)min(64, (int)_globalVolume + hi);
            else if (lo) _globalVolume = (uint8_t)max(0, (int)_globalVolume - lo);
        }
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (!_channelEnabled[ch]) continue;
        S3mChannel& c = _channels[ch];

        if (c.noteDelayParam != 0 && _tick == c.noteDelayParam) {
            applyNoteColumns(ch, c.pendingNote, c.pendingSample, c.pendingVolume, false);
            c.noteDelayParam = 0;
        }
        if (c.noteCutParam != 0xFF && _tick == c.noteCutParam) {
            c.volume = 0;
            c.noteCutParam = 0xFF;
        }
        if (c.retrigActiveRow && c.retrigParam > 0 && (_tick % c.retrigParam) == 0) {
            restartSamplePosition(ch); // same pitch, position back to 0 -- not a fresh triggerNote()
            // Qxy's volume-slide-type nibble: an approximation, not a
            // bit-exact reproduction of ST3's own table -- 1-7 nudge
            // volume down, 9-15 nudge it up, 0/8 leave it unchanged.
            // This is a genuinely obscure combined effect; documented
            // here as a deliberate simplification rather than guessed
            // silently.
            if (c.retrigVolSlide >= 1 && c.retrigVolSlide <= 7) {
                c.volume = (uint8_t)max(0, (int)c.volume - (int)c.retrigVolSlide);
            } else if (c.retrigVolSlide >= 9) {
                c.volume = (uint8_t)min(64, (int)c.volume + (int)(c.retrigVolSlide - 8));
            }
        }

        if (c.period == 0) continue;

        bool tremorMuted = tremorStep(c);

        // Volume slide (D, and the volslide half of K/L) -- regular
        // per-tick, gated by volSlideActiveRow (see its declaration --
        // must be given each row to continue, not just remembered);
        // 0xF-nibble ("fine") combinations are one-shot and already
        // applied in applyCell(), skipped here. Mutates the persistent
        // c.volume (unlike tremolo/tremor below, a slide is a real,
        // lasting change).
        if (c.volSlideActiveRow) {
            uint8_t slideUp = c.volSlideParam >> 4, slideDown = c.volSlideParam & 0x0F;
            if (slideUp != 0x0F && slideDown != 0x0F) {
                if (slideUp) c.volume = (uint8_t)min(64, (int)c.volume + slideUp);
                else if (slideDown) c.volume = (uint8_t)max(0, (int)c.volume - slideDown);
            }
        }
        // Fresh mix volume each tick; tremolo and tremor below only
        // adjust mixVolume, never the persistent volume.
        c.mixVolume = c.volume;

        // Portamento up/down (E/F) on the rows that give them, xx*4 period
        // units a tick, clamped so a long slide can't run off either end.
        if (c.portaDownActiveRow && c.portaDownSpeed && (c.portaDownSpeed & 0xF0) < 0xE0) {
            uint32_t p = c.period + (uint32_t)c.portaDownSpeed * 4;
            if (p > MAX_PERIOD) p = MAX_PERIOD;
            c.period = p;
            recomputeStep(c);
        }
        if (c.portaUpActiveRow && c.portaUpSpeed && (c.portaUpSpeed & 0xF0) < 0xE0) {
            uint32_t amount = (uint32_t)c.portaUpSpeed * 4;
            c.period = c.period > amount + 1 ? c.period - amount : 1;
            recomputeStep(c);
        }

        // Tone portamento (G, and the portamento half of L) -- slides
        // toward portaTarget at tonePortaSpeed/tick, gated by
        // tonePortaActiveRow.
        if (c.tonePortaActiveRow && c.portaTarget != 0 && c.tonePortaSpeed) {
            uint32_t speed = (uint32_t)c.tonePortaSpeed * 4;
            if (c.period < c.portaTarget) {
                c.period = min(c.portaTarget, c.period + speed);
            } else if (c.period > c.portaTarget) {
                c.period = c.period > c.portaTarget + speed ? c.period - speed : c.portaTarget;
            }
            recomputeStep(c);
        }

        // Vibrato (H/U, and the vibrato half of K) and arpeggio (J) only
        // change the pitch that plays this tick, c.period stays the base.
        // U is H at a quarter of the depth, flagged by vibratoDepth's high bit.
        uint32_t playPeriod = c.period;
        bool fineVibrato = (c.vibratoDepth & 0x80) != 0;
        uint8_t vibDepth = c.vibratoDepth & 0x7F;
        if (c.vibratoActiveRow && vibDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.vibratoPos / 64.0f;
            // 8 period units a depth step at the peak (2 for U), as OpenMPT plays ST3.
            int delta = (int)(sinf(angle) * vibDepth * (fineVibrato ? 2.0f : 8.0f));
            playPeriod = (uint32_t)max(1, (int)c.period + delta);
            c.vibratoPos = (uint8_t)((c.vibratoPos + c.vibratoSpeed) & 0x3F);
        }
        // Arpeggio: base note, +x semitones, +y semitones, one per tick.
        if (c.arpeggioParam != 0) {
            int cyclePos = _tick % 3;
            int semis = (cyclePos == 1) ? (c.arpeggioParam >> 4) : (cyclePos == 2) ? (c.arpeggioParam & 0x0F) : 0;
            if (semis != 0) {
                float factor = powf(2.0f, -(float)semis / 12.0f);
                playPeriod = (uint32_t)max(1.0f, (float)c.period * factor);
            }
        }
        if (playPeriod != c.stepPeriod) recomputeStep(c, playPeriod);

        // Tremolo (R) -- same idea as vibrato, transient volume offset,
        // gated by tremoloActiveRow -- and tremor's mute gate, both
        // applied to mixVolume only, so neither ever leaks into the
        // persistent c.volume a real slide effect (above) or the next
        // row's instrument/volume-column trigger operates on.
        if (c.tremoloActiveRow && c.tremoloDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.tremoloPos / 64.0f;
            int delta = (int)(sinf(angle) * c.tremoloDepth * 4.0f);
            int v = (int)c.mixVolume + delta;
            if (v < 0) v = 0; else if (v > 64) v = 64;
            c.mixVolume = (uint8_t)v;
            c.tremoloPos = (uint8_t)((c.tremoloPos + c.tremoloSpeed) & 0x3F);
        }
        if (tremorMuted) c.mixVolume = 0;

        // Panbrello (Y), on the rows that give it: an offset tickEnd() adds
        // to pan for the mix, pan itself stays put.
        // OpenMPT's shape: 64 steps a cycle, up to 8*depth either side.
        if (c.panbrelloActiveRow && c.panbrelloDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.panbrelloPos / 64.0f;
            c.panbrelloOffset = (int8_t)(sinf(angle) * c.panbrelloDepth * 8.0f);
            c.panbrelloPos = (uint8_t)((c.panbrelloPos + c.panbrelloSpeed) & 0x3F);
        }
    }
}

void S3mPlayer::tickEnd() {
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        S3mChannel& c = _channels[ch];
        int p = (int)c.pan + c.panbrelloOffset;
        c.mixPan = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
    }
}

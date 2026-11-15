#include "sd_card.h"
#include "xm_file.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>

namespace {

// Same Amiga clock MOD/S3M use, see xmNoteToPeriod(). float: no FPU.
const float AMIGA_PAL_CLOCK = 7093789.2f;

} // namespace

bool XmPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!sdOpenPath(_file, path, O_RDONLY)) return loadFail("could not open file");
    if (!sdOpenPath(_patternFile, path, O_RDONLY)) return loadFail("could not open file");

    uint8_t hdr[60];
    if (_file.read(hdr, 60) != 60 || memcmp(hdr, "Extended Module: ", 17) != 0 || hdr[37] != 0x1A) return loadFail("unsupported format");

    uint8_t sizeBuf[4];
    if (_file.read(sizeBuf, 4) != 4) return loadFail("invalid XM file");
    // Measured FROM this field's own start (offset 60) -- so pattern data
    // begins at 60+headerSize regardless of how much of the declared
    // header block the fixed fields below actually consume. Always trust
    // this field over any fixed-offset assumption -- see this class's
    // header comment on the instrument-size field's identical convention.
    uint32_t headerSize = (uint32_t)sizeBuf[0] | ((uint32_t)sizeBuf[1] << 8) |
                           ((uint32_t)sizeBuf[2] << 16) | ((uint32_t)sizeBuf[3] << 24);

    uint8_t fb[16];
    if (_file.read(fb, 16) != 16) return loadFail("invalid XM file");
    uint16_t songLenReal = (uint16_t)fb[0] | ((uint16_t)fb[1] << 8);
    // restartPos at fb[2]/fb[3] -- unused, see this class's header
    // comment on "loop until stopped" (matches MOD/S3M's convention).
    uint16_t numChannelsReal = (uint16_t)fb[4] | ((uint16_t)fb[5] << 8);
    uint16_t numPatternsReal = (uint16_t)fb[6] | ((uint16_t)fb[7] << 8);
    uint16_t numInstrumentsReal = (uint16_t)fb[8] | ((uint16_t)fb[9] << 8);
    uint16_t flags = (uint16_t)fb[10] | ((uint16_t)fb[11] << 8);
    uint16_t defaultTempo = (uint16_t)fb[12] | ((uint16_t)fb[13] << 8);
    uint16_t defaultBpm = (uint16_t)fb[14] | ((uint16_t)fb[15] << 8);

    // Sanity bound against a corrupt file claiming an absurd count --
    // same philosophy as S3mPlayer::load()'s identical guard.
    if (numChannelsReal < 1 || numChannelsReal > 128 || numPatternsReal > 4096 || numInstrumentsReal > 4096) return loadFail("invalid XM file");

    _linearFreqTable = (flags & 0x01) != 0;
    _fileNumChannels = numChannelsReal;
    _numChannels = (numChannelsReal < (uint16_t)MAX_CHANNELS) ? (int)numChannelsReal : MAX_CHANNELS;

    // Order table: always exactly 256 bytes regardless of songLen.
    if (_file.read(_orderTable, MAX_ORDERS) != MAX_ORDERS) return loadFail("invalid XM file");
    _songLength = (songLenReal < (uint16_t)MAX_ORDERS) ? (int)songLenReal : MAX_ORDERS;

    if (!_file.seekSet(60 + headerSize)) return loadFail("invalid XM file");

    // Slots the scan below doesn't reach stay empty 64-row patterns.
    for (int i = 0; i < MAX_PATTERNS; i++) {
        _patternEmpty[i] = true;
        _patternRowCounts[i] = 64;
    }

    // Pattern headers: sequential walk, one time -- XM patterns aren't
    // parapointer-addressable (see this class's header comment).
    for (int i = 0; i < (int)numPatternsReal; i++) {
        uint8_t ph[9];
        if (_file.read(ph, 9) != 9) break; // truncated file -- stop scanning, whatever was found stands
        uint32_t patHeaderLen = (uint32_t)ph[0] | ((uint32_t)ph[1] << 8) | ((uint32_t)ph[2] << 16) | ((uint32_t)ph[3] << 24);
        uint16_t numRows = (uint16_t)ph[5] | ((uint16_t)ph[6] << 8);
        uint16_t packedSize = (uint16_t)ph[7] | ((uint16_t)ph[8] << 8);
        if (patHeaderLen > 9) _file.seekCur(patHeaderLen - 9);

        uint32_t cellDataStart = _file.curPosition();
        if (i < MAX_PATTERNS) {
            _patternOffsets[i] = cellDataStart;
            _patternRowCounts[i] = (numRows > 0 && numRows <= MAX_ROWS) ? numRows : 64;
            _patternEmpty[i] = (packedSize == 0);
        }
        _file.seekSet(cellDataStart + packedSize);
    }

    // Instruments: sequential walk, one time. Each block's real total
    // size is isize + numSamples*sampleHeaderSize + sum(sample lengths)
    // -- NOT just isize -- see this class's header comment on this
    // verified correction.
    _numInstruments = 0;
    _numSamplesTotal = 0;
    for (int i = 0; i < (int)numInstrumentsReal; i++) {
        uint32_t instStart = _file.curPosition();
        uint8_t ib[4];
        if (_file.read(ib, 4) != 4) break;
        uint32_t isize = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8) | ((uint32_t)ib[2] << 16) | ((uint32_t)ib[3] << 24);

        uint8_t nb[25]; // name(22) + type(1) + numSamples(2)
        if (_file.read(nb, 25) != 25) break;
        uint16_t numSamplesReal = (uint16_t)nb[23] | ((uint16_t)nb[24] << 8);

        uint32_t sampleHeaderSize = 40;
        uint8_t keymapBuf[96] = {0};
        uint8_t volEnvRaw[48] = {0}, panEnvRaw[48] = {0};
        uint8_t singleBytes[14] = {0};
        uint16_t volFadeout = 0;

        if (numSamplesReal > 0) {
            uint8_t shb[4];
            if (_file.read(shb, 4) != 4) break;
            sampleHeaderSize = (uint32_t)shb[0] | ((uint32_t)shb[1] << 8) | ((uint32_t)shb[2] << 16) | ((uint32_t)shb[3] << 24);
            if (_file.read(keymapBuf, 96) != 96) break;
            if (_file.read(volEnvRaw, 48) != 48) break;
            if (_file.read(panEnvRaw, 48) != 48) break;
            if (_file.read(singleBytes, 14) != 14) break;
            uint8_t fadeBuf[2];
            if (_file.read(fadeBuf, 2) != 2) break;
            volFadeout = (uint16_t)fadeBuf[0] | ((uint16_t)fadeBuf[1] << 8);
        }

        // Sanity bound -- see MAX_SAMPLES's comment; no real instrument
        // approaches this (surveyed p95 was 16 samples in one instrument).
        if (numSamplesReal > 64) return loadFail("invalid XM file");

        // Defensive: seek to where the declared instrument size says the
        // sample headers actually start, regardless of how many bytes the
        // fixed fields above consumed (real files pad with zeros out to
        // isize -- see this class's header comment).
        _file.seekSet(instStart + isize);

        if (i < MAX_INSTRUMENTS) {
            XmInstrument& inst = _instruments[i];
            inst = XmInstrument();
            if (numSamplesReal > 0) {
                memcpy(inst.keymap, keymapBuf, 96);
                inst.volFadeout = volFadeout;

                XmEnvelope& ve = inst.volEnv;
                XmEnvelope& pe = inst.panEnv;
                // Values are 0-64; anything above would overflow the mix volume.
                for (int p = 0; p < MAX_ENV_POINTS; p++) {
                    ve.pointX[p] = (uint16_t)volEnvRaw[p * 4] | ((uint16_t)volEnvRaw[p * 4 + 1] << 8);
                    ve.pointY[p] = min((uint16_t)64, (uint16_t)((uint16_t)volEnvRaw[p * 4 + 2] | ((uint16_t)volEnvRaw[p * 4 + 3] << 8)));
                    pe.pointX[p] = (uint16_t)panEnvRaw[p * 4] | ((uint16_t)panEnvRaw[p * 4 + 1] << 8);
                    pe.pointY[p] = min((uint16_t)64, (uint16_t)((uint16_t)panEnvRaw[p * 4 + 2] | ((uint16_t)panEnvRaw[p * 4 + 3] << 8)));
                }
                ve.numPoints = (singleBytes[0] <= MAX_ENV_POINTS) ? singleBytes[0] : (uint8_t)MAX_ENV_POINTS;
                pe.numPoints = (singleBytes[1] <= MAX_ENV_POINTS) ? singleBytes[1] : (uint8_t)MAX_ENV_POINTS;
                ve.sustainPoint = singleBytes[2];
                ve.loopStartPoint = singleBytes[3];
                ve.loopEndPoint = singleBytes[4];
                pe.sustainPoint = singleBytes[5];
                pe.loopStartPoint = singleBytes[6];
                pe.loopEndPoint = singleBytes[7];
                ve.flags = singleBytes[8];
                pe.flags = singleBytes[9];
                // singleBytes[10..13] = vibType/vibSweep/vibDepth/vibRate
                // (instrument auto-vibrato) -- intentionally out of
                // scope, see this class's header comment.
            }
            inst.firstSampleIndex = (uint8_t)_numSamplesTotal;
        }

        // Sample headers, then each sample's PCM data, back to back --
        // must walk past every one of them regardless of MAX_SAMPLES pool
        // truncation, to reach the next instrument correctly.
        struct TempSampleInfo {
            uint32_t lengthBytes = 0, loopStartBytes = 0, loopLengthBytes = 0;
            uint8_t volume = 64;
            int8_t finetune = 0;
            uint8_t flags = 0;
            uint8_t panning = 128;
            int8_t relativeNote = 0;
        };
        TempSampleInfo temp[64];
        int numSamplesParsed = (int)numSamplesReal;
        for (int k = 0; k < numSamplesParsed; k++) {
            uint32_t shStart = _file.curPosition();
            uint8_t sb[18];
            if (_file.read(sb, 18) != 18) { numSamplesParsed = k; break; }
            temp[k].lengthBytes = (uint32_t)sb[0] | ((uint32_t)sb[1] << 8) | ((uint32_t)sb[2] << 16) | ((uint32_t)sb[3] << 24);
            temp[k].loopStartBytes = (uint32_t)sb[4] | ((uint32_t)sb[5] << 8) | ((uint32_t)sb[6] << 16) | ((uint32_t)sb[7] << 24);
            temp[k].loopLengthBytes = (uint32_t)sb[8] | ((uint32_t)sb[9] << 8) | ((uint32_t)sb[10] << 16) | ((uint32_t)sb[11] << 24);
            temp[k].volume = sb[12];
            temp[k].finetune = (int8_t)sb[13];
            temp[k].flags = sb[14];
            temp[k].panning = sb[15];
            temp[k].relativeNote = (int8_t)sb[16];
            // sb[17] = reserved; the 22-byte sample name that follows is
            // intentionally unread -- not used for playback, and this
            // reserved/name split is the one detail this class's header
            // comment flags as still wanting one more real-byte check.
            _file.seekSet(shStart + sampleHeaderSize);
        }

        for (int k = 0; k < numSamplesParsed; k++) {
            uint32_t pcmStart = _file.curPosition();
            bool storeIt = (i < MAX_INSTRUMENTS) && (_numSamplesTotal < MAX_SAMPLES);
            if (storeIt) {
                XmSample& s = _samples[_numSamplesTotal];
                s = XmSample();
                bool is16Bit = (temp[k].flags & 0x10) != 0;
                uint8_t loopType = temp[k].flags & 0x03;
                uint32_t bytesPerFrame = is16Bit ? 2 : 1;
                s.fileOffset = pcmStart;
                s.length = temp[k].lengthBytes / bytesPerFrame;
                s.loopStart = temp[k].loopStartBytes / bytesPerFrame;
                s.loopLength = temp[k].loopLengthBytes / bytesPerFrame;
                s.loopEnd = s.loopStart + s.loopLength;
                s.is16Bit = is16Bit;
                // Pingpong (loopType==2) treated as forward -- see
                // XmSample::looping's comment.
                s.looping = (loopType != 0) && s.loopLength > 0 && s.loopStart < s.length;
                s.volume = temp[k].volume > 64 ? 64 : temp[k].volume;
                s.finetune = temp[k].finetune;
                s.panning = temp[k].panning;
                s.relativeNoteNumber = temp[k].relativeNote;

                if (s.looping) {
                    // Precompute the accumulator anchor at the loop start
                    // -- see decodeDeltaRun()'s comment.
                    s.accumAtLoopStart = decodeDeltaRun(s.fileOffset, s.loopStart, s.is16Bit, 0);
                }

                if (i < MAX_INSTRUMENTS) _instruments[i].numSamplesUsed++;
                _numSamplesTotal++;
            }
            _file.seekSet(pcmStart + temp[k].lengthBytes);
        }

        if (i < MAX_INSTRUMENTS && numSamplesReal > 0) _numInstruments++;
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        XmChannel& c = _channels[ch];
        c = XmChannel();
        c.smp = (c.sampleIndex < _numSamplesTotal) ? &_samples[c.sampleIndex] : nullptr;
    }
    attachVoices(&_channels[0], sizeof(XmChannel), _numChannels, &_chunks[0][0], CHUNK_FRAMES);
    useSampleFile(_file);
    attachTails(_tails, (int)(sizeof(_tails) / sizeof(_tails[0])), _tailPool, TAIL_POOL_FRAMES);

    _globalVolume = 64;
    _globalVolSlideParam = 0;
    _globalVolSlideActiveRow = false;
    _openPatternNum = -1;
    _openPatternEmpty = false;
    _rowOffsetsValid = 0;
    resetSequencer(defaultTempo > 0 ? defaultTempo : 6, defaultBpm > 0 ? defaultBpm : 125);
    return true;
}

// note is an absolute, 0-based semitone position (pattern note - 1 plus
// the sample's relativeNoteNumber). Periods are in FT2's units in both
// modes, so slides move them by the same amounts FT2 does:
//  - linear: 7680 - note*64 - finetune/2, 768 units an octave;
//  - Amiga: 4x an Amiga period, anchored so C-4 (note 48) with finetune 0
//    is 8363 Hz like linear mode.
uint16_t XmPlayer::xmNoteToPeriod(int note, int8_t finetune) const {
    if (_linearFreqTable) {
        int period = 7680 - note * 64 - (int)finetune / 2;
        if (period < 1) period = 1;
        if (period > 65535) period = 65535;
        return (uint16_t)period;
    } else {
        const float AMIGA_ANCHOR_PERIOD = AMIGA_PAL_CLOCK * 2.0f / 8363.0f;
        float exponent = -((float)note + (float)finetune / 128.0f - 48.0f) / 12.0f;
        float period = AMIGA_ANCHOR_PERIOD * powf(2.0f, exponent);
        if (period < 1.0f) period = 1.0f;
        if (period > 65535.0f) period = 65535.0f;
        return (uint16_t)(period + 0.5f);
    }
}

float XmPlayer::periodToFreq(uint16_t period) const {
    if (period == 0) return 0.0f;
    if (_linearFreqTable) {
        return 8363.0f * powf(2.0f, (4608.0f - (float)period) / 768.0f);
    } else {
        return AMIGA_PAL_CLOCK * 2.0f / (float)period;
    }
}

void XmPlayer::recomputeStep(XmChannel& c, uint16_t period) {
    c.stepPeriod = period;
    if (period == 0) { c.sampleStep16 = 0; return; }
    c.sampleStep16 = stepForHz(periodToFreq(period));
}

int32_t XmPlayer::decodeDeltaRun(uint32_t fileOffset, uint32_t frameCount, bool is16Bit, int32_t startAccum) {
    if (frameCount == 0) return startAccum;
    uint32_t bytesPerFrame = is16Bit ? 2 : 1;
    if (!_file.seekSet(fileOffset)) return startAccum;
    int32_t accum = startAccum;
    uint32_t remaining = frameCount;
    while (remaining > 0) {
        uint32_t most = RAW_SCRATCH_BYTES / bytesPerFrame;
        uint32_t chunkFrames = remaining > most ? most : remaining;
        int n = _file.read(_rawScratch, chunkFrames * bytesPerFrame);
        if (n <= 0) break;
        uint32_t framesGot = (uint32_t)n / bytesPerFrame;
        for (uint32_t i = 0; i < framesGot; i++) {
            if (is16Bit) {
                int16_t delta = (int16_t)((uint16_t)_rawScratch[i * 2] | ((uint16_t)_rawScratch[i * 2 + 1] << 8));
                accum = (int16_t)(accum + delta);
            } else {
                int8_t delta = (int8_t)_rawScratch[i];
                accum = (int8_t)(accum + delta);
            }
        }
        remaining -= framesGot;
        if (framesGot < chunkFrames) break; // short read / EOF
    }
    return accum;
}

// Refills v's cache with decoded frames starting at `index`. A plain
// continuation reuses the carried accumulator; index 0 and loopStart have
// known accumulators; a small gap past one of those is decoded in the same
// read; anything further does a separate catch-up decode first.
int16_t __not_in_flash_func(XmPlayer::refillChunk)(TrackerVoice& v, uint32_t index) {
    const XmSample& s = static_cast<const XmSample&>(*v.smp);
    uint32_t loopEnd = s.loopEnd;
    uint32_t bytesPerFrame = s.is16Bit ? 2 : 1;
    uint32_t windowLimit = (v.looping && index < loopEnd) ? loopEnd : s.length;
    uint32_t windowFrames = (uint32_t)CHUNK_FRAMES;
    if (index + windowFrames > windowLimit) windowFrames = windowLimit - index;
    if (windowFrames == 0) { v.chunkLen = 0; return 0; }

    int32_t startAccum;
    uint32_t readStart = index; // where the SD read begins, see the small-gap fold-in
    if (v.chunkLen != 0 && v.chunkSmp == v.smp && index == v.chunkStart + v.chunkLen) {
        startAccum = v.chunkAccum;
    } else if (index == 0) {
        startAccum = 0;
    } else if (v.looping && index == s.loopStart) {
        startAccum = s.accumAtLoopStart;
    } else {
        // A loop wrap usually lands 1-3 frames past loopStart: fold that
        // gap into this read instead of a second seek. Bigger gaps (9xx)
        // decode up to the index first.
        uint32_t anchor = (v.looping && index >= s.loopStart) ? s.loopStart : 0;
        int32_t anchorAccum = (anchor == s.loopStart) ? s.accumAtLoopStart : 0;
        uint32_t gap = index - anchor;
        const uint32_t SMALL_GAP_FOLD_LIMIT = 64;
        if (gap > 0 && gap <= SMALL_GAP_FOLD_LIMIT) {
            readStart = anchor;
            startAccum = anchorAccum;
            if (windowFrames > (uint32_t)CHUNK_FRAMES - gap) windowFrames = (uint32_t)CHUNK_FRAMES - gap;
        } else {
            startAccum = (gap > 0) ? decodeDeltaRun(s.fileOffset + anchor * bytesPerFrame, gap, s.is16Bit, anchorAccum)
                                    : anchorAccum;
        }
    }

    uint32_t skipFrames = index - readStart; // decoded and dropped when a small gap was folded in
    uint32_t totalFrames = windowFrames + skipFrames;
    int32_t accum = startAccum;
    uint32_t stored = 0, done = 0;
    while (done < totalFrames) {
        uint32_t at = s.fileOffset + (readStart + done) * bytesPerFrame;
        uint32_t piece = (totalFrames - done) * bytesPerFrame;
        if (piece > rawSpan(at)) piece = rawSpan(at) & ~(bytesPerFrame - 1);
        int n = 0;
        const uint8_t* raw = readSampleBytes(_file, at, piece, n);
        uint32_t framesGot = n > 0 ? (uint32_t)n / bytesPerFrame : 0;
        for (uint32_t i = 0; i < framesGot; i++) {
            int16_t decoded;
            if (s.is16Bit) {
                int16_t delta = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
                accum = (int16_t)(accum + delta);
                decoded = (int16_t)accum;
            } else {
                int8_t delta = (int8_t)raw[i];
                accum = (int8_t)(accum + delta);
                decoded = (int16_t)((int16_t)(int8_t)accum << 8);
            }
            if (done + i >= skipFrames) v.chunkBuf[stored++] = decoded;
        }
        done += framesGot;
        if (framesGot * bytesPerFrame < piece) break;
    }
    if (stored == 0) { v.chunkLen = 0; return 0; }
    v.chunkStart = index;
    v.chunkLen = stored;
    v.chunkAccum = accum;
    v.chunkSmp = v.smp;
    if (v.chunkLen == 0) return 0;
    return v.chunkBuf[0];
}

void XmPlayer::restartSamplePosition(int ch) {
    XmChannel& c = _channels[ch];
    // The cache is kept: a retrigger of the same sample reuses it.
    if (c.smp) c.looping = static_cast<const XmSample*>(c.smp)->looping;
    startVoiceAt(c, 0);
}

void XmPlayer::resolveSampleIndex(XmChannel& c, int note) {
    c.sampleIndex = 0xFF;
    c.smp = nullptr;
    if (c.instrument < 1 || c.instrument > MAX_INSTRUMENTS) return;
    const XmInstrument& inst = _instruments[c.instrument - 1];
    if (inst.numSamplesUsed == 0) return;
    int keymapIdx = note;
    if (keymapIdx < 0) keymapIdx = 0;
    if (keymapIdx > 95) keymapIdx = 95;
    uint8_t rel = inst.keymap[keymapIdx];
    if (rel >= inst.numSamplesUsed) rel = inst.numSamplesUsed - 1;
    c.sampleIndex = (uint8_t)(inst.firstSampleIndex + rel);
    if (c.sampleIndex < _numSamplesTotal) c.smp = &_samples[c.sampleIndex];
}

void XmPlayer::triggerNote(int ch, int note) {
    XmChannel& c = _channels[ch];
    resolveSampleIndex(c, note);

    int8_t finetune = 0, relNote = 0;
    uint8_t vol = 64, pan = 128;
    if (c.sampleIndex < _numSamplesTotal) {
        const XmSample& s = _samples[c.sampleIndex];
        finetune = s.finetune;
        relNote = s.relativeNoteNumber;
        vol = s.volume;
        pan = s.panning;
    }
    c.volume = vol;
    c.tremorMute = false;
    c.mixVolume = vol;
    c.pan = pan;
    c.mixPan = pan;
    c.period = xmNoteToPeriod(note + relNote, finetune);
    recomputeStep(c);

    c.volEnvTick = 0;
    c.panEnvTick = 0;
    c.keyOff = false;
    c.fadeoutVolume = 32768;
    c.volEnvValue = 64;
    if (c.instrument >= 1 && c.instrument <= MAX_INSTRUMENTS) {
        const XmEnvelope& ve = _instruments[c.instrument - 1].volEnv;
        if (ve.enabled() && ve.numPoints > 0) c.volEnvValue = (uint8_t)ve.pointY[0];
    }
    updateMixVolume(c);

    restartSamplePosition(ch);
}

void XmPlayer::openPatternForScan(int num) {
    _openPatternNum = num;
    _rowOffsetsValid = 0;
    if (num >= 0 && num < MAX_PATTERNS) {
        _scanCursor = _patternOffsets[num];
        _openPatternEmpty = _patternEmpty[num];
        _openPatternRowCount = (_patternRowCounts[num] > 0 && _patternRowCounts[num] <= MAX_ROWS) ? _patternRowCounts[num] : 64;
    } else {
        _scanCursor = 0;
        _openPatternEmpty = true;
        _openPatternRowCount = 64;
    }
}

void XmPlayer::advanceToRow(int targetRow) {
    if (targetRow < 0 || targetRow >= MAX_ROWS) return;

    if (targetRow < _rowOffsetsValid) {
        _patternFile.seekSet(_rowOffsets[targetRow]);
        decodeRow(true);
        return;
    }

    while (_rowOffsetsValid <= targetRow && _rowOffsetsValid < MAX_ROWS && _rowOffsetsValid < _openPatternRowCount) {
        _rowOffsets[_rowOffsetsValid] = _scanCursor;
        _patternFile.seekSet(_scanCursor);
        bool isTarget = (_rowOffsetsValid == targetRow);
        decodeRow(isTarget); // earlier rows: parsed only, to find their length; the target row: applied
        _scanCursor = _patternFile.curPosition();
        _rowOffsetsValid++;
    }
}

void XmPlayer::decodeRow(bool apply) {
    if (_openPatternEmpty) return; // nothing stored -- every cell is implicitly blank, see _patternEmpty's comment
    if (_openPatternNum < 0 || _openPatternNum >= MAX_PATTERNS) return;

    for (int ch = 0; ch < _fileNumChannels; ch++) {
        uint8_t flag;
        if (_patternFile.read(&flag, 1) != 1) { return; } // truncated/EOF -- stop

        uint8_t note = 0, instrument = 0, volume = 0, effect = 0, param = 0;
        bool hasNote = false, hasInstrument = false, hasVolume = false, hasEffect = false;

        if (flag & 0x80) {
            hasNote = (flag & 0x01) != 0;
            hasInstrument = (flag & 0x02) != 0;
            hasVolume = (flag & 0x04) != 0;
            hasEffect = (flag & 0x08) != 0;
            bool hasParam = (flag & 0x10) != 0;
            if (hasNote && _patternFile.read(&note, 1) != 1) { return; }
            if (hasInstrument && _patternFile.read(&instrument, 1) != 1) { return; }
            if (hasVolume && _patternFile.read(&volume, 1) != 1) { return; }
            if (hasEffect && _patternFile.read(&effect, 1) != 1) { return; }
            if (hasParam && _patternFile.read(&param, 1) != 1) { return; }
        } else {
            // Uncompressed: the flag byte itself IS the note, and all
            // four other fields follow unconditionally.
            note = flag;
            hasNote = (note != 0);
            uint8_t b4[4];
            if (_patternFile.read(b4, 4) != 4) { return; }
            instrument = b4[0];
            hasInstrument = (instrument != 0);
            volume = b4[1];
            hasVolume = (volume != 0);
            effect = b4[2];
            param = b4[3];
            hasEffect = true;
        }

        if (apply && ch < MAX_CHANNELS) {
            applyCell(ch, note, instrument, hasNote, hasInstrument, volume, hasVolume, effect, param, hasEffect);
        }
    }
}

void XmPlayer::keyOff(XmChannel& c) {
    // Fades out through the volume envelope if there is one, else cuts.
    c.keyOff = true;
    bool envOn = (c.instrument >= 1 && c.instrument <= MAX_INSTRUMENTS) &&
                 _instruments[c.instrument - 1].volEnv.enabled();
    if (!envOn) c.havePair = false;
}

void XmPlayer::applyNoteColumns(int ch, uint8_t note, uint8_t instrument, uint8_t volume, bool isTonePorta) {
    XmChannel& c = _channels[ch];
    if (instrument != 0) c.instrument = instrument;

    if (note == 97) {
        keyOff(c);
    } else if (note >= 1 && note <= 96) {
        int noteAbs0 = note - 1;
        if (isTonePorta) {
            // Slides the sounding sample toward the new note, with that
            // sample's finetune and relative note; no retrigger.
            int8_t finetune = 0, relNote = 0;
            if (c.sampleIndex < _numSamplesTotal) {
                finetune = _samples[c.sampleIndex].finetune;
                relNote = _samples[c.sampleIndex].relativeNoteNumber;
            }
            c.portaTarget = xmNoteToPeriod(noteAbs0 + relNote, finetune);
        } else {
            triggerNote(ch, noteAbs0);
        }
    }
    // 98 and up are unused.

    if (volume != 0) applyVolumeColumn(c, volume);
}

void XmPlayer::applyVolumeColumn(XmChannel& c, uint8_t volume) {
    // Sets the same channel fields as the effect column.
    if ((volume >= 0x10 && volume <= 0x50) || (volume >= 0x80 && volume <= 0x9F)) c.tremorMute = false;
    if (volume >= 0x10 && volume <= 0x50) {
        c.volume = volume - 0x10;
    } else if (volume >= 0x60 && volume <= 0x6F) {
        c.volSlideActiveRow = true;
        c.volSlideParam = (uint8_t)(volume - 0x60);
    } else if (volume >= 0x70 && volume <= 0x7F) {
        c.volSlideActiveRow = true;
        c.volSlideParam = (uint8_t)((volume - 0x70) << 4);
    } else if (volume >= 0x80 && volume <= 0x8F) {
        c.volume = (uint8_t)max(0, (int)c.volume - (int)(volume - 0x80)); // fine slide down, one-shot
    } else if (volume >= 0x90 && volume <= 0x9F) {
        c.volume = (uint8_t)min(64, (int)c.volume + (int)(volume - 0x90)); // fine slide up, one-shot
    } else if (volume >= 0xA0 && volume <= 0xAF) {
        c.vibratoSpeed = volume - 0xA0;
    } else if (volume >= 0xB0 && volume <= 0xBF) {
        c.vibratoActiveRow = true;
        c.vibratoDepth = volume - 0xB0;
    } else if (volume >= 0xC0 && volume <= 0xCF) {
        c.pan = (uint8_t)((volume - 0xC0) * 17);
    } else if (volume >= 0xD0 && volume <= 0xDF) {
        c.panSlideActiveRow = true;
        c.panSlideParam = (uint8_t)(volume - 0xD0);
    } else if (volume >= 0xE0 && volume <= 0xEF) {
        c.panSlideActiveRow = true;
        c.panSlideParam = (uint8_t)((volume - 0xE0) << 4);
    } else if (volume >= 0xF0) {
        // Tone portamento, speed x*16 (times 4 when it's applied, as FT2).
        c.tonePortaActiveRow = true;
        c.tonePortaSpeed = (uint8_t)((volume - 0xF0) << 4);
    }
}

void XmPlayer::applyCell(int ch, uint8_t note, uint8_t instrument, bool hasNote, bool hasInstrument,
                            uint8_t volume, bool hasVolume, uint8_t effect, uint8_t param, bool hasEffect) {
    XmChannel& c = _channels[ch];

    bool isTonePorta = (hasEffect && (effect == 3 || effect == 5)) || (hasVolume && volume >= 0xF0);
    bool isNoteDelay = hasEffect && effect == 14 /* E */ && (param >> 4) == 0xD && (param & 0x0F) != 0;

    uint8_t cellNote = hasNote ? note : 0;
    uint8_t cellInstrument = (hasInstrument && instrument >= 1 && instrument <= MAX_INSTRUMENTS) ? instrument : 0;
    uint8_t cellVolume = hasVolume ? volume : 0;
    if (isNoteDelay) {
        // EDy holds the whole cell (instrument, note and volume column) until tick y.
        c.noteDelayParam = param & 0x0F;
        c.pendingNote = cellNote;
        c.pendingInstrument = cellInstrument;
        c.pendingVolume = cellVolume;
    } else {
        applyNoteColumns(ch, cellNote, cellInstrument, cellVolume, isTonePorta);
    }

    if (!hasEffect) return;

    switch (effect) {
        case 0: // Arpeggio (reset each row, see advanceRow())
            c.arpeggioParam = param;
            break;
        case 1: // Porta up
            c.portaUpActiveRow = true;
            if (param != 0) c.portaUpSpeed = param;
            break;
        case 2: // Porta down
            c.portaDownActiveRow = true;
            if (param != 0) c.portaDownSpeed = param;
            break;
        case 3: // Tone portamento (target already set above if a note was given)
            c.tonePortaActiveRow = true;
            if (param != 0) c.tonePortaSpeed = param;
            break;
        case 4: // Vibrato
            c.vibratoActiveRow = true;
            if (param >> 4) c.vibratoSpeed = param >> 4;
            if (param & 0x0F) c.vibratoDepth = param & 0x0F;
            break;
        case 5: // Tone portamento + volume slide
            c.tonePortaActiveRow = true;
            c.volSlideActiveRow = true;
            if (param != 0) c.volSlideParam = param;
            break;
        case 6: // Vibrato + volume slide
            c.vibratoActiveRow = true;
            c.volSlideActiveRow = true;
            if (param != 0) c.volSlideParam = param;
            break;
        case 7: // Tremolo
            c.tremoloActiveRow = true;
            if (param >> 4) c.tremoloSpeed = param >> 4;
            if (param & 0x0F) c.tremoloDepth = param & 0x0F;
            break;
        case 8: // Set panning
            c.pan = param;
            break;
        case 9: // Sample offset, one-shot; the refill decodes up to the offset
            if (c.sampleIndex < _numSamplesTotal) {
                const XmSample& s = _samples[c.sampleIndex];
                uint32_t off = (uint32_t)param * 256;
                if (off < s.length) {
                    startVoiceAt(c, off);
                }
            }
            break;
        case 10: // A - Volume slide
            c.volSlideActiveRow = true;
            if (param != 0) c.volSlideParam = param;
            break;
        case 11: // B - Position jump
            _positionJumpPending = true;
            _positionJumpTarget = param;
            break;
        case 12: // C - Set volume
            c.volume = (param > 64) ? 64 : param;
            c.tremorMute = false;
            break;
        case 13: // D - Pattern break, row in BCD like MOD's
            _patternBreakPending = true;
            _patternBreakRow = (param >> 4) * 10 + (param & 0x0F);
            break;
        case 14: // E - Extended (sub-commands; EDy note-delay already handled above)
            if (!isNoteDelay) processExtendedCommand(ch, param);
            break;
        case 15: // F - Set speed/BPM: param < 0x20 -> ticks/row, else -> BPM
            if (param > 0) {
                if (param < 0x20) _speed = param;
                else _tempo = param;
            }
            break;
        case 16: // G - Set global volume
            _globalVolume = (param > 64) ? 64 : param;
            break;
        case 17: // H - Global volume slide -- own memory (_globalVolSlideParam)
            _globalVolSlideActiveRow = true;
            if (param != 0) _globalVolSlideParam = param;
            break;
        case 20: // K - Key off at tick `param` (0 == immediate, same tick)
            if (param == 0) {
                keyOff(c);
            } else {
                c.keyOffTickParam = param;
            }
            break;
        case 21: // L - Set envelope position (both volume and panning envelopes)
            c.volEnvTick = param;
            c.panEnvTick = param;
            break;
        case 25: // P - Panning slide
            c.panSlideActiveRow = true;
            if (param != 0) c.panSlideParam = param;
            break;
        case 27: // R - Multi retrigger
            c.retrigActiveRow = true;
            if (param != 0) {
                c.retrigVolChangeType = param >> 4;
                c.retrigParam = param & 0x0F;
            }
            break;
        case 29: // T - Tremor (same on/off tick-cycle shape as S3M's I)
            c.tremorActiveRow = true;
            if (param != 0) {
                c.tremorOnParam = (param >> 4) + 1;
                c.tremorOffParam = (param & 0x0F) + 1;
            }
            break;
        case 33: // X - Extra-fine portamento, one period unit a step (E1y/E2y move 4)
            if ((param >> 4) == 1) { // X1 - extra-fine porta up
                int p = (int)c.period - (int)(param & 0x0F);
                if (p < 1) p = 1;
                c.period = (uint16_t)p;
                recomputeStep(c);
            } else if ((param >> 4) == 2) { // X2 - extra-fine porta down
                c.period = (uint16_t)((int)c.period + (int)(param & 0x0F));
                recomputeStep(c);
            }
            break;
        default:
            break; // effects FT2 never assigned (I/J/M/N/O/Q/S/U/V/W/Y/Z) -- reserved/unused
    }
}

void XmPlayer::processExtendedCommand(int ch, uint8_t param) {
    XmChannel& c = _channels[ch];
    uint8_t sub = param >> 4;
    uint8_t val = param & 0x0F;

    switch (sub) {
        case 0x1: { // E1y - Fine porta up, one-shot
            int p = (int)c.period - val * 4;
            if (p < 1) p = 1;
            c.period = (uint16_t)p;
            recomputeStep(c);
            break;
        }
        case 0x2: // E2y - Fine porta down, one-shot
            c.period = (uint16_t)min(65535, (int)c.period + val * 4);
            recomputeStep(c);
            break;
        case 0x3: // E3y - Glissando control
            c.glissando = (val != 0);
            break;
        case 0x4: // E4y - Vibrato waveform -- only sine implemented, no-op otherwise
            break;
        case 0x5: // E5y - Set finetune -- per-sample finetune is already
                  // read from the sample header and used directly; a
                  // runtime 4-bit override is rare and out of scope, same
                  // treatment given to several other obscure sub-commands here
            break;
        case 0x6: // E6y - Pattern loop (same logic as S3M's SBy)
            patternLoop(c.patternLoopRow, c.patternLoopCount, val);
            break;
        case 0x7: // E7y - Tremolo waveform -- same treatment as E4y
            break;
        case 0x8: // E8y - Set panning, coarse (0-15 -> 0-255)
            c.pan = (uint8_t)(val * 17);
            break;
        case 0x9: // E9y - Retrigger note every y ticks, no volume change
            if (val > 0) {
                c.retrigActiveRow = true;
                c.retrigParam = val;
                c.retrigVolChangeType = 0;
            }
            break;
        case 0xA: // EAy - Fine volume slide up, one-shot
            c.volume = (uint8_t)min(64, (int)c.volume + val);
            c.tremorMute = false;
            break;
        case 0xB: // EBy - Fine volume slide down, one-shot
            c.volume = (uint8_t)max(0, (int)c.volume - val);
            c.tremorMute = false;
            break;
        case 0xC: // ECy - Note cut at tick y
            c.noteCutParam = val;
            break;
        case 0xD: // EDy - Note delay -- handled entirely by applyCell()'s isNoteDelay branch
            break;
        case 0xE: // EEy - Pattern delay: hold the current row for y extra tick-cycles
            _patternDelayRepeatsLeft = val;
            break;
        case 0x0: // E0y - Set filter -- no hardware filter to toggle here
        default:
            break;
    }
}

void XmPlayer::advanceRow() {
    _globalVolSlideActiveRow = false;
    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];
        c.arpeggioParam = 0;
        c.noteDelayParam = 0;
        c.noteCutParam = 0xFF;
        c.keyOffTickParam = 0xFF;
        // See their declarations -- must be given each row to continue,
        // not just remembered.
        c.volSlideActiveRow = false;
        c.portaDownActiveRow = false;
        c.portaUpActiveRow = false;
        c.tonePortaActiveRow = false;
        c.vibratoActiveRow = false;
        c.tremoloActiveRow = false;
        c.retrigActiveRow = false;
        c.panSlideActiveRow = false;
        c.tremorActiveRow = false;
    }

    int openRows = (_openPatternNum >= 0 && _openPatternNum < MAX_PATTERNS) ? _patternRowCounts[_openPatternNum] : 64;
    if (openRows <= 0 || openRows > MAX_ROWS) openRows = 64;
    // Break rows past the target pattern's length are clamped below.
    stepPosition(openRows, 0x7FFFFFFF);

    // XM's order table has no S3M-style skip/end marker bytes -- the
    // song simply ends when orderPos runs past _songLength. Matches
    // MOD/S3M's "stop at the end, PLAY restarts" convention rather than
    // auto-looping via the header's restartPos field -- see load()'s comment.
    if (_orderPos < 0 || _orderPos >= _songLength) {
        _state = STATE_DONE;
        return;
    }

    int patternNum = _orderTable[_orderPos];
    if (patternNum < 0 || patternNum >= MAX_PATTERNS) {
        // Corrupt/out-of-range order entry -- skip forward defensively
        // rather than looping forever on an unplayable position.
        _orderPos++;
        _row = 0;
        if (_orderPos >= _songLength) { _state = STATE_DONE; return; }
        patternNum = _orderTable[_orderPos];
        if (patternNum < 0 || patternNum >= MAX_PATTERNS) { _state = STATE_DONE; return; }
    }
    // A jump/break to a row beyond the target pattern's own length is
    // invalid input -- clamp rather than read past it.
    int rowCount = _patternRowCounts[patternNum] > 0 ? _patternRowCounts[patternNum] : 64;
    if (_row >= rowCount) _row = 0;

    if (patternNum != _openPatternNum) openPatternForScan(patternNum);
    advanceToRow(_row);

    // A row starts at the base pitch, dropping last tick's vibrato or
    // arpeggio, and plays this row's volume from its first tick.
    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];
        if (c.period == 0) continue;
        if (c.stepPeriod != c.period) recomputeStep(c);
        updateMixVolume(c);
    }
}

void XmPlayer::processEnvelopes() {
    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];
        if (c.period == 0) { c.mixPan = c.pan; continue; }
        // Baseline for this tick -- processTickEffects() (tremolo) may
        // still adjust mixVolume further as a transient, same ordering
        // S3M uses. A minor one-tick lag between a volume-slide effect
        // and the envelope-scaled result is an accepted simplification,
        // see this class's header comment.
        c.mixVolume = c.volume;
        c.mixPan = c.pan;
        c.volEnvValue = 64;
        if (c.instrument < 1 || c.instrument > MAX_INSTRUMENTS) continue;
        const XmInstrument& inst = _instruments[c.instrument - 1];

        uint8_t volEnvValue = 64;
        const XmEnvelope& ve = inst.volEnv;
        if (ve.enabled() && ve.numPoints > 0) {
            bool holding = ve.sustainOn() && !c.keyOff && ve.sustainPoint < ve.numPoints &&
                           c.volEnvTick >= ve.pointX[ve.sustainPoint];
            if (!holding) {
                c.volEnvTick++;
                if (ve.loopOn() && ve.loopEndPoint < ve.numPoints && c.volEnvTick > ve.pointX[ve.loopEndPoint]) {
                    c.volEnvTick = ve.pointX[ve.loopStartPoint < ve.numPoints ? ve.loopStartPoint : 0];
                }
            }
            if (c.volEnvTick >= ve.pointX[ve.numPoints - 1]) {
                volEnvValue = (uint8_t)ve.pointY[ve.numPoints - 1];
            } else {
                int i = 0;
                while (i + 1 < ve.numPoints && ve.pointX[i + 1] < c.volEnvTick) i++;
                if (i + 1 < ve.numPoints && ve.pointX[i + 1] > ve.pointX[i]) {
                    volEnvValue = (uint8_t)(ve.pointY[i] + (int32_t)(ve.pointY[i + 1] - ve.pointY[i]) *
                                             (int32_t)(c.volEnvTick - ve.pointX[i]) / (int32_t)(ve.pointX[i + 1] - ve.pointX[i]));
                } else {
                    volEnvValue = (uint8_t)ve.pointY[i];
                }
            }
        }

        if (c.keyOff) {
            uint32_t fade = inst.volFadeout > 0 ? inst.volFadeout : 1;
            c.fadeoutVolume = (c.fadeoutVolume > fade) ? c.fadeoutVolume - fade : 0;
            if (c.fadeoutVolume == 0) c.havePair = false; // faded out, stop the voice
        }

        uint8_t panEnvValue = 32;
        const XmEnvelope& pe = inst.panEnv;
        if (pe.enabled() && pe.numPoints > 0) {
            bool holding = pe.sustainOn() && !c.keyOff && pe.sustainPoint < pe.numPoints &&
                           c.panEnvTick >= pe.pointX[pe.sustainPoint];
            if (!holding) {
                c.panEnvTick++;
                if (pe.loopOn() && pe.loopEndPoint < pe.numPoints && c.panEnvTick > pe.pointX[pe.loopEndPoint]) {
                    c.panEnvTick = pe.pointX[pe.loopStartPoint < pe.numPoints ? pe.loopStartPoint : 0];
                }
            }
            if (c.panEnvTick >= pe.pointX[pe.numPoints - 1]) {
                panEnvValue = (uint8_t)pe.pointY[pe.numPoints - 1];
            } else {
                int i = 0;
                while (i + 1 < pe.numPoints && pe.pointX[i + 1] < c.panEnvTick) i++;
                if (i + 1 < pe.numPoints && pe.pointX[i + 1] > pe.pointX[i]) {
                    panEnvValue = (uint8_t)(pe.pointY[i] + (int32_t)(pe.pointY[i + 1] - pe.pointY[i]) *
                                             (int32_t)(c.panEnvTick - pe.pointX[i]) / (int32_t)(pe.pointX[i + 1] - pe.pointX[i]));
                } else {
                    panEnvValue = (uint8_t)pe.pointY[i];
                }
            }
        }

        c.volEnvValue = volEnvValue;
        updateMixVolume(c);
        // Deliberately simplified pan/envelope blend, not FT2's own
        // extremes-pulling formula -- see this class's header comment.
        int panOffset = ((int)panEnvValue - 32) * 2;
        int p = (int)c.pan + panOffset;
        if (p < 0) p = 0; else if (p > 255) p = 255;
        c.mixPan = (uint8_t)p;
    }
}

void XmPlayer::processTickEffects() {
    // Global volume slide (H), on the rows that give it.
    if (_globalVolSlideActiveRow) {
        uint8_t hi = _globalVolSlideParam >> 4, lo = _globalVolSlideParam & 0x0F;
        if (hi) _globalVolume = (uint8_t)min(64, (int)_globalVolume + hi);
        else if (lo) _globalVolume = (uint8_t)max(0, (int)_globalVolume - lo);
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];

        if (c.noteDelayParam != 0 && _tick == c.noteDelayParam) {
            applyNoteColumns(ch, c.pendingNote, c.pendingInstrument, c.pendingVolume, false);
            updateMixVolume(c);
            c.noteDelayParam = 0;
        }
        if (c.noteCutParam != 0xFF && _tick == c.noteCutParam) {
            c.volume = 0;
            c.tremorMute = false;
            c.noteCutParam = 0xFF;
        }
        if (c.keyOffTickParam != 0xFF && _tick == c.keyOffTickParam) {
            keyOff(c);
            c.keyOffTickParam = 0xFF;
        }
        if (c.retrigActiveRow && c.retrigParam > 0 && (_tick % c.retrigParam) == 0) {
            restartSamplePosition(ch); // same pitch, position back to 0 -- not a fresh triggerNote()
            // Rxy's volume-change-type nibble -- an approximation, not a
            // bit-exact reproduction of FT2's own 16-code table, same
            // deliberate-simplification treatment as S3M's Qxy.
            uint8_t t = c.retrigVolChangeType;
            if (t >= 1 && t <= 6) c.volume = (uint8_t)max(0, (int)c.volume - (int)t);
            else if (t == 7) c.volume = (uint8_t)(c.volume / 2);
            else if (t >= 9 && t <= 14) c.volume = (uint8_t)min(64, (int)c.volume + (int)(t - 8));
            else if (t == 15) c.volume = (uint8_t)min(64, (int)c.volume * 2);
            if (t != 0 && t != 8) c.tremorMute = false;
        }

        if (c.period == 0) continue;

        if (c.volSlideActiveRow) {
            uint8_t up = c.volSlideParam >> 4, down = c.volSlideParam & 0x0F;
            if (up) c.volume = (uint8_t)min(64, (int)c.volume + up);
            else if (down) c.volume = (uint8_t)max(0, (int)c.volume - down);
            c.tremorMute = false;
        }
        // Tremor (T), on the rows that give it. Runs after the slides, so
        // the off phase wins, and stays muted if T ends in it.
        if (c.tremorActiveRow && (c.tremorOnParam > 0 || c.tremorOffParam > 0)) {
            uint8_t cycleLen = c.tremorOnParam + c.tremorOffParam;
            if (cycleLen > 0) {
                uint8_t pos = c.tremorCounter % cycleLen;
                c.tremorMute = (pos >= c.tremorOnParam);
                c.tremorCounter++;
            }
        }
        // Volume changes on this tick show up now, not a tick later.
        updateMixVolume(c);
        if (c.panSlideActiveRow) {
            uint8_t left = c.panSlideParam >> 4, right = c.panSlideParam & 0x0F;
            if (right) c.pan = (uint8_t)min(255, (int)c.pan + right * 2);
            else if (left) c.pan = (uint8_t)max(0, (int)c.pan - left * 2);
        }

        // Porta and tone porta speeds are 4 period units a step, as FT2.
        if (c.portaDownActiveRow && c.portaDownSpeed) {
            int p = (int)c.period + c.portaDownSpeed * 4;
            if (p > 32000) p = 32000;
            c.period = (uint16_t)p;
        }
        if (c.portaUpActiveRow && c.portaUpSpeed) {
            int p = (int)c.period - c.portaUpSpeed * 4;
            if (p < 1) p = 1;
            c.period = (uint16_t)p;
        }
        if (c.tonePortaActiveRow && c.portaTarget != 0 && c.tonePortaSpeed) {
            int speed = c.tonePortaSpeed * 4;
            if (c.period < c.portaTarget) c.period = (uint16_t)min((int)c.portaTarget, (int)c.period + speed);
            else if (c.period > c.portaTarget) c.period = (uint16_t)max((int)c.portaTarget, (int)c.period - speed);
        }

        // Vibrato and arpeggio only change the pitch that plays this tick,
        // c.period stays the base.
        uint16_t playPeriod = c.period;
        if (c.vibratoActiveRow && c.vibratoDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.vibratoPos / 64.0f;
            int delta = (int16_t)(sinf(angle) * c.vibratoDepth * 8.0f); // FT2's depth, 8 period units a step
            playPeriod = (uint16_t)max(1, (int)c.period + delta);
            c.vibratoPos = (uint8_t)((c.vibratoPos + c.vibratoSpeed) & 0x3F);
        }
        // Arpeggio: base note, +x semitones, +y semitones, counted the way
        // FT2 does from the end of the row, so tick 1 at speed 6 is +y. A
        // semitone is 64 units in linear mode, a 2^(1/12) ratio in Amiga mode.
        if (c.arpeggioParam != 0) {
            int cyclePos = (_speed - _tick) % 3;
            int semis = (cyclePos == 1) ? (c.arpeggioParam >> 4) : (cyclePos == 2) ? (c.arpeggioParam & 0x0F) : 0;
            if (semis != 0) {
                if (_linearFreqTable) {
                    int p = (int)c.period - semis * 64;
                    playPeriod = (uint16_t)(p < 1 ? 1 : p);
                } else {
                    float factor = powf(2.0f, -(float)semis / 12.0f);
                    playPeriod = (uint16_t)max(1.0f, (float)c.period * factor);
                }
            }
        }
        if (playPeriod != c.stepPeriod) recomputeStep(c, playPeriod);

        if (c.tremoloActiveRow && c.tremoloDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.tremoloPos / 64.0f;
            int delta = (int)(sinf(angle) * c.tremoloDepth * 4.0f);
            int v = (int)c.mixVolume + delta; // mixVolume already holds this tick's envelope-scaled value, see processEnvelopes()
            if (v < 0) v = 0; else if (v > 64) v = 64;
            c.mixVolume = (uint8_t)v;
            c.tremoloPos = (uint8_t)((c.tremoloPos + c.tremoloSpeed) & 0x3F);
        }
        if (c.tremorMute) c.mixVolume = 0;
    }
}

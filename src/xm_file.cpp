#include "xm_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>

namespace {

// Same Amiga hardware clock constant MOD/S3M use -- see xmNoteToPeriod()'s
// comment on why Amiga-mode XM is anchored at the same period/frequency
// reference point those two already use. float, not double -- see
// S3mPlayer's identical AMIGA_PAL_CLOCK comment on why (no hardware FPU).
const float AMIGA_PAL_CLOCK = 7093789.2f;

// _mixShift budgets headroom for this many simultaneous full-volume
// channels, capped well below MAX_CHANNELS (32) -- see _mixShift's own
// comment for why a literal all-channels-at-once budget was needlessly
// quiet for real music, and softClampMix() below for what absorbs the
// (rare) moments actual content exceeds this budget. Same cap, same
// reasoning, duplicated in ModPlayer::load()/S3mPlayer::load().
//
// No longer actually used to compute _mixShift (see load()) -- capping at
// 8 channels only ever reduced the shift for files with *more* than 8
// channels, so an <=8-channel file got zero benefit and stayed shifted by
// its full literal channel count. Real-hardware listening found tracker
// playback still noticeably quieter than WAV/MIDI even after that fix.
// Kept here only as a reminder of that dead end; _mixShift is
// unconditionally 0 now, relying entirely on softClampMix() for safety.
const int MIX_SHIFT_TYPICAL_CHANNELS = 8;

// Soft knee above SOFT_KNEE_START, hard ceiling at HARD_LIMIT -- same
// shape and same constants as Synth's own softLimit() in synth.cpp (kept
// independent here rather than shared/exported, matching this file's
// existing convention of parallel, cross-referenced per-engine mixing
// code rather than a shared utility -- see e.g. readChannelSample()'s
// comment). Replaces mixOneSample()'s previous bare +-32767 clamp, which
// a real-hardware investigation had flagged as a suspect for audible pops
// (see g_xmDiagClips/g_xmDiagMaxAbsPreClamp below) -- now that
// MIX_SHIFT_TYPICAL_CHANNELS budgets less headroom than a literal
// all-channels-at-once bound, that clamp is more likely to actually
// trigger, so it needs to compress gracefully instead of cutting off flat.
int32_t softClampMix(int32_t mix) {
    int32_t sign = (mix < 0) ? -1 : 1;
    int32_t mag = mix * sign;
    if (mag > 24576) mag = 24576 + (mag - 24576) / 4;
    if (mag > 32000) mag = 32000;
    return mag * sign;
}

} // namespace

// -- Temporary real-hardware diagnostics -------------------------------
// Same technique/reasoning as S3mPlayer's own (now-removed) g_diag*
// counters during its debugging saga: cheap counters incremented through
// the hot paths, printed once a second, removed once no longer needed.
// Not gated behind a build flag -- meant to be deleted outright once the
// investigation this was added for is done.
//
// Silenced (not deleted) for now -- flip back to true to get the once/sec
// Serial print back; the counters themselves keep accumulating either
// way (cheap, harmless), only the print+reset is skipped while this is
// false. Same flag/reasoning as S3mPlayer's own DIAG_PRINT_ENABLED.
const bool DIAG_PRINT_ENABLED = false;
uint32_t g_xmDiagCalls = 0, g_xmDiagProduced = 0, g_xmDiagPeakOut = 0;
uint32_t g_xmDiagBusyUs = 0, g_xmDiagMaxUs = 0;
uint32_t g_xmDiagMinFree = 0xFFFFFFFF;
uint32_t g_xmDiagUnderruns = 0;
// Snapshot of Synth::wavStreamUnderrunSamples() at the last print --
// diffed against the current reading to get real dropped-sample count
// per window, a duration/severity measure the plain underruns counter
// above can't provide (see that function's own comment).
uint32_t g_xmDiagLastUnderrunSamples = 0;
uint32_t g_xmDiagActiveChSum = 0, g_xmDiagActiveChSamples = 0;
uint32_t g_xmDiagChunkHits = 0, g_xmDiagChunkMisses = 0;
uint32_t g_xmDiagChunkUs = 0;
// Splits chunkUs into the actual SD seekSet()+read() syscall time versus
// everything else in a refill (anchor selection, the delta-decode loop
// itself) -- added specifically to find out whether refill cost is
// SD-latency-bound (bigger CHUNK_SIZE would help a lot, same lesson
// S3M's own saga already learned) or decode-loop-bound (it wouldn't).
uint32_t g_xmDiagSeekReadUs = 0;
// Splits refills further: "sequential" is a miss whose index is exactly
// the previous chunk's end (playback simply outran its cached window,
// no real seek needed conceptually -- just the next bytes in the file);
// "seek" is everything else (a genuine jump: retrigger, loop wrap,
// pattern-loop restart, 9xx). If sequential misses cost about as much as
// seek misses, SD cost here is transfer-time-bound, not fixed-seek-
// latency-bound -- meaning a bigger CHUNK_SIZE would NOT help (same
// total bytes moved either way), contrary to the assumption a same-
// session CHUNK_SIZE increase was based on (which then had to be
// reverted after a real-hardware crash/audio-corruption incident).
uint32_t g_xmDiagSeqMisses = 0, g_xmDiagSeqUs = 0;
uint32_t g_xmDiagSeekMisses = 0, g_xmDiagSeekMissUs = 0;
// Underruns measured zero throughout a real-hardware test where the user
// still heard the pops -- ruling out ring-buffer starvation as the cause.
// These test the next hypothesis: genuine hard-clipping (mixOneSample()'s
// final clamp has no limiter) or an abrupt sample-to-sample discontinuity
// (e.g. a retrigger jumping between very different instantaneous values
// with no declick) actually present in the mixed signal itself.
uint32_t g_xmDiagClips = 0;          // how many output samples (L or R) hit the +-32767/-32768 clamp
int32_t g_xmDiagMaxAbsPreClamp = 0;  // largest |mixL|/|mixR| seen BEFORE clamping, headroom check
int16_t g_xmDiagLastOutL = 0;
uint32_t g_xmDiagMaxJump = 0;        // largest |outL[n] - outL[n-1]| seen, sample-to-sample discontinuity check
// Time spent in mixOneSample() specifically, measured directly around
// its call site in update() rather than inferred by subtracting chunkUs/
// tickUs from busyUs (which would silently hide any other unaccounted
// cost, e.g. wavStreamWrite() itself).
uint32_t g_xmDiagMixUs = 0;
uint32_t g_xmDiagAdvRows = 0, g_xmDiagApplyCells = 0, g_xmDiagNoteTrigs = 0;
uint32_t g_xmDiagPatReadFails = 0;
uint32_t g_xmDiagTickUs = 0;

bool XmPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!_file.open(path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }
    // See _patternFile's declaration -- a second, independent handle to
    // the same file, used exclusively for pattern-cell reads during
    // playback.
    if (!_patternFile.open(path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    uint8_t hdr[60];
    if (_file.read(hdr, 60) != 60 || memcmp(hdr, "Extended Module: ", 17) != 0 || hdr[37] != 0x1A) {
        strncpy(_error, "unsupported format", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }

    uint8_t sizeBuf[4];
    if (_file.read(sizeBuf, 4) != 4) {
        strncpy(_error, "invalid XM file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }
    // Measured FROM this field's own start (offset 60) -- so pattern data
    // begins at 60+headerSize regardless of how much of the declared
    // header block the fixed fields below actually consume. Always trust
    // this field over any fixed-offset assumption -- see this class's
    // header comment on the instrument-size field's identical convention.
    uint32_t headerSize = (uint32_t)sizeBuf[0] | ((uint32_t)sizeBuf[1] << 8) |
                           ((uint32_t)sizeBuf[2] << 16) | ((uint32_t)sizeBuf[3] << 24);

    uint8_t fb[16];
    if (_file.read(fb, 16) != 16) {
        strncpy(_error, "invalid XM file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }
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
    if (numChannelsReal < 1 || numChannelsReal > 128 || numPatternsReal > 4096 || numInstrumentsReal > 4096) {
        strncpy(_error, "invalid XM file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }

    _linearFreqTable = (flags & 0x01) != 0;
    _fileNumChannels = numChannelsReal;
    _numChannels = (numChannelsReal < (uint16_t)MAX_CHANNELS) ? (int)numChannelsReal : MAX_CHANNELS;
    // See _mixShift's declaration and MIX_SHIFT_TYPICAL_CHANNELS's comment --
    // no channel-count-based headroom shift at all anymore, relying
    // entirely on softClampMix() for safety.
    _mixShift = 0;

    // Order table: always exactly 256 bytes regardless of songLen.
    if (_file.read(_orderTable, MAX_ORDERS) != MAX_ORDERS) {
        strncpy(_error, "invalid XM file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }
    _orderCount = (songLenReal < (uint16_t)MAX_ORDERS) ? (int)songLenReal : MAX_ORDERS;

    if (!_file.seekSet(60 + headerSize)) {
        strncpy(_error, "invalid XM file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }

    for (int i = 0; i < MAX_PATTERNS; i++) _patternEmpty[i] = true; // see its declaration -- safe default for unscanned/out-of-range slots

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
        if (numSamplesReal > 64) {
            strncpy(_error, "invalid XM file", sizeof(_error) - 1);
            _file.close();
            _patternFile.close();
            _state = STATE_ERROR;
            return false;
        }

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
                for (int p = 0; p < MAX_ENV_POINTS; p++) {
                    ve.pointX[p] = (uint16_t)volEnvRaw[p * 4] | ((uint16_t)volEnvRaw[p * 4 + 1] << 8);
                    ve.pointY[p] = (uint16_t)volEnvRaw[p * 4 + 2] | ((uint16_t)volEnvRaw[p * 4 + 3] << 8);
                    pe.pointX[p] = (uint16_t)panEnvRaw[p * 4] | ((uint16_t)panEnvRaw[p * 4 + 1] << 8);
                    pe.pointY[p] = (uint16_t)panEnvRaw[p * 4 + 2] | ((uint16_t)panEnvRaw[p * 4 + 3] << 8);
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
        _channels[ch] = XmChannel();
    }

    _globalVolume = 64;
    _globalVolSlideParam = 0;
    _speed = defaultTempo > 0 ? defaultTempo : 6;
    _bpm = defaultBpm > 0 ? defaultBpm : 125;

    _orderPos = 0;
    _row = -1; // see currentRow()'s comment -- avoids skipping row 0 of the first pattern
    _tick = 0;
    _samplesUntilNextTick = ticksToSamples(_bpm);
    _patternDelayRepeatsLeft = 0;
    _patternBreakPending = false;
    _positionJumpPending = false;
    _openPatternNum = -1;
    _openPatternEmpty = false;
    _rowOffsetsValid = 0;

    _elapsedMsFrozen = 0;
    _lastResumeMicros = micros();
    _state = STATE_PAUSED;
    return true;
}

void XmPlayer::close() {
    if (_file) _file.close();
    if (_patternFile) _patternFile.close();
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

void XmPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _lastResumeMicros = micros();
    _state = STATE_PLAYING;
    // Pre-fill the ring buffer before the I2S consumer starts pulling
    // from it (wavStreamSetActive(true) below) -- real-hardware
    // diagnostics found a genuine underrun right at song start: the
    // caller's first update() call doesn't happen until its own next
    // main-loop iteration (itself often delayed further by a synchronous
    // screen redraw), during which a consumer already marked active
    // would be pulling from a completely empty 8192-frame ring. update()
    // safely no-ops once the ring is full (checks wavStreamFree()==0
    // itself), so a bounded handful of calls here just tops it up --
    // this also happens to be exactly where the first, coldest SD reads
    // for whatever channels trigger on row 0 land, so doing it before
    // the consumer is live means that cost can't itself cause an
    // audible stall.
    //
    // Iteration count is intentionally NOT a small fixed number -- it
    // used to be (8), sized against update()'s own per-call output cap
    // (MAX_FRAMES_PER_TICK_CALL) being 512 at the time (up to 4096 frames
    // of pre-fill, half the 8192-frame ring). When that cap was later
    // lowered to 128 (to bound worst-case single-call latency, see
    // update()'s own comment), this loop was never revisited -- it kept
    // pre-filling only up to 8*128=1024 frames, and a real underrun
    // reappeared in exactly this ramp-up window on every real-hardware
    // test since, even though the cause (this loop silently pre-filling
    // far less than intended) had nothing to do with whatever each of
    // those tests was actually investigating. Loop directly on
    // wavStreamFree() reaching 0 (i.e. actually full) instead, with a
    // generous iteration ceiling only as a defensive bound against an
    // unexpected infinite loop, not as the real limiting factor.
    for (int i = 0; i < 128 && Synth::wavStreamFree() > 0; i++) update();
    Synth::wavStreamSetActive(true);
}

void XmPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _elapsedMsFrozen += (micros() - _lastResumeMicros) / 1000;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void XmPlayer::stop() {
    close();
}

uint32_t XmPlayer::elapsedMs() const {
    uint32_t extra = (_state == STATE_PLAYING) ? (micros() - _lastResumeMicros) / 1000 : 0;
    return _elapsedMsFrozen + extra;
}

uint32_t XmPlayer::ticksToSamples(uint16_t bpm) {
    if (bpm == 0) bpm = 125;
    // Same tick-length convention S3M's tempo uses (a tracker-standard
    // "2.5/BPM seconds per tick" tie, not specific to S3M).
    return (uint32_t)((double)SAMPLE_RATE * 2.5 / (double)bpm);
}

// note is an absolute, 0-based semitone position (pattern note - 1, plus
// the sample's own relativeNoteNumber -- see triggerNote()/applyCell()).
// Both formulas are anchored at the same universal "C-4 (note 48),
// finetune 0 => 8363 Hz" reference point, so a fixed note/finetune
// produces the same pitch regardless of which mode is active -- only
// portamento/vibrato/arpeggio (which operate on period deltas, not
// absolute notes) sound different between modes, which is correct FT2
// behavior, not a bug -- see this class's header comment.
uint16_t XmPlayer::xmNoteToPeriod(int note, int8_t finetune) const {
    if (_linearFreqTable) {
        int period = 7680 - note * 64 - (int)finetune / 2;
        if (period < 1) period = 1;
        if (period > 65535) period = 65535;
        return (uint16_t)period;
    } else {
        // Anchor derived so that periodToFreq() below reproduces exactly
        // 8363 Hz at this same (note=48, finetune=0) reference point --
        // NOT ModPlayer::BASE_PERIOD_TABLE's raw C-2=428 Amiga-hardware
        // period, which was this formula's original anchor until the
        // cross-validation pass measured real output audio against
        // libopenmpt (rendering a synthetic single-cycle-waveform sample
        // through both modes and counting cycles in the output) and found
        // XM's own "Amiga frequency table" mode is calibrated to the same
        // 8363 Hz reference linear mode uses, not the raw 428/PAL-clock
        // Amiga value (which measured ~8287 Hz, a consistent ~1.5%/~25
        // cent error against every real note tested) -- i.e. FT2 itself
        // doesn't actually use authentic Amiga hardware periods for this
        // mode, despite the name. Kept as a separate formula/branch from
        // linear mode anyway (not simply merged) since real FT2's actual
        // amiga-table quantization is known to differ from this
        // continuous approximation by a small amount away from octave
        // points -- see this class's header comment.
        const float AMIGA_ANCHOR_PERIOD = AMIGA_PAL_CLOCK / (2.0f * 8363.0f);
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
        return AMIGA_PAL_CLOCK / (2.0f * (float)period);
    }
}

void XmPlayer::recomputeStep(XmChannel& c) {
    if (c.period == 0) { c.sampleStep16 = 0; return; }
    float hz = periodToFreq(c.period);
    c.sampleStep16 = (uint32_t)((hz / (float)SAMPLE_RATE) * 65536.0f);
}

int32_t XmPlayer::decodeDeltaRun(uint32_t fileOffset, uint32_t frameCount, bool is16Bit, int32_t startAccum) {
    if (frameCount == 0) return startAccum;
    uint32_t bytesPerFrame = is16Bit ? 2 : 1;
    if (!_file.seekSet(fileOffset)) return startAccum;
    int32_t accum = startAccum;
    uint32_t remaining = frameCount;
    while (remaining > 0) {
        uint32_t chunkFrames = remaining > (uint32_t)XmChannel::CHUNK_SIZE ? (uint32_t)XmChannel::CHUNK_SIZE : remaining;
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

// __not_in_flash_func: see S3mPlayer::readRawSample()'s identical comment
// on why the hottest per-sample-frame functions live in RAM, not flash.
// This is now only ever reached on an actual cache miss -- see
// readRawSample()'s inline fast path in xm_file.h.
int16_t __not_in_flash_func(XmPlayer::refillChunk)(XmChannel& c, const XmSample& s, uint32_t index, uint32_t loopEnd) {
    g_xmDiagChunkMisses++;
    uint32_t __diagChunkStartUs = micros();

    // Cache miss -- refill. See this class's header comment on the full
    // sequential/known-anchor/bounded-catch-up/random-seek algorithm.
    uint32_t bytesPerFrame = s.is16Bit ? 2 : 1;
    uint32_t windowLimit = (c.looping && index < loopEnd) ? loopEnd : s.length;
    uint32_t windowFrames = (uint32_t)XmChannel::CHUNK_SIZE;
    if (index + windowFrames > windowLimit) windowFrames = windowLimit - index;
    if (windowFrames == 0) { c.chunkLen = 0; return 0; }

    int32_t startAccum;
    uint32_t readStart = index; // where the SD read actually begins -- see the small-gap fold-in below
    bool __diagIsSequential = false;
    if (c.chunkLen != 0 && c.chunkSampleIndex == c.sampleIndex && index == c.chunkStart + c.chunkLen) {
        // Sequential continuation -- the common case -- reuse the carried
        // accumulator directly, no extra SD work.
        startAccum = c.chunkAccum;
        __diagIsSequential = true;
    } else if (index == 0) {
        startAccum = 0;
    } else if (c.looping && index == s.loopStart) {
        startAccum = s.accumAtLoopStart; // precomputed at load(), also free
    } else {
        // Non-sequential gap. The realistic loop-wrap-overshoot case
        // (readChannelSample()'s wrap arithmetic almost never lands
        // exactly on loopStart -- typically 1-3 frames past it, bounded
        // by the resample step) is folded into THIS SAME SD read by
        // moving readStart back to the anchor, rather than a separate
        // decodeDeltaRun() seek+read -- measured on real hardware this
        // was doubling the SD-seek cost of essentially every ordinary
        // loop wrap (a second full seek's fixed latency paid on top of
        // the main window's), which is common enough in real content
        // (any sustained looped instrument) to be a genuine, measurable
        // cost, not just a rare edge case. Only a gap too large to fold
        // into one CHUNK_SIZE-bounded read (a true random seek -- the 9xx
        // sample-offset effect, or an unusually large step) still falls
        // back to the separate bounded catch-up read.
        uint32_t anchor = (c.looping && index >= s.loopStart) ? s.loopStart : 0;
        int32_t anchorAccum = (anchor == s.loopStart) ? s.accumAtLoopStart : 0;
        uint32_t gap = index - anchor;
        const uint32_t SMALL_GAP_FOLD_LIMIT = 64; // generous margin over a realistic 1-3 frame overshoot
        if (gap > 0 && gap <= SMALL_GAP_FOLD_LIMIT) {
            readStart = anchor;
            startAccum = anchorAccum;
            if (windowFrames > (uint32_t)XmChannel::CHUNK_SIZE - gap) windowFrames = (uint32_t)XmChannel::CHUNK_SIZE - gap;
        } else {
            startAccum = (gap > 0) ? decodeDeltaRun(s.fileOffset + anchor * bytesPerFrame, gap, s.is16Bit, anchorAccum)
                                    : anchorAccum;
        }
    }

    uint32_t __diagSeekStartUs = micros();
    if (!_file.seekSet(s.fileOffset + readStart * bytesPerFrame)) { c.chunkLen = 0; return 0; }
    uint32_t skipFrames = index - readStart; // decoded-and-discarded prefix when a small gap was folded in above
    uint32_t totalFrames = windowFrames + skipFrames;
    int n = _file.read(_rawScratch, totalFrames * bytesPerFrame);
    uint32_t __diagThisUs = micros() - __diagSeekStartUs;
    g_xmDiagSeekReadUs += __diagThisUs;
    if (__diagIsSequential) {
        g_xmDiagSeqMisses++;
        g_xmDiagSeqUs += __diagThisUs;
    } else {
        g_xmDiagSeekMisses++;
        g_xmDiagSeekMissUs += __diagThisUs;
    }
    if (n <= 0) { c.chunkLen = 0; return 0; }
    uint32_t framesGot = (uint32_t)n / bytesPerFrame;
    if (framesGot <= skipFrames) { c.chunkLen = 0; return 0; } // short read landed entirely within the discarded prefix

    int32_t accum = startAccum;
    uint32_t stored = 0;
    for (uint32_t i = 0; i < framesGot; i++) {
        int16_t decoded;
        if (s.is16Bit) {
            int16_t delta = (int16_t)((uint16_t)_rawScratch[i * 2] | ((uint16_t)_rawScratch[i * 2 + 1] << 8));
            accum = (int16_t)(accum + delta);
            decoded = (int16_t)accum;
        } else {
            int8_t delta = (int8_t)_rawScratch[i];
            accum = (int8_t)(accum + delta);
            // Widen signed 8-bit to 16-bit range, same convention
            // S3M/WavPlayer already use for their own 8-bit PCM.
            decoded = (int16_t)((int16_t)(int8_t)accum << 8);
        }
        if (i >= skipFrames) c.chunkBuf[stored++] = decoded;
    }
    c.chunkStart = index;
    c.chunkLen = stored;
    c.chunkAccum = accum;
    c.chunkSampleIndex = c.sampleIndex;
    g_xmDiagChunkUs += micros() - __diagChunkStartUs;
    if (c.chunkLen == 0) return 0;
    return c.chunkBuf[0];
}

void XmPlayer::restartSamplePosition(int ch) {
    XmChannel& c = _channels[ch];
    c.samplePos16 = 0;
    c.srcIndex = 0;
    // Deliberately does NOT clear chunkLen/chunkBuf here -- see
    // chunkSampleIndex's comment. readRawSample() below re-validates the
    // cache against c.sampleIndex itself (already updated by
    // resolveSampleIndex() before this is called), so a same-sample
    // retrigger reuses the existing cache for free, while a genuine
    // sample change still invalidates correctly.
    c.havePair = false;

    if (c.sampleIndex < _numSamplesTotal) {
        const XmSample& s = _samples[c.sampleIndex];
        c.looping = s.looping;
        if (s.length > 0) {
            c.lastSrcSample = readRawSample(c, s, 0);
            c.nextSrcSample = readRawSample(c, s, 1);
            c.havePair = true;
        }
    }
}

void XmPlayer::resolveSampleIndex(XmChannel& c, int note) {
    if (c.instrument < 1 || c.instrument > MAX_INSTRUMENTS) { c.sampleIndex = 0xFF; return; }
    const XmInstrument& inst = _instruments[c.instrument - 1];
    if (inst.numSamplesUsed == 0) { c.sampleIndex = 0xFF; return; }
    int keymapIdx = note;
    if (keymapIdx < 0) keymapIdx = 0;
    if (keymapIdx > 95) keymapIdx = 95;
    uint8_t rel = inst.keymap[keymapIdx];
    if (rel >= inst.numSamplesUsed) rel = inst.numSamplesUsed - 1;
    c.sampleIndex = (uint8_t)(inst.firstSampleIndex + rel);
}

void XmPlayer::triggerNote(int ch, int note) {
    g_xmDiagNoteTrigs++;
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
    c.mixVolume = vol;
    c.pan = pan;
    c.mixPan = pan;
    c.period = xmNoteToPeriod(note + relNote, finetune);
    recomputeStep(c);

    c.volEnvTick = 0;
    c.panEnvTick = 0;
    c.keyOff = false;
    c.fadeoutVolume = 65536;

    restartSamplePosition(ch);
}

int16_t __not_in_flash_func(XmPlayer::readChannelSample)(XmChannel& c) { // see readRawSample()'s comment
    if (!c.havePair || c.sampleStep16 == 0 || c.sampleIndex >= _numSamplesTotal) return 0;
    const XmSample& s = _samples[c.sampleIndex];
    uint32_t loopEnd = s.loopStart + s.loopLength;

    uint16_t frac16 = (uint16_t)(c.samplePos16 & 0xFFFF);
    int32_t diff = (int32_t)c.nextSrcSample - (int32_t)c.lastSrcSample;
    int16_t interpolated = (int16_t)(c.lastSrcSample + ((diff * (int32_t)frac16) >> 16));

    c.samplePos16 += c.sampleStep16;
    while (c.havePair && c.samplePos16 >= 0x10000) {
        c.samplePos16 -= 0x10000;
        c.srcIndex++;
        if (c.looping && c.srcIndex >= loopEnd) {
            c.srcIndex = s.loopStart + ((c.srcIndex - loopEnd) % s.loopLength);
        } else if (!c.looping && c.srcIndex >= s.length) {
            c.havePair = false;
            break;
        }
        c.lastSrcSample = c.nextSrcSample;
        c.nextSrcSample = readRawSample(c, s, c.srcIndex + 1);
    }

    // >>6 instead of /64 -- mixVolume is always clamped to 0-64 by
    // construction (every writer already min()/max()s it), so this is a
    // genuine signed integer division on every active channel's every
    // single output sample -- the hottest call site in the whole player
    // -- on hardware with no divide instruction at all (Cortex-M0+).
    // Real-hardware diagnostics found the pure per-sample mixing cost
    // (excluding SD refills entirely) already consuming roughly half the
    // real-time budget at just 3-4 active channels; this is one
    // contributor. Same documented-approximation convention as the >>8
    // pan scaling just below in mixOneSample() and S3mPlayer's original.
    return (int16_t)(((int32_t)interpolated * (int32_t)c.mixVolume) >> 6);
}

void __not_in_flash_func(XmPlayer::mixOneSample)(int16_t& outL, int16_t& outR) { // see readRawSample()'s comment
    int32_t mixL = 0, mixR = 0;
    uint32_t __diagActive = 0;
    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];
        if (c.period == 0 || !c.havePair) continue;
        __diagActive++;
        int16_t s = readChannelSample(c);
        // >>8 instead of /255 -- see S3mPlayer::mixOneSample()'s identical comment.
        mixL += (s * (int32_t)(255 - c.mixPan)) >> 8;
        mixR += (s * (int32_t)c.mixPan) >> 8;
    }
    // >>6 instead of /64 -- _globalVolume is always clamped to 0-64,
    // same reasoning as readChannelSample()'s identical fix.
    mixL = ((mixL >> _mixShift) * (int32_t)_globalVolume) >> 6;
    mixR = ((mixR >> _mixShift) * (int32_t)_globalVolume) >> 6;
    // Even with _mixShift's channel-count-based headroom removed entirely
    // (see its declaration), real-hardware A/B listening against WAV/MIDI
    // still found tracker playback a bit quiet, so a further +25% makeup
    // gain here closes most of that remaining gap. Applied before the
    // diagnostics below so they measure what actually reaches
    // softClampMix(), not the pre-boost level.
    mixL += mixL >> 2;
    mixR += mixR >> 2;
    int32_t absL = mixL < 0 ? -mixL : mixL;
    int32_t absR = mixR < 0 ? -mixR : mixR;
    int32_t absMax = absL > absR ? absL : absR;
    if (absMax > g_xmDiagMaxAbsPreClamp) g_xmDiagMaxAbsPreClamp = absMax;
    if (absMax > 24576) g_xmDiagClips++; // now counts soft-knee engagement, not hard clips -- see softClampMix()
    mixL = softClampMix(mixL);
    mixR = softClampMix(mixR);
    outL = (int16_t)mixL;
    outR = (int16_t)mixR;
    int32_t jump = (int32_t)outL - (int32_t)g_xmDiagLastOutL;
    if (jump < 0) jump = -jump;
    if ((uint32_t)jump > g_xmDiagMaxJump) g_xmDiagMaxJump = (uint32_t)jump;
    g_xmDiagLastOutL = outL;
    g_xmDiagActiveChSum += __diagActive;
    g_xmDiagActiveChSamples++;
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
        if (_patternFile.read(&flag, 1) != 1) { g_xmDiagPatReadFails++; return; } // truncated/EOF -- stop

        uint8_t note = 0, instrument = 0, volume = 0, effect = 0, param = 0;
        bool hasNote = false, hasInstrument = false, hasVolume = false, hasEffect = false;

        if (flag & 0x80) {
            hasNote = (flag & 0x01) != 0;
            hasInstrument = (flag & 0x02) != 0;
            hasVolume = (flag & 0x04) != 0;
            hasEffect = (flag & 0x08) != 0;
            bool hasParam = (flag & 0x10) != 0;
            if (hasNote && _patternFile.read(&note, 1) != 1) { g_xmDiagPatReadFails++; return; }
            if (hasInstrument && _patternFile.read(&instrument, 1) != 1) { g_xmDiagPatReadFails++; return; }
            if (hasVolume && _patternFile.read(&volume, 1) != 1) { g_xmDiagPatReadFails++; return; }
            if (hasEffect && _patternFile.read(&effect, 1) != 1) { g_xmDiagPatReadFails++; return; }
            if (hasParam && _patternFile.read(&param, 1) != 1) { g_xmDiagPatReadFails++; return; }
        } else {
            // Uncompressed: the flag byte itself IS the note, and all
            // four other fields follow unconditionally.
            note = flag;
            hasNote = (note != 0);
            uint8_t b4[4];
            if (_patternFile.read(b4, 4) != 4) { g_xmDiagPatReadFails++; return; }
            instrument = b4[0];
            hasInstrument = (instrument != 0);
            volume = b4[1];
            hasVolume = (volume != 0);
            effect = b4[2];
            param = b4[3];
            hasEffect = true;
        }

        if (apply && ch < MAX_CHANNELS) {
            g_xmDiagApplyCells++;
            applyCell(ch, note, instrument, hasNote, hasInstrument, volume, hasVolume, effect, param, hasEffect);
        }
    }
}

void XmPlayer::applyCell(int ch, uint8_t note, uint8_t instrument, bool hasNote, bool hasInstrument,
                            uint8_t volume, bool hasVolume, uint8_t effect, uint8_t param, bool hasEffect) {
    XmChannel& c = _channels[ch];

    bool isTonePorta = hasEffect && (effect == 3 || effect == 5);
    bool isNoteDelay = hasEffect && effect == 14 /* E */ && (param >> 4) == 0xD && (param & 0x0F) != 0;

    if (hasInstrument && instrument >= 1 && instrument <= MAX_INSTRUMENTS) {
        c.instrument = instrument;
    }

    if (isNoteDelay) {
        c.noteDelayParam = param & 0x0F;
        c.pendingNote = hasNote ? note : 0xFF;
        c.pendingInstrument = hasInstrument ? instrument : 0;
    } else if (hasNote) {
        if (note == 97) {
            // Key Off -- see this class's header comment: fades out via
            // the volume envelope if one is enabled, else cuts immediately.
            c.keyOff = true;
            bool envOn = (c.instrument >= 1 && c.instrument <= MAX_INSTRUMENTS) &&
                         _instruments[c.instrument - 1].volEnv.enabled();
            if (!envOn) c.havePair = false;
        } else if (note >= 1 && note <= 96) {
            int noteAbs0 = note - 1;
            if (isTonePorta) {
                // Slides the CURRENTLY sounding sample toward the new
                // note -- uses the already-playing sample's own
                // finetune/relativeNoteNumber, not a newly-resolved one
                // (tone portamento never retriggers).
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
        // note==98..255: reserved/undefined, treated as a no-op defensively.
    }

    if (hasVolume) {
        // XM's volume-column mini-language -- writes into the SAME
        // per-channel fields the effect column uses (see this class's
        // header comment), not a parallel system.
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
            // Coarse tone-portamento speed -- approximate scaling, see
            // this class's header comment (flagged for libopenmpt tuning).
            c.tonePortaActiveRow = true;
            c.tonePortaSpeed = (uint8_t)((volume - 0xF0) << 4);
        }
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
        case 9: // Sample offset -- one-shot; see readRawSample()'s
                // comment on this being the one non-O(1) delta-decode path
            if (c.sampleIndex < _numSamplesTotal) {
                const XmSample& s = _samples[c.sampleIndex];
                uint32_t off = (uint32_t)param * 256;
                if (off < s.length) {
                    c.srcIndex = off;
                    c.samplePos16 = 0;
                    // Doesn't clear chunkLen -- see chunkSampleIndex's
                    // comment; readRawSample() below validates/reuses or
                    // invalidates the cache correctly either way.
                    c.lastSrcSample = readRawSample(c, s, off);
                    c.nextSrcSample = readRawSample(c, s, off + 1);
                    c.havePair = true;
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
            break;
        case 13: // D - Pattern break -- plain decimal row number, unlike
                 // MOD's BCD Dxx (flagged for libopenmpt verification)
            _patternBreakPending = true;
            _patternBreakRow = param;
            break;
        case 14: // E - Extended (sub-commands; EDy note-delay already handled above)
            if (!isNoteDelay) processExtendedCommand(ch, param);
            break;
        case 15: // F - Set speed/BPM: param < 0x20 -> ticks/row, else -> BPM
            if (param > 0) {
                if (param < 0x20) _speed = param;
                else _bpm = param;
            }
            break;
        case 16: // G - Set global volume
            _globalVolume = (param > 64) ? 64 : param;
            break;
        case 17: // H - Global volume slide -- own memory (_globalVolSlideParam)
            if (param != 0) _globalVolSlideParam = param;
            break;
        case 20: // K - Key off at tick `param` (0 == immediate, same tick)
            if (param == 0) {
                c.keyOff = true;
                bool envOn = (c.instrument >= 1 && c.instrument <= MAX_INSTRUMENTS) &&
                             _instruments[c.instrument - 1].volEnv.enabled();
                if (!envOn) c.havePair = false;
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
            if (param != 0) {
                c.tremorOnParam = (param >> 4) + 1;
                c.tremorOffParam = (param & 0x0F) + 1;
            }
            break;
        case 33: // X - Extra-fine portamento: sub-command via param's high nibble
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
            int p = (int)c.period - val;
            if (p < 1) p = 1;
            c.period = (uint16_t)p;
            recomputeStep(c);
            break;
        }
        case 0x2: // E2y - Fine porta down, one-shot
            c.period = (uint16_t)((int)c.period + val);
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
            if (val == 0) {
                c.patternLoopRow = (uint8_t)_row;
            } else {
                if (c.patternLoopCount == 0) c.patternLoopCount = val;
                else c.patternLoopCount--;
                if (c.patternLoopCount > 0) {
                    _patternBreakPending = true;
                    _patternBreakRow = c.patternLoopRow;
                    _positionJumpPending = true;
                    _positionJumpTarget = _orderPos; // stay on the current pattern
                }
            }
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
            break;
        case 0xB: // EBy - Fine volume slide down, one-shot
            c.volume = (uint8_t)max(0, (int)c.volume - val);
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
    g_xmDiagAdvRows++;
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
    }

    if (_patternBreakPending || _positionJumpPending) {
        int nextOrderPos = _positionJumpPending ? _positionJumpTarget : (_orderPos + 1);
        int nextRow = _patternBreakPending ? _patternBreakRow : 0;
        if (nextRow < 0) nextRow = 0;
        _orderPos = nextOrderPos;
        _row = nextRow;
        _patternBreakPending = false;
        _positionJumpPending = false;
    } else {
        _row++;
        int rowCount = (_openPatternNum >= 0 && _openPatternNum < MAX_PATTERNS) ? _patternRowCounts[_openPatternNum] : 64;
        if (rowCount <= 0 || rowCount > MAX_ROWS) rowCount = 64;
        if (_row >= rowCount) {
            _row = 0;
            _orderPos++;
        }
    }

    // XM's order table has no S3M-style skip/end marker bytes -- the
    // song simply ends when orderPos runs past _orderCount. Matches
    // MOD/S3M's "stop at the end, PLAY restarts" convention rather than
    // auto-looping via the header's restartPos field -- see load()'s comment.
    if (_orderPos < 0 || _orderPos >= _orderCount) {
        _state = STATE_DONE;
        return;
    }

    int patternNum = _orderTable[_orderPos];
    if (patternNum < 0 || patternNum >= MAX_PATTERNS) {
        // Corrupt/out-of-range order entry -- skip forward defensively
        // rather than looping forever on an unplayable position.
        _orderPos++;
        _row = 0;
        if (_orderPos >= _orderCount) { _state = STATE_DONE; return; }
        patternNum = _orderTable[_orderPos];
        if (patternNum < 0 || patternNum >= MAX_PATTERNS) { _state = STATE_DONE; return; }
    }
    // A jump/break to a row beyond the target pattern's own length is
    // invalid input -- clamp rather than read past it.
    int rowCount = _patternRowCounts[patternNum] > 0 ? _patternRowCounts[patternNum] : 64;
    if (_row >= rowCount) _row = 0;

    if (patternNum != _openPatternNum) openPatternForScan(patternNum);
    advanceToRow(_row);
}

void XmPlayer::advanceTick() {
    uint32_t __diagStart = micros();
    // Envelopes advance every tick unconditionally, including the
    // row-boundary tick -- see this class's header comment and
    // processEnvelopes()'s own comment on why this can't be folded into
    // processTickEffects() (which skips the row-boundary tick).
    processEnvelopes();

    _tick++;
    if (_tick >= _speed) {
        _tick = 0;
        if (_patternDelayRepeatsLeft > 0) {
            _patternDelayRepeatsLeft--;
        } else {
            advanceRow();
        }
    } else {
        processTickEffects();
    }
    g_xmDiagTickUs += micros() - __diagStart;
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

        c.mixVolume = (uint8_t)(((uint32_t)c.volume * volEnvValue / 64) * c.fadeoutVolume / 65536);
        // Deliberately simplified pan/envelope blend, not FT2's own
        // extremes-pulling formula -- see this class's header comment.
        int panOffset = ((int)panEnvValue - 32) * 2;
        int p = (int)c.pan + panOffset;
        if (p < 0) p = 0; else if (p > 255) p = 255;
        c.mixPan = (uint8_t)p;
    }
}

void XmPlayer::processTickEffects() {
    // Global volume slide (H) -- same regular-slide shape as a
    // per-channel A, applied to _globalVolume once per tick.
    {
        uint8_t hi = _globalVolSlideParam >> 4, lo = _globalVolSlideParam & 0x0F;
        if (hi != 0x0F && lo != 0x0F) {
            if (hi) _globalVolume = (uint8_t)min(64, (int)_globalVolume + hi);
            else if (lo) _globalVolume = (uint8_t)max(0, (int)_globalVolume - lo);
        }
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];

        if (c.noteDelayParam != 0 && _tick == c.noteDelayParam) {
            if (c.pendingInstrument != 0) c.instrument = c.pendingInstrument;
            if (c.pendingNote != 0xFF) triggerNote(ch, c.pendingNote);
            c.noteDelayParam = 0;
        }
        if (c.noteCutParam != 0xFF && _tick == c.noteCutParam) {
            c.volume = 0;
            c.noteCutParam = 0xFF;
        }
        if (c.keyOffTickParam != 0xFF && _tick == c.keyOffTickParam) {
            c.keyOff = true;
            bool envOn = (c.instrument >= 1 && c.instrument <= MAX_INSTRUMENTS) &&
                         _instruments[c.instrument - 1].volEnv.enabled();
            if (!envOn) c.havePair = false;
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
        }

        if (c.period == 0) continue;

        // Tremor (T) -- see S3mPlayer::processTickEffects()'s identical comment.
        bool tremorMuted = false;
        if (c.tremorOnParam > 0 || c.tremorOffParam > 0) {
            uint8_t cycleLen = c.tremorOnParam + c.tremorOffParam;
            if (cycleLen > 0) {
                uint8_t pos = c.tremorCounter % cycleLen;
                tremorMuted = (pos >= c.tremorOnParam);
                c.tremorCounter++;
            }
        }

        if (c.volSlideActiveRow) {
            uint8_t up = c.volSlideParam >> 4, down = c.volSlideParam & 0x0F;
            if (up) c.volume = (uint8_t)min(64, (int)c.volume + up);
            else if (down) c.volume = (uint8_t)max(0, (int)c.volume - down);
        }
        if (c.panSlideActiveRow) {
            uint8_t left = c.panSlideParam >> 4, right = c.panSlideParam & 0x0F;
            if (right) c.pan = (uint8_t)min(255, (int)c.pan + right * 2);
            else if (left) c.pan = (uint8_t)max(0, (int)c.pan - left * 2);
        }

        if (c.portaDownActiveRow && c.portaDownSpeed) {
            int p = (int)c.period + c.portaDownSpeed;
            if (p > 32000) p = 32000;
            c.period = (uint16_t)p;
            recomputeStep(c);
        }
        if (c.portaUpActiveRow && c.portaUpSpeed) {
            int p = (int)c.period - c.portaUpSpeed;
            if (p < 1) p = 1;
            c.period = (uint16_t)p;
            recomputeStep(c);
        }

        if (c.tonePortaActiveRow && c.portaTarget != 0 && c.tonePortaSpeed) {
            if (c.period < c.portaTarget) c.period = (uint16_t)min((int)c.portaTarget, (int)c.period + c.tonePortaSpeed);
            else if (c.period > c.portaTarget) c.period = (uint16_t)max((int)c.portaTarget, (int)c.period - c.tonePortaSpeed);
            recomputeStep(c);
        }

        uint16_t effectivePeriod = c.period;
        if (c.vibratoActiveRow && c.vibratoDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.vibratoPos / 64.0f;
            int16_t delta = (int16_t)(sinf(angle) * c.vibratoDepth * 4.0f);
            effectivePeriod = (uint16_t)max(1, (int)c.period + delta);
            c.vibratoPos = (uint8_t)((c.vibratoPos + c.vibratoSpeed) & 0x3F);
        }
        if (effectivePeriod != c.period) {
            uint16_t saved = c.period;
            c.period = effectivePeriod;
            recomputeStep(c);
            c.period = saved;
        }

        if (c.tremoloActiveRow && c.tremoloDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.tremoloPos / 64.0f;
            int delta = (int)(sinf(angle) * c.tremoloDepth * 4.0f);
            int v = (int)c.mixVolume + delta; // mixVolume already holds this tick's envelope-scaled value, see processEnvelopes()
            if (v < 0) v = 0; else if (v > 64) v = 64;
            c.mixVolume = (uint8_t)v;
            c.tremoloPos = (uint8_t)((c.tremoloPos + c.tremoloSpeed) & 0x3F);
        }
        if (tremorMuted) c.mixVolume = 0;

        // Arpeggio -- cycles the sounding pitch between the base note,
        // +x semitones, and +y semitones every tick within the row. In
        // linear mode a semitone is a fixed 64-unit period step; in Amiga
        // mode it's the same exponential period scale S3M/MOD use --
        // this IS the "same effect parameter, different musical interval
        // per mode" behavior flagged in this class's header comment, not
        // a bug.
        if (c.arpeggioParam != 0) {
            int cyclePos = _tick % 3;
            int semis = (cyclePos == 1) ? (c.arpeggioParam >> 4) : (cyclePos == 2) ? (c.arpeggioParam & 0x0F) : 0;
            if (semis != 0) {
                uint16_t arpPeriod;
                if (_linearFreqTable) {
                    int p = (int)c.period - semis * 64;
                    if (p < 1) p = 1;
                    arpPeriod = (uint16_t)p;
                } else {
                    float factor = powf(2.0f, -(float)semis / 12.0f);
                    arpPeriod = (uint16_t)max(1.0f, (float)c.period * factor);
                }
                uint16_t saved = c.period;
                c.period = arpPeriod;
                recomputeStep(c);
                c.period = saved;
            }
        }
    }
}

void __not_in_flash_func(XmPlayer::update)() { // see readRawSample()'s comment on __not_in_flash_func
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;

    uint32_t __diagCallStart = micros();
    g_xmDiagCalls++;

    // Smaller than S3mPlayer/ModPlayer's identical-in-spirit 512 --
    // real-hardware diagnostics found maxUs (worst single update() call)
    // already reaching 10-14ms at 512, with the ring buffer's free space
    // sitting at 0 essentially continuously (near-zero real-time margin).
    // A bigger CHUNK_SIZE (tried, then reverted after a real-hardware
    // crash/audio-corruption incident) would reduce AVERAGE SD cost but
    // increases WORST-CASE single-call latency further -- with margin
    // already at zero, that's the more likely failure mode than a memory
    // bug. Capping how many output frames (and therefore how many
    // channel-refill "boundary crossings") one call can produce bounds
    // the worst case a burst of several channels needing a refill at once
    // can cost, in exchange for update() being called more often -- which
    // the caller's main loop() already does every iteration, for free.
    const size_t MAX_FRAMES_PER_TICK_CALL = 128;
    size_t produced = 0;
    while (produced < MAX_FRAMES_PER_TICK_CALL) {
        size_t free = Synth::wavStreamFree();
        if (free < g_xmDiagMinFree) g_xmDiagMinFree = (uint32_t)free;
        if (free == 0) break;
        if (_samplesUntilNextTick == 0) {
            advanceTick();
            if (_state == STATE_DONE) break;
            _samplesUntilNextTick = ticksToSamples(_bpm);
        }
        int16_t l, r;
        uint32_t __diagMixStartUs = micros();
        mixOneSample(l, r);
        g_xmDiagMixUs += micros() - __diagMixStartUs;
        int16_t frame[2] = { l, r };
        if (Synth::wavStreamWrite(frame, 1) == 0) break;
        _samplesUntilNextTick--;
        produced++;
    }
    if (Synth::wavStreamTookUnderrun()) g_xmDiagUnderruns++;

    uint32_t callUs = micros() - __diagCallStart;
    g_xmDiagBusyUs += callUs;
    if (callUs > g_xmDiagMaxUs) g_xmDiagMaxUs = callUs;
    g_xmDiagProduced += (uint32_t)produced;
    if ((uint32_t)produced > g_xmDiagPeakOut) g_xmDiagPeakOut = (uint32_t)produced;

    static uint32_t lastPrintMs = 0;
    uint32_t nowMs = millis();
    if (DIAG_PRINT_ENABLED && nowMs - lastPrintMs >= 1000) {
        lastPrintMs = nowMs;
        uint32_t avgActiveCh100 = g_xmDiagActiveChSamples ? (g_xmDiagActiveChSum * 100 / g_xmDiagActiveChSamples) : 0;
        uint32_t seqAvgNs = g_xmDiagSeqMisses ? (g_xmDiagSeqUs * 1000 / g_xmDiagSeqMisses) : 0;
        uint32_t seekAvgNs = g_xmDiagSeekMisses ? (g_xmDiagSeekMissUs * 1000 / g_xmDiagSeekMisses) : 0;
        uint32_t nowUnderrunSamples = Synth::wavStreamUnderrunSamples();
        uint32_t underrunSamplesThisWindow = nowUnderrunSamples - g_xmDiagLastUnderrunSamples;
        g_xmDiagLastUnderrunSamples = nowUnderrunSamples;
        Serial.printf(
            "XM diag: calls=%lu produced=%lu peakOut=%lu busyUs=%lu maxUs=%lu "
            "minFree=%lu underruns=%lu underrunSamples=%lu avgActiveCh=%lu.%02lu chunkHits=%lu chunkMiss=%lu "
            "chunkUs=%lu seekReadUs=%lu mixUs=%lu advRows=%lu applyCells=%lu noteTrigs=%lu patReadFails=%lu tickUs=%lu "
            "seqMisses=%lu seqUs=%lu seqAvgNs=%lu seekMisses=%lu seekMissUs=%lu seekAvgNs=%lu "
            "clips=%lu maxAbsPreClamp=%ld maxJump=%lu\n",
            (unsigned long)g_xmDiagCalls, (unsigned long)g_xmDiagProduced, (unsigned long)g_xmDiagPeakOut,
            (unsigned long)g_xmDiagBusyUs, (unsigned long)g_xmDiagMaxUs,
            (unsigned long)(g_xmDiagMinFree == 0xFFFFFFFF ? 0 : g_xmDiagMinFree), (unsigned long)g_xmDiagUnderruns,
            (unsigned long)underrunSamplesThisWindow,
            (unsigned long)(avgActiveCh100 / 100), (unsigned long)(avgActiveCh100 % 100),
            (unsigned long)g_xmDiagChunkHits, (unsigned long)g_xmDiagChunkMisses,
            (unsigned long)g_xmDiagChunkUs, (unsigned long)g_xmDiagSeekReadUs, (unsigned long)g_xmDiagMixUs,
            (unsigned long)g_xmDiagAdvRows, (unsigned long)g_xmDiagApplyCells,
            (unsigned long)g_xmDiagNoteTrigs, (unsigned long)g_xmDiagPatReadFails, (unsigned long)g_xmDiagTickUs,
            (unsigned long)g_xmDiagSeqMisses, (unsigned long)g_xmDiagSeqUs, (unsigned long)seqAvgNs,
            (unsigned long)g_xmDiagSeekMisses, (unsigned long)g_xmDiagSeekMissUs, (unsigned long)seekAvgNs,
            (unsigned long)g_xmDiagClips, (long)g_xmDiagMaxAbsPreClamp, (unsigned long)g_xmDiagMaxJump
        );
        g_xmDiagCalls = 0; g_xmDiagProduced = 0; g_xmDiagPeakOut = 0;
        g_xmDiagBusyUs = 0; g_xmDiagMaxUs = 0;
        g_xmDiagMinFree = 0xFFFFFFFF;
        g_xmDiagUnderruns = 0;
        g_xmDiagActiveChSum = 0; g_xmDiagActiveChSamples = 0;
        g_xmDiagChunkHits = 0; g_xmDiagChunkMisses = 0;
        g_xmDiagChunkUs = 0; g_xmDiagSeekReadUs = 0; g_xmDiagMixUs = 0;
        g_xmDiagAdvRows = 0; g_xmDiagApplyCells = 0; g_xmDiagNoteTrigs = 0;
        g_xmDiagPatReadFails = 0;
        g_xmDiagTickUs = 0;
        g_xmDiagSeqMisses = 0; g_xmDiagSeqUs = 0;
        g_xmDiagSeekMisses = 0; g_xmDiagSeekMissUs = 0;
        g_xmDiagClips = 0; g_xmDiagMaxAbsPreClamp = 0; g_xmDiagMaxJump = 0;
    }
}

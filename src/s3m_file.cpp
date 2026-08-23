#include "s3m_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>

namespace {

// Same Amiga hardware clock constant ModPlayer uses -- see this class's
// header comment on why S3M's C2SPD-based pitch is converted through an
// equivalent pseudo-period rather than handled with its own parallel
// semitone-based math.
// float, not double -- this feeds s3mNoteToPeriod()/recomputeStep(), both
// called on every note trigger (and recomputeStep() again on every active
// portamento/vibrato/arpeggio tick). The RP2040's Cortex-M0+ has no
// hardware FPU at all, so double-precision math here is fully software-
// emulated and meaningfully slower than single-precision float would be
// for the same operations -- on a file with many simultaneous triggers
// (S3M's usual 12-16 channels vs MOD's 4, several of which can retrigger
// on the very same row), that cost concentrates right at the busiest
// musical moments, which is exactly when audible glitches showed up.
// Periods are integers 0-65535 either way, so float's ~7 significant
// digits are already far more precision than the result needs.
const float AMIGA_PAL_CLOCK = 7093789.2f;

// All S3M file-position pointers (instrument/pattern parapointers, and
// the sample-data pointer within an instrument header) are stored as
// actualOffset/16 -- multiply back by 16 to recover the real position.
inline uint32_t paraToOffset(uint32_t para) { return para * 16; }

// _mixShift budgets headroom for this many simultaneous full-volume
// channels, capped well below MAX_CHANNELS (20) -- see _mixShift's own
// comment for why a literal all-channels-at-once budget was needlessly
// quiet for real music, and softClampMix() below for what absorbs the
// (rare) moments actual content exceeds this budget. Same cap, same
// reasoning, duplicated in ModPlayer::load()/XmPlayer::load().
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

// -- Temporary real-hardware diagnostics --------------------------------
// See update()'s own comment further down for the full story -- added to
// test "The Reflex.s3m"'s audible slowdown+crackling in a busy passage.
// Removed once no longer needed.
//
// Silenced (not deleted) for now -- flip back to true to get the once/sec
// Serial print back; the counters themselves keep accumulating either way
// (cheap, harmless), only the print+reset is skipped while this is false.
const bool DIAG_PRINT_ENABLED = false;
uint32_t g_s3mDiagCalls = 0, g_s3mDiagBusyUs = 0, g_s3mDiagMaxUs = 0;
uint32_t g_s3mDiagMinFree = 0xFFFFFFFF;
uint32_t g_s3mDiagLastUnderrunSamples = 0;
uint32_t g_s3mDiagChunkHits = 0, g_s3mDiagChunkMisses = 0, g_s3mDiagChunkUs = 0;
uint32_t g_s3mDiagMixUs = 0;
uint32_t g_s3mDiagActiveChSum = 0, g_s3mDiagActiveChSamples = 0;

bool S3mPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!_file.open(path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }
    // See _patternFile's declaration -- a second, independent handle to
    // the same file, dedicated to pattern-data reads during playback.
    if (!_patternFile.open(path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    // The fixed header is 96 bytes (offsets 0-95): 28-byte name, 1-byte
    // DOS EOF marker, 1-byte type, 2 reserved, then OrdNum/InsNum/PatNum/
    // Flags/Cwtv/Ffi (2 bytes each) through offset 44, "SCRM" (4 bytes)
    // through 48, 6 single-byte fields (globalVol/speed/tempo/master
    // vol/ultra-click/default-pan) through 54, 8 reserved bytes, a
    // 2-byte "special" pointer (unused here) through 64, and finally the
    // 32-byte channel settings table at 64-95. The order table begins
    // immediately after, at offset 96 -- reading any more or less here
    // shifts every subsequent parapointer/order-table read by that same
    // amount, corrupting pattern offsets (this was originally miscounted
    // as 98 bytes with channels at 66, which desynced everything after
    // it and sent decodeRow() scanning garbage pattern data).
    uint8_t hdr[96];
    if (_file.read(hdr, 96) != 96) {
        strncpy(_error, "invalid S3M file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }
    if (hdr[29] != 16 || memcmp(hdr + 44, "SCRM", 4) != 0) {
        strncpy(_error, "unsupported format", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }

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
    if (ordNumReal > 4096 || insNumReal > 4096 || patNumReal > 4096) {
        strncpy(_error, "invalid S3M file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }

    _numChannels = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        _channelEnabled[i] = (hdr[64 + i] != 0xFF);
        if (_channelEnabled[i]) _numChannels++;
    }
    // See _mixShift's declaration and MIX_SHIFT_TYPICAL_CHANNELS's comment --
    // no channel-count-based headroom shift at all anymore, relying
    // entirely on softClampMix() for safety.
    _mixShift = 0;

    // Order table: bulk-read what fits in _orderTable, skip the rest
    // (still needed to keep the file position correct for what follows).
    int ordNum = (int)(ordNumReal < (uint16_t)MAX_ORDERS ? ordNumReal : (uint16_t)MAX_ORDERS);
    if (_file.read(_orderTable, ordNum) != ordNum) {
        strncpy(_error, "invalid S3M file", sizeof(_error) - 1);
        _file.close();
        _patternFile.close();
        _state = STATE_ERROR;
        return false;
    }
    if (ordNumReal > ordNum) _file.seekCur(ordNumReal - ordNum);
    _orderCount = ordNum;

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
            c.pan = 128; // center by default -- S3M's own panning effects are first-class,
                         // unlike MOD's convention-based hard-panning, so there's less need
                         // to guess at a per-channel-type default here
        }
    }

    _globalVolume = globalVol > 64 ? 64 : globalVol;
    _speed = initialSpeed > 0 ? initialSpeed : 6;
    _tempo = initialTempo > 0 ? initialTempo : 125;

    _orderPos = 0;
    _row = -1; // see currentRow()'s comment -- avoids skipping row 0 of the first pattern, same ModPlayer fix
    _tick = 0;
    _samplesUntilNextTick = ticksToSamples(_tempo);
    _patternDelayRepeatsLeft = 0;
    _patternBreakPending = false;
    _positionJumpPending = false;
    _openPatternNum = -1;
    _rowOffsetsValid = 0;

    _elapsedMsFrozen = 0;
    _lastResumeMicros = micros();
    _state = STATE_PAUSED;
    return true;
}

void S3mPlayer::close() {
    if (_file) _file.close();
    if (_patternFile) _patternFile.close();
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

void S3mPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _lastResumeMicros = micros();
    _state = STATE_PLAYING;
    // Pre-fill the ring buffer before the I2S consumer starts pulling
    // from it -- same fix XmPlayer::play() already needed tonight, for
    // the same reason: without this, the consumer can start draining a
    // completely empty ring before this class's own next update() call
    // even happens (the caller's next main-loop iteration, itself often
    // delayed further by a synchronous screen redraw), a guaranteed
    // underrun window. update() safely no-ops once the ring is full.
    for (int i = 0; i < 128 && Synth::wavStreamFree() > 0; i++) update();
    Synth::wavStreamSetActive(true);
}

void S3mPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _elapsedMsFrozen += (micros() - _lastResumeMicros) / 1000;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void S3mPlayer::stop() {
    close();
}

uint32_t S3mPlayer::elapsedMs() const {
    uint32_t extra = (_state == STATE_PLAYING) ? (micros() - _lastResumeMicros) / 1000 : 0;
    return _elapsedMsFrozen + extra;
}

uint32_t S3mPlayer::ticksToSamples(uint16_t tempo) {
    if (tempo == 0) tempo = 125;
    return (uint32_t)((double)SAMPLE_RATE * 2.5 / (double)tempo);
}

uint16_t S3mPlayer::s3mNoteToPeriod(uint8_t note, uint32_t c2spd) {
    int octave = note >> 4;
    int noteInOctave = note & 0x0F;
    int semitoneFromRef = (octave - 4) * 12 + noteInOctave; // reference: octave 4, note C
    float hz = (float)c2spd * powf(2.0f, (float)semitoneFromRef / 12.0f);
    if (hz < 1.0f) hz = 1.0f;
    float period = AMIGA_PAL_CLOCK / (2.0f * hz);
    if (period < 1.0f) period = 1.0f;
    if (period > 65535.0f) period = 65535.0f;
    return (uint16_t)(period + 0.5f);
}

void S3mPlayer::recomputeStep(S3mChannel& c) {
    if (c.period == 0) { c.sampleStep16 = 0; return; }
    float hz = AMIGA_PAL_CLOCK / ((float)c.period * 2.0f);
    c.sampleStep16 = (uint32_t)((hz / (float)SAMPLE_RATE) * 65536.0f);
}

// __not_in_flash_func: places this function's code in RAM instead of
// flash. RP2040 executes flash code through a small (16KB) XIP cache;
// this is one of the three hottest functions in the mix pipeline (called
// up to ~10-20 times per output sample under full polyphony), so cache
// misses there are paid on every call. RAM headroom is tight, but these
// are small functions -- worth the trade given the diagnostic data
// showed ~72% of per-sample time going to unexplained "pure compute"
// cost that -O2 alone barely touched.
int16_t __not_in_flash_func(S3mPlayer::readRawSample)(S3mChannel& c, const S3mSample& s, uint32_t index) {
    // c.looping is only ever set true when s.loopEnd > s.loopStart (see
    // restartSamplePosition()), so re-checking that here on every call is
    // redundant -- trimmed since this runs in the hottest per-sample path.
    if (c.looping && index >= s.loopEnd) {
        index = s.loopStart + ((index - s.loopEnd) % (s.loopEnd - s.loopStart));
    }
    if (index >= s.length) return 0;

    uint32_t bytesPerSample = s.is16Bit ? 2 : 1;
    if (c.chunkLen == 0 || c.chunkSample != c.sample ||
        index < c.chunkStart || index >= c.chunkStart + c.chunkLen) {
        g_s3mDiagChunkMisses++;
        uint32_t __diagStart = micros();
        uint32_t readFrames = S3mChannel::CHUNK_SIZE / bytesPerSample;
        if (index + readFrames > s.length) readFrames = s.length - index;
        _file.seekSet(s.fileOffset + index * bytesPerSample);
        int n = _file.read(c.chunkBuf, readFrames * bytesPerSample);
        c.chunkStart = index;
        c.chunkLen = (n > 0) ? ((uint32_t)n / bytesPerSample) : 0;
        c.chunkSample = c.sample;
        g_s3mDiagChunkUs += micros() - __diagStart;
        if (c.chunkLen == 0) return 0;
    } else {
        g_s3mDiagChunkHits++;
    }

    uint32_t pos = (index - c.chunkStart) * bytesPerSample;
    if (s.is16Bit) {
        // Read as unsigned first -- subtracting the bias must happen in
        // a wide-enough type before narrowing to int16_t, not after
        // already reinterpreting the bits as signed (which wraps values
        // >= 32768 the wrong way for the unsigned/"new format" case).
        uint16_t u = (uint16_t)(uint8_t)c.chunkBuf[pos] | ((uint16_t)(uint8_t)c.chunkBuf[pos + 1] << 8);
        return s.isSigned ? (int16_t)u : (int16_t)((int32_t)u - 32768);
    } else {
        uint8_t raw = (uint8_t)c.chunkBuf[pos];
        // Unsigned 8-bit (the common, "new format" case): center then
        // scale up, same convention WavPlayer already uses for unsigned
        // WAV PCM. Signed 8-bit (old format, rare): already centered,
        // just scale up.
        int16_t centered = s.isSigned ? (int8_t)raw : (int16_t)((int)raw - 128);
        return (int16_t)(centered << 8);
    }
}

void S3mPlayer::restartSamplePosition(int ch) {
    S3mChannel& c = _channels[ch];
    c.samplePos16 = 0;
    c.srcIndex = 0;
    // Deliberately does NOT clear chunkLen/chunkBuf here -- see
    // S3mChannel::chunkSample's comment. readRawSample() re-validates the
    // cache against c.sample itself, so a same-sample retrigger reuses
    // the existing cache for free, while a genuine sample change still
    // invalidates correctly.
    c.havePair = false;

    if (c.sample >= 1 && c.sample <= MAX_INSTRUMENTS) {
        const S3mSample& s = _samples[c.sample - 1];
        c.looping = s.loopEnd > s.loopStart;
        if (s.length > 0) {
            c.lastSrcSample = readRawSample(c, s, 0);
            c.nextSrcSample = readRawSample(c, s, 1);
            c.havePair = true;
        }
    }
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

int16_t __not_in_flash_func(S3mPlayer::readChannelSample)(S3mChannel& c) { // see readRawSample()'s comment
    if (!c.havePair || c.sampleStep16 == 0 || c.sample < 1) return 0;
    const S3mSample& s = _samples[c.sample - 1];

    uint16_t frac16 = (uint16_t)(c.samplePos16 & 0xFFFF);
    int32_t diff = (int32_t)c.nextSrcSample - (int32_t)c.lastSrcSample;
    int16_t interpolated = (int16_t)(c.lastSrcSample + ((diff * (int32_t)frac16) >> 16));

    c.samplePos16 += c.sampleStep16;
    while (c.havePair && c.samplePos16 >= 0x10000) {
        c.samplePos16 -= 0x10000;
        c.srcIndex++;
        if (c.looping && c.srcIndex >= s.loopEnd) { // see readRawSample()'s comment on this redundant-check trim
            c.srcIndex = s.loopStart + ((c.srcIndex - s.loopEnd) % (s.loopEnd - s.loopStart));
        } else if (!c.looping && c.srcIndex >= s.length) {
            c.havePair = false;
            break;
        }
        c.lastSrcSample = c.nextSrcSample;
        c.nextSrcSample = readRawSample(c, s, c.srcIndex + 1);
    }

    // >>6 instead of /64 -- mixVolume is always clamped to 0-64 by
    // construction. Real-hardware diagnostics (on "The Reflex.s3m", a
    // busy passage) found update() calls slowing to ~80-90/sec, ~11-19ms
    // each -- close enough to saturating the 1-second budget that a
    // little extra load tips it into a sustained, audible ring-buffer
    // drought (thousands of dropped samples/sec, not just a blip). This
    // is the same fix XmPlayer::readChannelSample() already got tonight
    // for the identical reason -- a genuine signed integer division, on
    // hardware with no divide instruction, at the single hottest call
    // site in the whole player (every active channel, every sample).
    return (int16_t)(((int32_t)interpolated * (int32_t)c.mixVolume) >> 6);
}

void __not_in_flash_func(S3mPlayer::mixOneSample)(int16_t& outL, int16_t& outR) { // see readRawSample()'s comment
    int32_t mixL = 0, mixR = 0;
    // _channelEnabled is fixed for the whole song (set once in load()),
    // so which/how-many channels are enabled never changes here -- no
    // need to (re-)count it every sample the way an earlier version did.
    uint32_t __diagActive = 0;
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (!_channelEnabled[ch]) continue;
        S3mChannel& c = _channels[ch];
        if (c.period == 0 || !c.havePair) continue;
        __diagActive++;
        int16_t s = readChannelSample(c);
        // >>8 (divide by 256) instead of /255 -- the RP2040's Cortex-M0+
        // has no hardware integer divide, so /255 here is a genuine
        // software-division call, times two, times every active channel,
        // every single output sample. The 256-vs-255 error is far below
        // audible (<0.4%); see _mixShift's comment for why this matters
        // more for S3M's usual 12-16 channels than it would for MOD's 4.
        mixL += (s * (int32_t)(255 - c.pan)) >> 8;
        mixR += (s * (int32_t)c.pan) >> 8;
    }
    // Same fixed-headroom-by-channel-count intent as ModPlayer::mixOneSample(),
    // approximated to a shift (see _mixShift's comment) instead of a
    // runtime division, plus global volume (V/W effects) as an additional
    // final scale. This final /64 was NOT actually a free compiler-emitted
    // shift despite the constant being a power of 2 -- a SIGNED dividend
    // needs a rounding correction a plain arithmetic shift doesn't provide,
    // so GCC emits a short correction sequence instead of one instruction.
    // >>6 directly (same real-hardware-motivated fix as
    // readChannelSample()'s identical change just above).
    mixL = ((mixL >> _mixShift) * (int32_t)_globalVolume) >> 6;
    mixR = ((mixR >> _mixShift) * (int32_t)_globalVolume) >> 6;
    // Even with _mixShift's channel-count-based headroom removed entirely
    // (see its declaration), real-hardware A/B listening against WAV/MIDI
    // still found tracker playback a bit quiet, so a further +25% makeup
    // gain here closes most of that remaining gap. softClampMix() below
    // absorbs the rare moments this pushes things over budget.
    mixL += mixL >> 2;
    mixR += mixR >> 2;
    mixL = softClampMix(mixL);
    mixR = softClampMix(mixR);
    outL = (int16_t)mixL;
    outR = (int16_t)mixR;
    g_s3mDiagActiveChSum += __diagActive;
    g_s3mDiagActiveChSamples++;
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
    // A real row has at most one entry per channel (MAX_CHANNELS) before
    // its terminator -- bounding the loop by that guards against ever
    // scanning far into a corrupt/mismatched file (e.g. a bad parapointer
    // pointing at non-pattern data) for an unbounded number of slow SD
    // reads if a stray 0x00 terminator byte is never encountered.
    for (int guard = 0; guard <= MAX_CHANNELS; guard++) {
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

void S3mPlayer::applyCell(int ch, uint8_t note, uint8_t sample, bool hasNote,
                            uint8_t volume, bool hasVolume, uint8_t command, uint8_t param, bool hasCommand) {
    S3mChannel& c = _channels[ch];

    // Command letters are stored 1-based (1=A .. 26=Z). Tone portamento
    // (G) and portamento+volslide (L) both mean "slide toward a target"
    // rather than "jump to it immediately", same distinction MOD's 3xx/
    // 5xy make from a plain note trigger.
    bool isTonePorta = hasCommand && (command == 7 /* G */ || command == 12 /* L */);
    bool isNoteDelay = hasCommand && command == 19 /* S */ && (param >> 4) == 0xD && (param & 0x0F) != 0;

    // Instrument change applies immediately regardless of a pending note
    // delay -- only the note trigger itself is deferred, same as
    // ModPlayer's identical EDy handling.
    if (hasNote && sample >= 1 && sample <= MAX_INSTRUMENTS) {
        c.sample = sample;
        c.volume = _samples[sample - 1].volume;
        c.mixVolume = c.volume; // see S3mChannel::mixVolume's comment -- keeps mixing correct
                                 // even for the handful of samples before the next tick resyncs it
    }

    // The volume column is S3M's own first-class per-row volume
    // override -- independent of any effect command in the same cell.
    if (hasVolume) {
        c.volume = (volume > 64) ? 64 : volume;
        c.mixVolume = c.volume;
    }

    if (isNoteDelay) {
        c.noteDelayParam = param & 0x0F;
        c.pendingNote = hasNote ? note : 0xFF;
        c.pendingSample = (hasNote && sample >= 1 && sample <= MAX_INSTRUMENTS) ? sample : 0;
    } else if (hasNote) {
        if (note == 0xFF) {
            // Note Off -- immediate silence; no envelope release to
            // model for a plain PCM instrument (S3M has none; envelopes
            // are an XM feature).
            c.havePair = false;
        } else if (note < 0xFE) {
            if (isTonePorta) {
                c.portaTarget = s3mNoteToPeriod(note, c.c2spd);
            } else {
                triggerNote(ch, note);
            }
        }
        // note == 0xFE: uncertain/rare marker, treated as a no-op defensively.
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
        case 5: // E - Portamento down: regular (per-tick, gated by
                // portaDownActiveRow), or one-shot fine/extra-fine via a
                // high-nibble range, applied once here (no gating needed).
            c.portaDownActiveRow = true;
            if (param != 0) c.portaDownSpeed = param;
            if ((c.portaDownSpeed & 0xF0) == 0xE0) {
                c.period = (uint16_t)min(65535, (int)c.period + (int)(c.portaDownSpeed & 0x0F) * 4);
                recomputeStep(c);
            } else if ((c.portaDownSpeed & 0xF0) == 0xF0) {
                c.period = (uint16_t)min(65535, (int)c.period + (int)(c.portaDownSpeed & 0x0F));
                recomputeStep(c);
            }
            break;
        case 6: // F - Portamento up (mirror of E)
            c.portaUpActiveRow = true;
            if (param != 0) c.portaUpSpeed = param;
            if ((c.portaUpSpeed & 0xF0) == 0xE0) {
                c.period = (uint16_t)max(1, (int)c.period - (int)(c.portaUpSpeed & 0x0F) * 4);
                recomputeStep(c);
            } else if ((c.portaUpSpeed & 0xF0) == 0xF0) {
                c.period = (uint16_t)max(1, (int)c.period - (int)(c.portaUpSpeed & 0x0F));
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
            if (param != 0) _globalVolSlideParam = param;
            break;
        case 24: // X - Set panning
            c.pan = param;
            break;
        case 25: // Y - Panbrello
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

    // Order table: 0xFF ends the song outright. 0xFE marks a skipped
    // position -- silently advance past it. S3M has no dedicated
    // "restart position" byte the way MOD does; a trailing position-jump
    // (B) effect serves the identical looping purpose in practice, and
    // is already handled generically above (it's just another jump).
    while (true) {
        if (_orderPos < 0 || _orderPos >= _orderCount) {
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
}

void S3mPlayer::advanceTick() {
    _tick++;
    if (_tick >= _speed) {
        _tick = 0;
        if (_patternDelayRepeatsLeft > 0) {
            // SEy: hold on the current row for one more full tick-cycle
            // instead of advancing -- per-tick effects keep applying
            // through these extra cycles, notes just don't re-trigger.
            _patternDelayRepeatsLeft--;
        } else {
            advanceRow();
        }
    } else {
        processTickEffects();
    }
}

void S3mPlayer::processTickEffects() {
    // Global volume slide (W) -- same regular-slide shape as a
    // per-channel D, applied to _globalVolume once per tick. Fine
    // variants aren't offered for W in the classic format, so no
    // 0xF-nibble special-case is needed here.
    {
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
            if (c.pendingSample != 0) {
                c.sample = c.pendingSample;
                c.volume = _samples[c.pendingSample - 1].volume;
            }
            if (c.pendingNote != 0xFF) triggerNote(ch, c.pendingNote);
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

        // Tremor (I): cycles the channel between audible and silent
        // every tremorOnParam/tremorOffParam ticks -- implemented as a
        // volume gate (0 while "off") rather than touching havePair, so
        // the sample position keeps advancing silently through the off
        // phase, same as real tremor (it doesn't restart on re-entry).
        bool tremorMuted = false;
        if (c.tremorOnParam > 0 || c.tremorOffParam > 0) {
            uint8_t cycleLen = c.tremorOnParam + c.tremorOffParam;
            if (cycleLen > 0) {
                uint8_t pos = c.tremorCounter % cycleLen;
                tremorMuted = (pos >= c.tremorOnParam);
                c.tremorCounter++;
            }
        }

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
        // Resync the mixing volume to the (possibly just-slid) true
        // volume before any transient effect below adjusts it further --
        // see S3mChannel::mixVolume's comment. Unconditional regardless
        // of volSlideActiveRow, since tremolo/tremor still need a fresh
        // baseline every tick even on rows with no active slide.
        c.mixVolume = c.volume;

        // Portamento up/down (E/F), gated by their ActiveRow flags (see
        // their declarations), clamped to the same period range
        // ModPlayer uses -- see its own comment on why an unclamped
        // slide is a real bug, not just a cosmetic one.
        if (c.portaDownActiveRow && c.portaDownSpeed &&
            (c.portaDownSpeed & 0xF0) != 0xE0 && (c.portaDownSpeed & 0xF0) != 0xF0) {
            int p = (int)c.period + c.portaDownSpeed;
            if (p > 27392) p = 27392; // ~113 Hz floor at the widest realistic C2SPD -- generous upper period bound
            c.period = (uint16_t)p;
            recomputeStep(c);
        }
        if (c.portaUpActiveRow && c.portaUpSpeed &&
            (c.portaUpSpeed & 0xF0) != 0xE0 && (c.portaUpSpeed & 0xF0) != 0xF0) {
            int p = (int)c.period - c.portaUpSpeed;
            if (p < 1) p = 1;
            c.period = (uint16_t)p;
            recomputeStep(c);
        }

        // Tone portamento (G, and the portamento half of L) -- slides
        // toward portaTarget at tonePortaSpeed/tick, gated by
        // tonePortaActiveRow.
        if (c.tonePortaActiveRow && c.portaTarget != 0 && c.tonePortaSpeed) {
            if (c.period < c.portaTarget) {
                c.period = (uint16_t)min((int)c.portaTarget, (int)c.period + c.tonePortaSpeed);
            } else if (c.period > c.portaTarget) {
                c.period = (uint16_t)max((int)c.portaTarget, (int)c.period - c.tonePortaSpeed);
            }
            recomputeStep(c);
        }

        // Vibrato (H/U, and the vibrato half of K) -- transient period
        // offset recomputed fresh each tick, not accumulated into
        // c.period itself, gated by vibratoActiveRow. U (fine vibrato)
        // reuses this with a quarter depth, flagged via vibratoDepth's
        // high bit (see applyCell()).
        uint16_t effectivePeriod = c.period;
        bool fineVibrato = (c.vibratoDepth & 0x80) != 0;
        uint8_t vibDepth = c.vibratoDepth & 0x7F;
        if (c.vibratoActiveRow && vibDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.vibratoPos / 64.0f;
            float depthScale = fineVibrato ? 1.0f : 4.0f;
            int16_t delta = (int16_t)(sinf(angle) * vibDepth * depthScale);
            effectivePeriod = (uint16_t)max(1, (int)c.period + delta);
            c.vibratoPos = (uint8_t)((c.vibratoPos + c.vibratoSpeed) & 0x3F);
        }
        if (effectivePeriod != c.period) {
            uint16_t savedPeriod = c.period;
            c.period = effectivePeriod;
            recomputeStep(c);
            c.period = savedPeriod;
        }

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

        // Arpeggio (J) -- cycles the sounding pitch between the base
        // note, +x semitones, and +y semitones every tick within the row.
        if (c.arpeggioParam != 0) {
            int cyclePos = _tick % 3;
            int semis = (cyclePos == 1) ? (c.arpeggioParam >> 4) : (cyclePos == 2) ? (c.arpeggioParam & 0x0F) : 0;
            if (semis != 0) {
                float factor = powf(2.0f, -(float)semis / 12.0f);
                uint16_t arpPeriod = (uint16_t)max(1.0f, (float)c.period * factor);
                uint16_t savedPeriod = c.period;
                c.period = arpPeriod;
                recomputeStep(c);
                c.period = savedPeriod;
            }
        }

        // Panbrello (Y) -- same idea as vibrato/tremolo, transient pan offset.
        if (c.panbrelloDepth) {
            float angle = 2.0f * (float)M_PI * (float)c.panbrelloPos / 32.0f;
            int delta = (int)(sinf(angle) * c.panbrelloDepth * 4.0f);
            int p = (int)c.pan + delta;
            if (p < 0) p = 0; else if (p > 255) p = 255;
            c.pan = (uint8_t)p; // panbrello is rare enough that a small permanent drift is an acceptable simplification vs. threading a second "effective pan" through mixOneSample()
            c.panbrelloPos = (uint8_t)((c.panbrelloPos + c.panbrelloSpeed) & 0x1F);
        }
    }
}

void __not_in_flash_func(S3mPlayer::update)() { // see readRawSample()'s comment on __not_in_flash_func
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;

    uint32_t __diagCallStart = micros();
    g_s3mDiagCalls++;

    // Was 512 -- lowered to match XmPlayer::update()'s identical fix.
    // Real-hardware diagnostics on "The Reflex.s3m"'s busy passage showed
    // maxUs reaching 12-19ms per call with only ~80-90 calls/sec even
    // BEFORE a sustained underrun started -- capping how many output
    // frames (and therefore how many channel-refill "boundary crossings")
    // one call can produce bounds the worst case a burst of several
    // channels needing a refill at once can cost, in exchange for
    // update() being called more often, which the caller's main loop()
    // already does every iteration, for free.
    const size_t MAX_FRAMES_PER_TICK_CALL = 128;
    size_t produced = 0;
    while (produced < MAX_FRAMES_PER_TICK_CALL) {
        size_t free = Synth::wavStreamFree();
        if (free < g_s3mDiagMinFree) g_s3mDiagMinFree = (uint32_t)free;
        if (free == 0) break;
        if (_samplesUntilNextTick == 0) {
            advanceTick();
            if (_state == STATE_DONE) break;
            _samplesUntilNextTick = ticksToSamples(_tempo);
        }
        int16_t l, r;
        uint32_t __diagMixStart = micros();
        mixOneSample(l, r);
        g_s3mDiagMixUs += micros() - __diagMixStart;
        int16_t frame[2] = { l, r };
        if (Synth::wavStreamWrite(frame, 1) == 0) break;
        _samplesUntilNextTick--;
        produced++;
    }

    uint32_t callUs = micros() - __diagCallStart;
    g_s3mDiagBusyUs += callUs;
    if (callUs > g_s3mDiagMaxUs) g_s3mDiagMaxUs = callUs;

    static uint32_t lastPrintMs = 0;
    uint32_t nowMs = millis();
    if (DIAG_PRINT_ENABLED && nowMs - lastPrintMs >= 1000) {
        lastPrintMs = nowMs;
        uint32_t nowUnderrunSamples = Synth::wavStreamUnderrunSamples();
        uint32_t underrunSamplesThisWindow = nowUnderrunSamples - g_s3mDiagLastUnderrunSamples;
        g_s3mDiagLastUnderrunSamples = nowUnderrunSamples;
        uint32_t avgActiveCh100 = g_s3mDiagActiveChSamples ? (g_s3mDiagActiveChSum * 100 / g_s3mDiagActiveChSamples) : 0;
        Serial.printf("S3M diag: calls=%lu busyUs=%lu maxUs=%lu minFree=%lu underrunSamples=%lu "
                       "chunkHits=%lu chunkMiss=%lu chunkUs=%lu mixUs=%lu avgActiveCh=%lu.%02lu\n",
                       (unsigned long)g_s3mDiagCalls, (unsigned long)g_s3mDiagBusyUs, (unsigned long)g_s3mDiagMaxUs,
                       (unsigned long)(g_s3mDiagMinFree == 0xFFFFFFFF ? 0 : g_s3mDiagMinFree),
                       (unsigned long)underrunSamplesThisWindow,
                       (unsigned long)g_s3mDiagChunkHits, (unsigned long)g_s3mDiagChunkMisses,
                       (unsigned long)g_s3mDiagChunkUs, (unsigned long)g_s3mDiagMixUs,
                       (unsigned long)(avgActiveCh100 / 100), (unsigned long)(avgActiveCh100 % 100));
        g_s3mDiagCalls = 0; g_s3mDiagBusyUs = 0; g_s3mDiagMaxUs = 0;
        g_s3mDiagMinFree = 0xFFFFFFFF;
        g_s3mDiagChunkHits = 0; g_s3mDiagChunkMisses = 0; g_s3mDiagChunkUs = 0;
        g_s3mDiagMixUs = 0;
        g_s3mDiagActiveChSum = 0; g_s3mDiagActiveChSamples = 0;
    }
}

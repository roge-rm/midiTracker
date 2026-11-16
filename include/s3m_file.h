#pragma once
#include "tracker_base.h"

// Scream Tracker 3 .s3m player on the shared tracker base (see
// tracker_base.h), following OpenMPT's S3M playback.
//  - Patterns are packed (a per-channel flag byte says which of note/
//    instrument/volume/effect follow, 0x00 ends the row), so reaching row
//    N means decoding the rows before it. _rowOffsets caches where each row
//    starts once it's been seen.
//  - Pitch uses OpenMPT's S3M periods: 8363 * 1712 / period Hz, a note's
//    period scaled by its sample's C2Spd. Slides move the period.
//  - Samples can be 8 or 16-bit, signed or unsigned. Stereo samples play
//    their left channel, ADPCM samples are silent.
//  - AdLib channels and instruments are silent.
//  - Files saved by ST3 share one effect memory across D E F I J K L Q R S.
//
// Effects: A-Z and the S sub-commands except M N P (not in ST3), S6x S7x
// S9x and Z (MIDI macro).
class S3mPlayer : public TrackerPlayer {
public:
    // The format allows 32 channels; data for PCM channels past
    // MAX_CHANNELS is skipped (see load()).
    static const int MAX_CHANNELS = 20;
    static const int MAX_INSTRUMENTS = 99;
    static const int MAX_PATTERNS = 100;
    static const int MAX_ORDERS = 256;

    bool load(const char* path) override;

private:
    struct S3mSample : TrackerSample {
        uint32_t c2spd = 8363;  // rate (Hz) at octave 4 note C
        uint8_t volume = 64;
        bool is16Bit = false;
        bool isSigned = false; // from the header's FileFormatVersion: 1 = signed, else unsigned
    };

    struct S3mChannel : TrackerVoice {
        uint8_t sample = 0;  // 1..MAX_INSTRUMENTS, 0 == none yet
        uint16_t volume = 256; // 0-256; tremolo and tremor only touch mixVolume
        uint16_t pan = 128;  // 0 (left) .. 256 (right)
        uint32_t c2spd = 8363;
        uint32_t period = 0;     // S3M period in 1/256 units, 0 == none
        uint32_t stepPeriod = 0; // period the step was worked out from (base plus vibrato/arpeggio)
        uint32_t step23 = 0;     // step in 1/2^23 frames, its low 7 bits spread over ticks
        uint8_t stepAcc = 0;
        uint8_t note = 0;        // last note as 12 * octave + semitone, for arpeggio
        bool cut = false;        // silent until the next note (^^, SCx, slid past the top)
        uint32_t portaTarget = 0;
        uint32_t prevOffset = 0; // a note with no instrument starts here (ST3)
        uint8_t pendIns = 0;     // lone instrument the next note without one picks up

        // Effect memory, never reset.
        uint8_t portaSlide = 0;   // G
        uint8_t volSlide = 0;     // D K L
        uint8_t portaUp = 0, portaDown = 0; // E F
        uint8_t tremorParam = 0, arpParam = 0, retrigParam = 0, cmdEx = 0;
        uint8_t vibSpeed = 0, vibDepth = 0;     // depth x4
        uint8_t tremSpeed = 0, tremDepth = 0;   // depth x4
        uint8_t panbSpeed = 0, panbDepth = 0;
        uint8_t offset = 0, hiOffset = 0, tempo = 0;
        uint8_t vibType = 0, tremType = 0, panbType = 0;
        uint8_t vibPos = 0, tremPos = 0, panbPos = 0;
        uint8_t retrigCount = 0, tremorCount = 0;
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        bool glissando = false;

        // This row's effect (0 == none) with memory applied, and the cell
        // parts SDx holds back.
        uint8_t cmd = 0, param = 0;
        uint8_t delayTick = 0;
        uint8_t pendNote = 0xFF, pendSample = 0, pendVolume = 0xFF;
        bool fineVibrato = false; // the row's vibrato is U
        int16_t panbDelta = 0;
    };

    // Each refill reads 1024 bytes: 1024 frames of 8-bit or 512 of 16-bit.
    static const int CHUNK_FRAMES = 2048; // a power of two

    S3mSample _samples[MAX_INSTRUMENTS];
    uint32_t _patternOffsets[MAX_PATTERNS]; // absolute, from the parapointers
    uint8_t _orderTable[MAX_ORDERS] = {0};
    uint8_t _chanSlot[32]; // file channel to player channel, 0xFF == skipped
    uint8_t _songVolume = 64; // V and W, 0-64; _globalVolume adds the master volume
    uint8_t _preamp = 48;     // master volume, 48 == unchanged
    uint8_t _globalSlide = 0; // W memory
    bool _globalSlideRow = false;

    // Behaviour picked from the header.
    bool _st3Memory = false;  // shared effect memory
    bool _fastSlides = false; // volume slides on tick 0 too
    bool _amigaLimits = false;
    bool _upperLimit = false; // periods stop at 32767
    bool _oldVibrato = false; // vibrato twice as deep
    bool _sampleSwap = false; // an instrument without a note swaps the sample (ST3 on a SoundBlaster)
    bool _portaKeepsSample = false; // tone porta to a new instrument keeps the old sample (ST3 on a GUS)
    uint8_t _rowRepeats = 0;  // SEx repeats left
    uint8_t _loopRow = 0, _loopCount = 0; // SBx, one for the whole song in ST3
    bool _loopPending = false;
    bool _limitPeriod = false; // a note stops at the lowest period (ModPlug, Schism)
    bool _hertz = false;      // pitch as OpenMPT plays Impulse Tracker and Schism files: notes in Hz
    uint32_t _minPeriod = 64 << 8;

    S3mChannel _channels[MAX_CHANNELS];
    int16_t _chunks[MAX_CHANNELS][CHUNK_FRAMES];
    TrackerTail _tails[MAX_CHANNELS];
    int16_t _tailPool[TAIL_POOL_FRAMES];

    // Pattern scan state: the open pattern, the start offset of each row
    // seen so far, and the read cursor for scanning forward.
    int _openPatternNum = -1;
    uint32_t _rowOffsets[64];
    int _rowOffsetsValid = 0;
    uint32_t _scanCursor = 0;

    // Note index (12 * octave + semitone) and C2Spd to a period.
    uint32_t periodForNote(int note, uint32_t c2spd) const;
    int noteForPeriod(uint32_t period, uint32_t c2spd) const; // lowest note at or above the period's pitch

    void recomputeStep(S3mChannel& c, uint32_t period); // step for `period`, leaves c.period alone
    void setSample(S3mChannel& c, uint8_t sample);
    void startNote(S3mChannel& c, uint32_t pos); // position to pos, or silent past the end

    void openPatternForScan(int num);
    void advanceToRow(int targetRow);
    void decodeRow(bool apply);
    void applyNoteColumns(S3mChannel& c, uint8_t note, uint8_t sample, uint8_t volume, bool porta, bool hasOffset);
    void applyCell(int ch, uint8_t note, uint8_t sample, uint8_t volume, uint8_t command, uint8_t param);
    void sampleOffset(S3mChannel& c);

    // Effects that run every tick of a row, tick 0 included.
    void runTick(bool firstTick);
    void slideVolume(S3mChannel& c, bool firstTick);
    void slidePeriod(S3mChannel& c, int32_t amount, bool tonePorta);
    void retrig(S3mChannel& c);
    void advanceRow() override;
    void processTickEffects() override;
    void tickEnd() override; // gains, pan and step to the mixer
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;
};

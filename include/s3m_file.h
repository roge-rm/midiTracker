#pragma once
#include "tracker_base.h"

// Scream Tracker 3 .s3m player on the shared tracker base (see
// tracker_base.h).
//  - Patterns are packed (a per-channel flag byte says which of note/
//    instrument/volume/effect follow, 0x00 ends the row), so reaching row
//    N means decoding the rows before it. _rowOffsets caches where each row
//    starts once it's been seen.
//  - Pitch comes from each instrument's C2SPD (rate at octave 4 note C),
//    converted to an Amiga-style period so slides work like MOD's.
//  - Samples can be 8 or 16-bit, signed or unsigned.
//
// Effects: A-Z and the S sub-commands except Z (MIDI macro).
class S3mPlayer : public TrackerPlayer {
public:
    // The format allows 32; files on hand top out at 16. Data for channels
    // at or above MAX_CHANNELS is skipped (see decodeRow()).
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
        bool isStereo = false; // played as mono (left channel only)
    };

    struct S3mChannel : TrackerVoice {
        uint8_t sample = 0;  // 1..MAX_INSTRUMENTS, 0 == none yet
        uint8_t volume = 64; // what defaults, the volume column and slides set; tremolo/tremor only touch mixVolume
        uint16_t c2spd = 8363;
        // ST3 period (4x an Amiga period) from s3mNoteToPeriod(); 0 == silent.
        uint32_t period = 0;
        uint32_t stepPeriod = 0; // period sampleStep16 was worked out from (base plus vibrato/arpeggio)
        uint8_t pan = 128; // 0 (left) .. 255 (right)

        // Effect memory: a 00 param reuses the last value. Never reset.
        uint8_t portaUpSpeed = 0, portaDownSpeed = 0;
        uint32_t portaTarget = 0; // tone portamento target period
        uint8_t tonePortaSpeed = 0;
        uint8_t vibratoSpeed = 0, vibratoDepth = 0, vibratoPos = 0;
        uint8_t tremoloSpeed = 0, tremoloDepth = 0, tremoloPos = 0;
        uint8_t panbrelloSpeed = 0, panbrelloDepth = 0, panbrelloPos = 0;
        uint8_t volSlideParam = 0;
        uint8_t retrigParam = 0;    // Qxy low nibble
        uint8_t retrigVolSlide = 0; // Qxy high nibble
        uint8_t arpeggioParam = 0;  // reset each row
        uint8_t tremorOnParam = 0, tremorOffParam = 0, tremorCounter = 0; // Ixy
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0; // reset each row
        uint8_t pendingNote = 0xFF, pendingSample = 0, pendingVolume = 0xFF; // for SDy, 0xFF/0 == none
        uint8_t noteCutParam = 0xFF; // reset each row
        bool glissando = false;

        // Whether each continuing effect was given on this row. Reset at
        // the start of every row, set in applyCell().
        bool volSlideActiveRow = false;
        bool portaDownActiveRow = false, portaUpActiveRow = false;
        bool tonePortaActiveRow = false;
        bool vibratoActiveRow = false;
        bool tremoloActiveRow = false;
        bool retrigActiveRow = false;
        bool tremorActiveRow = false;
        bool panbrelloActiveRow = false;
        int8_t panbrelloOffset = 0; // added to pan in the mix while panbrello runs
    };

    // Each refill reads 1024 bytes: 1024 frames of 8-bit or 512 of 16-bit.
    static const int CHUNK_BYTES = 1024;
    static const int CHUNK_FRAMES = 1024;

    S3mSample _samples[MAX_INSTRUMENTS];
    uint32_t _patternOffsets[MAX_PATTERNS]; // absolute, from the parapointers
    uint8_t _orderTable[MAX_ORDERS] = {0};
    bool _channelEnabled[MAX_CHANNELS] = {false}; // channels can be enabled sparsely
    uint8_t _globalVolSlideParam = 0; // W's own memory
    bool _globalVolSlideActiveRow = false;

    S3mChannel _channels[MAX_CHANNELS];
    int16_t _chunks[MAX_CHANNELS][CHUNK_FRAMES];

    // Pattern scan state: the open pattern, the start offset of each row
    // seen so far, and the read cursor for scanning forward.
    int _openPatternNum = -1;
    uint32_t _rowOffsets[64];
    int _rowOffsetsValid = 0;
    uint32_t _scanCursor = 0;

    // S3M note byte ((octave<<4)|note) plus C2SPD to an ST3 period.
    static uint32_t s3mNoteToPeriod(uint8_t note, uint32_t c2spd);

    void recomputeStep(S3mChannel& c) { recomputeStep(c, c.period); }
    void recomputeStep(S3mChannel& c, uint32_t period); // step for `period`, leaves c.period alone
    void setSample(S3mChannel& c, uint8_t sample);
    void restartSamplePosition(int ch); // position back to 0, pitch untouched
    void triggerNote(int ch, uint8_t note); // sets pitch from `note`, then restartSamplePosition()

    void openPatternForScan(int num);
    // Applies row `targetRow` of the open pattern: a direct seek if its
    // offset is cached, else scans forward from the last cached row.
    void advanceToRow(int targetRow);
    // Reads one packed row from the current position; `apply` == false
    // only parses it to find its length.
    void decodeRow(bool apply);
    // Instrument, volume column and note of one cell, in that order, on its
    // row or on the SDy delay tick. 0 / 0xFF / 0xFF == none.
    void applyNoteColumns(int ch, uint8_t note, uint8_t sample, uint8_t volume, bool isTonePorta);
    void applyCell(int ch, uint8_t note, uint8_t sample, bool hasNote,
                   uint8_t volume, bool hasVolume, uint8_t command, uint8_t param, bool hasCommand);
    void processExtendedTickZero(int ch, uint8_t command, uint8_t param); // S sub-commands

    bool tremorStep(S3mChannel& c); // one tick of tremor, true == muted
    void advanceRow() override;
    void processTickEffects() override;
    void tickEnd() override; // copies pan to the mixer
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;
};

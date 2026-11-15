#pragma once
#include "tracker_base.h"

// ProTracker-family .mod player on the shared tracker base (see
// tracker_base.h). Pattern rows are read by fixed offset, samples are
// 8-bit signed PCM after the patterns.
//
// Formats: "M.K."/"M!K!"/"FLT4" (4 channels), "6CHN", "8CHN"/"FLT8"/
// "CD81"/"OKTA" (8), and the numeric "nCHN"/"nnCH" tags up to
// MAX_MOD_CHANNELS. Anything else is STATE_ERROR.
//
// Effects: the ProTracker 0-F set and E0-EF except EFy (invert loop).
// Finetune uses the exponential pitch formula instead of the Amiga
// finetune tables, so periods aren't bit-exact to the hardware.
//
// Reaching the end of the order table jumps to the restart position and
// keeps playing; an out-of-range restart position is STATE_DONE.
class ModPlayer : public TrackerPlayer {
public:
    static const int MAX_MOD_CHANNELS = 32;
    static const int MAX_SAMPLES = 31;

    bool load(const char* path) override;

private:
    struct ModSample : TrackerSample {
        int8_t finetune = 0; // -8..7
        uint8_t volume = 64; // default volume, 0-64
    };

    struct ModChannel : TrackerVoice {
        uint8_t sample = 0; // 1..31, 0 == none yet
        uint8_t volume = 64;
        uint16_t period = 0; // Amiga period; 0 == silent
        uint8_t pan = 128;   // 0 (left) .. 255 (right)

        // Effect memory: a 00 param reuses the last value of that effect.
        uint8_t portaUpSpeed = 0, portaDownSpeed = 0;
        uint16_t portaTarget = 0;
        uint8_t tonePortaSpeed = 0;
        uint8_t vibratoSpeed = 0, vibratoDepth = 0;
        uint8_t vibratoPos = 0; // 0..63, one waveform cycle
        uint8_t tremoloSpeed = 0, tremoloDepth = 0;
        uint8_t tremoloPos = 0;
        uint8_t volSlideParam = 0;
        uint8_t sampleOffsetParam = 0;
        uint8_t retrigParam = 0;   // E9y: retrigger every y ticks, 0 == none; reset each row
        uint8_t arpeggioParam = 0; // reset each row
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0;   // EDy: tick to trigger on, 0 == none; reset each row
        uint16_t noteDelayPeriod = 0; // period for the delayed trigger, 0 == retrigger only
        uint8_t noteCutParam = 0xFF;  // ECy: tick to cut on, 0xFF == none; reset each row
        bool glissando = false;       // E3y
    };

    // 256 frames per voice cache.
    static const int CHUNK_FRAMES = 256;

    ModSample _samples[MAX_SAMPLES];
    uint8_t _orderTable[128] = {0};
    int _restartPos = 0;
    int _numPatterns = 0;
    uint32_t _patternDataStart = 0;

    ModChannel _channels[MAX_MOD_CHANNELS];
    int16_t _chunks[MAX_MOD_CHANNELS][CHUNK_FRAMES];

    static uint16_t periodForNote(int noteIndex); // finetune-0 table, noteIndex 0..35
    static int nearestNoteIndexForPeriod(uint16_t period);
    static uint16_t applyFinetune(uint16_t basePeriod, int8_t finetune);

    void setSample(ModChannel& c, uint8_t sampleNum);
    void triggerNote(int ch); // restarts the sample from the current period
    void recomputeStep(ModChannel& c);

    void advanceRow() override;
    void processTickEffects() override; // ticks 1..speed-1
    void tickEnd() override;            // copies volume/pan to the mixer
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;

    void processCellTriggers(int ch, const uint8_t cell[4]);
    void processExtendedTickZero(int ch, uint8_t param);
};

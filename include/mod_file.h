#pragma once
#include "tracker_base.h"

// ProTracker-family .mod player on the shared tracker base (see
// tracker_base.h). Pattern rows are read by fixed offset, samples are
// 8-bit signed PCM after the patterns.
//
// Formats: "M.K."/"M!K!"/"M&K!"/"FLT4"/"N.T."/"FEST"/"NSMS"/"LARD"/"PATT"
// (4 channels), "TDZ1-3", "8CHN"/"CD81"/"OKTA"/"OCTA" (8), "FLT8"
// (Startrekker, two 4-channel halves per pattern), the numeric
// "xCHN"/"xxCH"/"xxCN" tags up to MAX_MOD_CHANNELS, and untagged
// 15-sample Soundtracker files. Anything else is STATE_ERROR.
//
// Effects: the ProTracker 0-F set and E0-EE, played as OpenMPT plays a
// MOD: ProTracker's finetune table and the vibrato and tremolo
// waveforms. Files that look like ProTracker's (4-channel M.K. with
// notes in the Amiga range and no loop length of 0) also get its period
// limits, tempo changes from the next tick, its sample offset quirks and
// the sample swap at the loop end. EFx (invert loop) isn't done.
//
// Periods are kept in quarter periods, as OpenMPT does, so finetuned
// notes in the upper octaves keep their fraction.
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
        uint8_t finetune = 0; // 0..15, ProTracker's nibble (8..15 == -8..-1)
        uint8_t volume = 64;  // default volume, 0-64
    };

    struct ModChannel : TrackerVoice {
        uint8_t sample = 0;   // 1..31 from the last sample number, 0 == none yet
        uint8_t finetune = 0; // 0..15, from the sample number or E5x
        uint8_t volume = 64;
        uint16_t period = 0;     // quarter periods; 0 == silent
        uint16_t stepPeriod = 0; // period sampleStep16 was worked out from (base plus vibrato/arpeggio)
        uint32_t step23 = 0;     // exact step for stepPeriod, 16.23
        uint8_t stepAcc = 0;     // step23's low 7 bits carried from tick to tick
        uint8_t pan = 128;       // 0 (left) .. 255 (right)

        // This row's effect and parameter, run on ticks 1+. Porta up/down
        // and volume slide use the parameter as given, 00 does nothing.
        uint8_t fx = 0, fxParam = 0;
        int8_t tremoloDelta = 0; // added to volume in the mix, 0 outside tremolo rows and the tick after

        // Effect memory: a 00 param reuses the last value of that effect.
        uint16_t portaTarget = 0; // quarter periods
        uint8_t tonePortaSpeed = 0;
        uint8_t vibratoSpeed = 0, vibratoDepth = 0;
        uint8_t vibratoPos = 0; // 0..63, one waveform cycle
        uint8_t tremoloSpeed = 0, tremoloDepth = 0;
        uint8_t tremoloPos = 0;
        uint8_t vibratoWave = 0, tremoloWave = 0; // E4x/E7x: 0 sine, 1 ramp, 2-3 square; +4 == don't reset on a new note
        uint8_t sampleOffsetParam = 0;
        uint32_t startOffset = 0;  // where a note starts in the sample, moved by 9xx in ProTracker mode
        bool startPastEnd = false; // a 9xx went past the end: notes play only the loop until the next sample number
        uint8_t retrigParam = 0;   // E9y: retrigger every y ticks, 0 == none; reset each row
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0;   // EDy: tick to trigger on, 0 == none; reset each row
        uint16_t noteDelayPeriod = 0; // period for the delayed trigger, 0 == retrigger only
        uint8_t noteCutParam = 0xFF;  // ECy: tick to cut on, 0xFF == none; reset each row
        bool glissando = false;       // E3y
        const ModSample* swapTo = nullptr; // sample to swap to at the loop end, see swapSample()
        uint32_t swapLastPos = 0;          // srcIndex at the last check, to see the loop wrap
    };

    // Frames per voice cache, a power of two.
    static const int CHUNK_FRAMES = 1024;

    ModSample _samples[MAX_SAMPLES];
    uint8_t _orderTable[128] = {0};
    int _restartPos = 0;
    int _numPatterns = 0;
    uint32_t _patternDataStart = 0;
    uint16_t _nextTempo = 0;   // Fxx tempo for the next tick, 0 == none
    bool _ptMode = false;      // ProTracker mode: Amiga period limits (see clampPeriod()) and tempo from the next tick
    bool _startrekker = false; // FLT4/FLT8: Fxx is always speed
    bool _flt8 = false;        // Startrekker FLT8 pattern layout

    ModChannel _channels[MAX_MOD_CHANNELS];
    int16_t _chunks[MAX_MOD_CHANNELS][CHUNK_FRAMES];
    TrackerTail _tails[MAX_MOD_CHANNELS];
    int16_t _tailPool[TAIL_POOL_FRAMES];

    static const int NUM_NOTES = 72; // 6 octaves, 1712 down to 28
    static int noteForPeriod(uint16_t rawPeriod); // nearest note to a pattern period, 0..71
    static uint16_t periodForNote(int note, uint8_t finetune); // quarter periods
    static int noteAtOrAbove(uint16_t period, uint8_t finetune); // first note at or above this pitch
    uint16_t clampPeriod(uint16_t period, uint8_t finetune) const;

    void triggerNote(int ch, bool newNote); // restarts the sample from the current period; a new note resets vibrato and tremolo
    void swapSample(ModChannel& c);
    void startAt(ModChannel& c, uint32_t pos); // starts c's sample at pos, or its loop or silence past the end
    void recomputeStep(ModChannel& c) { recomputeStep(c, c.period); }
    void recomputeStep(ModChannel& c, uint16_t period); // step for `period`, leaves c.period alone

    void tickStart() override;
    void advanceRow() override;
    void processTickEffects() override; // ticks 1..speed-1
    void tickEnd() override;            // copies volume (plus tremolo) and pan to the mixer
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;

    void processCellTriggers(int ch, const uint8_t cell[4]);
    void processExtendedTickZero(int ch, uint8_t param, bool hasNote);
};

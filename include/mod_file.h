#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Streams a ProTracker-family .mod file directly off the SD card and
// mixes it down to a stereo stream fed into Synth's WAV playback ring
// buffer (see Synth::wavStream*() in synth.h) -- the same output path
// WavPlayer uses, just with ModPlayer doing its own multi-channel mixing
// on core 0 first, rather than resampling one linear stream. Neither
// pattern data nor sample (instrument) PCM data is ever loaded into RAM
// in full -- both are read on demand from fixed, computed byte offsets
// within the file (patterns immediately after the 1084-byte header,
// samples immediately after the pattern data), same "stream everything"
// convention as MidiPlayer/WavPlayer.
//
// Format support: the common "M.K."/"M!K!"/"FLT4" (4 channel), "6CHN" (6
// channel), and "8CHN"/"FLT8"/"CD81"/"OKTA" (8 channel) tags, plus the
// generic "nCHN"/"nnCH" numeric-channel-count convention up to
// MAX_MOD_CHANNELS. Rejects anything else (STATE_ERROR, see
// errorMessage()) rather than guessing.
//
// Effects: the full ProTracker 0-F command set (including all E0-EF
// extended sub-effects) except EFy (Invert Loop/Funk Repeat, an obscure
// effect that mutates sample data in place -- awkward against a
// streamed-from-SD sample and skipped even by many full-featured
// players). Finetune is applied via the mathematically-equivalent
// exponential pitch formula rather than a second historical lookup table
// per finetune step -- musically identical, just not bit-for-bit
// identical to original Amiga hardware period rounding.
//
// No seek/scrub: unlike a fixed-duration PCM stream, a tracker's
// "position" isn't a time offset (tempo itself changes via effects
// during the song), so there's no simple target to jump to. PLAY/PAUSE/
// stop-to-start only.
//
// Looping: honors the classic MOD "restart position" byte -- reaching
// the end of the pattern order table jumps back into the song and keeps
// playing (same "runs until you stop it" feel as LooperMode) rather than
// finishing, unless the restart position is itself out of range, which
// is STATE_DONE.
class ModPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    static const int MAX_MOD_CHANNELS = 32;
    static const int MAX_SAMPLES = 31;

    // Parses the header (song name, sample headers, order table, format
    // tag, pattern count) and computes pattern/sample byte offsets.
    // Leaves the player in STATE_PAUSED (ready to play()) on success, or
    // STATE_ERROR (see errorMessage()) on failure. Does not touch Synth's
    // stream at all -- that only starts once play() is first called.
    bool load(const char* path);

    // Closes the file handle and resets to STATE_IDLE. Also resets
    // Synth's WAV stream (see Synth::wavStreamReset()).
    void close();

    // Call every loop() iteration regardless of play/pause state -- same
    // "keep the ring buffer topped up unconditionally" reasoning as
    // WavPlayer::update() (see its own comment), just with per-sample
    // multi-channel mixing (see mixOneSample()) standing in for
    // WavPlayer's single-stream resample.
    void update();

    void play();  // resume/start -- unmutes Synth's stream
    void pause(); // hold in place, mutes Synth's stream without discarding buffered audio
    void stop();  // stop + close()

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t elapsedMs() const; // wall-clock based, same reasoning as WavPlayer::elapsedMs()

    // Position/format info for the UI -- there's no fixed "total time"
    // (see this class's header comment on looping), so the screen shows
    // position within the song instead of a progress bar.
    int patternOrderPosition() const { return _orderPos; }
    int songLength() const { return _songLength; }
    int currentRow() const { return _row < 0 ? 0 : _row; } // _row is briefly -1 immediately after load(), before the first row loads
    int channelCount() const { return _numChannels; }
    int instrumentCount() const { return _numInstruments; }

private:
    struct ModSample {
        uint32_t fileOffset = 0; // absolute byte offset of this instrument's PCM data
        uint32_t length = 0;     // in samples (== bytes, 8-bit mono PCM)
        uint32_t loopStart = 0;  // in samples
        uint32_t loopLength = 0; // in samples; <=1 means "no loop" per MOD convention
        int8_t finetune = 0;     // -8..7
        uint8_t volume = 64;     // default volume, 0-64
    };

    struct ModChannel {
        uint8_t sample = 0; // 1..31, 0 == none assigned yet
        uint8_t volume = 64;
        uint16_t period = 0; // current Amiga period (pitch); 0 == silent

        // Resample state -- same 16.16 fixed-point linear-interpolation
        // shape WavPlayer uses, instantiated per channel. Position/step
        // are in source SAMPLES (not bytes; MOD sample data is 8-bit
        // mono, so they're the same thing here).
        uint32_t samplePos16 = 0;   // 16.16 fractional position between lastSrcSample/nextSrcSample
        uint32_t sampleStep16 = 0;
        uint32_t srcIndex = 0;      // whole-sample index of lastSrcSample; nextSrcSample is always srcIndex+1
        int16_t lastSrcSample = 0, nextSrcSample = 0;
        bool havePair = false;
        bool looping = false; // whether the currently-playing instrument loops

        // Small read-ahead buffer for this channel's raw sample bytes --
        // all channels share one FsFile handle (re-seeking per channel
        // read would be far too slow at up to MAX_MOD_CHANNELS channels),
        // so each channel instead caches a chunk and only re-seeks+re-
        // reads when playback advances past it. Sized for the worst
        // case, not the typical one: a high note (period near the
        // table's low end, ~113) plays close to the 44.1kHz output rate
        // itself, not "well below" it, so a small chunk drains almost as
        // fast as it's produced -- with several such channels active at
        // once this meant a full reseek+read every ~1-2 output samples
        // combined across channels, more than core 0 could sustain
        // (audible dropouts under heavy polyphony). 256 bytes keeps even
        // that case comfortably infrequent.
        static const int CHUNK_SIZE = 256;
        int8_t chunkBuf[CHUNK_SIZE];
        uint32_t chunkStart = 0; // sample index (from this instrument's start) the buffer begins at
        uint32_t chunkLen = 0;   // valid bytes in chunkBuf

        // Panning: 0 (hard left) .. 255 (hard right), 128 == center.
        // Defaulted per channel index (classic hard L-R-R-L) at load
        // time; overridden by 8xx/E8x if the pattern uses them.
        uint8_t pan = 128;

        // Per-effect "last used" memory -- ProTracker's convention of a
        // 00 param meaning "reuse whatever this effect last used",
        // tracked per effect slot since they're independent of each other.
        uint8_t portaUpSpeed = 0, portaDownSpeed = 0;
        uint16_t portaTarget = 0;
        uint8_t tonePortaSpeed = 0;
        uint8_t vibratoSpeed = 0, vibratoDepth = 0;
        uint8_t vibratoPos = 0; // 0..63, one full waveform cycle
        uint8_t tremoloSpeed = 0, tremoloDepth = 0;
        uint8_t tremoloPos = 0;
        uint8_t volSlideParam = 0;
        uint8_t fineVolSlideUpParam = 0, fineVolSlideDownParam = 0;
        uint8_t sampleOffsetParam = 0;
        uint8_t retrigParam = 0; // E9y: retrigger the note every this-many ticks, 0 == none pending; reset each row
        uint8_t arpeggioParam = 0; // reset each row
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0;  // EDy: tick within this row to trigger on, 0 == none pending; reset each row
        uint16_t noteDelayPeriod = 0; // period to apply when the delayed trigger fires (0 == no new note, just re-arm)
        uint8_t noteCutParam = 0xFF; // ECy: tick to cut the note on, 0xFF == none pending; reset each row
        bool glissando = false;     // E3y: tone portamento snaps to semitones instead of smooth
    };

    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};

    ModSample _samples[MAX_SAMPLES];
    uint8_t _orderTable[128] = {0};
    int _songLength = 0;
    int _restartPos = 0;
    int _numPatterns = 0;
    int _numChannels = 4;
    int _numInstruments = 0; // count of samples with nonzero length, for the UI only
    uint32_t _patternDataStart = 0;
    // Headroom shift for mixOneSample() -- see S3mPlayer::_mixShift's
    // identical comment (smallest N with (1<<N) >= _numChannels, computed
    // once in load()) -- replaces a runtime `/ _numChannels` division
    // (the RP2040's Cortex-M0+ has no hardware integer divide) with a
    // shift. MOD's usual 4 channels made this less urgent than S3M/XM's
    // higher channel counts, but real-hardware diagnostics found the
    // shared audio pipeline running close enough to its throughput
    // ceiling under load that every avoidable per-sample cost matters.
    uint8_t _mixShift = 2; // log2(4), matches the _numChannels=4 default above until load() recomputes it

    ModChannel _channels[MAX_MOD_CHANNELS];

    // Sequencer position.
    int _orderPos = 0;
    int _row = 0;
    int _tick = 0;
    uint8_t _speed = 6;
    uint16_t _tempo = 125;
    uint32_t _samplesUntilNextTick = 0; // countdown in OUTPUT samples, see advanceTick()
    uint8_t _patternDelayRepeatsLeft = 0; // EEy: extra full tick-cycles to hold the current row for, see advanceTick()

    bool _patternBreakPending = false;
    int _patternBreakRow = 0;
    bool _positionJumpPending = false;
    int _positionJumpTarget = 0;

    uint32_t _elapsedMsFrozen = 0;
    uint32_t _lastResumeMicros = 0;

    static uint32_t ticksToSamples(uint16_t tempo);
    static uint16_t periodForNote(int noteIndex); // finetune-0 base table lookup, noteIndex 0..35
    static int nearestNoteIndexForPeriod(uint16_t period);
    static uint16_t applyFinetune(uint16_t basePeriod, int8_t finetune);

    void triggerNote(int ch); // resets a channel's resample position/step from its current period+finetune
    void recomputeStep(ModChannel& c);

    void advanceTick();
    void advanceRow();
    void processCellTriggers(int ch, const uint8_t cell[4]);
    void processExtendedTickZero(int ch, uint8_t param);
    void processTickEffects(); // ticks 1..speed-1: vibrato/tremolo/portamento/volume-slide/retrigger/note-cut/note-delay
    void mixOneSample(int16_t& outL, int16_t& outR);
    int16_t readChannelSample(ModChannel& c); // one resampled, volume-scaled sample from a channel's current position, advances it
    int16_t readRawSample(ModChannel& c, const ModSample& s, uint32_t index); // via c's chunk cache, refilling from _file on a miss
};

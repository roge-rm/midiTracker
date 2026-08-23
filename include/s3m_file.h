#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Streams a Scream Tracker 3 .s3m file directly off the SD card and
// mixes it down to a stereo stream fed into Synth's WAV playback ring
// buffer -- same architecture as ModPlayer (see mod_file.h's header
// comment for the shared reasoning: neither pattern nor sample data is
// ever loaded into RAM in full, both read on demand from the file).
// A separate class from ModPlayer, not a subclass/shared base -- S3M's
// pattern format, pitch model, and per-row data layout differ enough
// that forcing an abstraction now would mean guessing at what XM needs
// too, before it's built.
//
// Three real differences from MOD this class has to handle that MOD
// didn't:
//  - Patterns are a compressed, variable-length stream (a per-channel
//    flag byte says which of note/instrument/volume/effect follow, 0x00
//    ends the row) -- reaching row N means decoding every row before it,
//    not simple arithmetic. A small per-pattern row-offset cache (see
//    _rowOffsets below) avoids rescanning from the top on every loop.
//  - Pitch is sample-rate-based (each instrument has a C2SPD -- the
//    playback rate at a fixed reference note, by convention octave 4
//    note C) rather than MOD's Amiga period table.
//  - Samples can be 8 or 16-bit, signed or unsigned (MOD is always 8-bit
//    signed) -- the conversion logic is the same WavPlayer already has
//    for WAV's own 8/16-bit PCM, just applied per channel.
//
// Effects: the full A-Z command set plus S-prefixed "special" sub-
// commands, S3M's equivalent of MOD's full 0-F/E0-EF support. Z (MIDI
// macro) is out of scope -- no MIDI output target exists for tracker
// playback here, same reasoning ModPlayer skips MOD's EFy.
//
// No seek/scrub, same reasoning as ModPlayer (tempo itself changes
// during a song, so "position" isn't a time offset to jump to).
class S3mPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    // The S3M spec allows up to 32, but real-world files don't get
    // anywhere near that -- a survey of every .s3m file on hand (8 files,
    // several different artists/labels) topped out at 16 simultaneous
    // channels. Capped here at 20 (a safety margin above the observed
    // max, not a razor-thin fit to it) specifically to free RAM for a
    // much larger per-channel CHUNK_SIZE (see S3mChannel), which real-
    // hardware measurement (not just the earlier Python simulation, which
    // has no real SD latency to model) confirmed meaningfully reduces SD
    // access cost under many-channel polyphony. A file that genuinely
    // uses channels 20-31 degrades gracefully: decodeRow()'s apply gate
    // (`ch < MAX_CHANNELS`) silently ignores data for out-of-range
    // channels rather than crashing.
    static const int MAX_CHANNELS = 20;
    static const int MAX_INSTRUMENTS = 99;
    static const int MAX_PATTERNS = 100;
    static const int MAX_ORDERS = 256;

    bool load(const char* path);
    void close();
    void update(); // see ModPlayer::update()'s comment -- same "always keep the ring buffer topped up" reasoning
    void play();
    void pause();
    void stop();

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }
    uint32_t elapsedMs() const;

    int patternOrderPosition() const { return _orderPos; }
    int songLength() const { return _orderCount; }
    int currentRow() const { return _row < 0 ? 0 : _row; } // briefly -1 immediately after load(), see ModPlayer's identical note
    int channelCount() const { return _numChannels; }
    int instrumentCount() const { return _numInstruments; }

private:
    struct S3mSample {
        uint32_t fileOffset = 0;
        uint32_t length = 0;    // in samples (frames), not bytes
        uint32_t loopStart = 0; // in samples
        uint32_t loopEnd = 0;   // in samples; loopEnd <= loopStart means "no loop"
        uint32_t c2spd = 8363;  // playback rate (Hz) at the reference note (octave 4, note C)
        uint8_t volume = 64;
        bool is16Bit = false;
        bool isSigned = false; // from the file's FileFormatVersion (old=signed, new=unsigned is the common case)
        bool isStereo = false; // rare; treated as mono (left channel only) if set, see load()
    };

    struct S3mChannel {
        uint8_t sample = 0; // 1..MAX_INSTRUMENTS, 0 == none assigned yet
        uint8_t volume = 64; // "true" volume -- what instrument defaults/volume column/slides set
        // What readChannelSample() actually scales by -- synced from
        // `volume` at the top of each tick, then tremolo/tremor may
        // adjust it further for that tick's mixing only. Kept separate
        // so transient effects never leak into the persistent `volume`
        // slides/etc operate on (an earlier draft of this class got that
        // wrong -- tremolo drifting the "real" volume tick over tick
        // instead of oscillating around it).
        uint8_t mixVolume = 64;
        uint16_t c2spd = 8363; // reference playback rate (Hz) from the assigned instrument

        // Pitch is tracked as an Amiga-style pseudo-period, not
        // C2SPD/semitone math directly -- converted once per note
        // trigger (see S3mPlayer::s3mNoteToPeriod()) and otherwise
        // handled with the exact same period-arithmetic
        // portamento/vibrato/arpeggio/tremolo already use in ModPlayer,
        // reused deliberately: that logic is already proven against real
        // files, and S3M's own effect parameters (slide speeds, vibrato
        // depth units, etc) were themselves designed around this same
        // period-granularity convention historically, so nothing is lost
        // by converting through it rather than inventing a parallel
        // semitone-based scheme.
        uint16_t period = 0; // 0 == silent

        uint32_t samplePos16 = 0;
        uint32_t sampleStep16 = 0;
        uint32_t srcIndex = 0;
        int16_t lastSrcSample = 0, nextSrcSample = 0;
        bool havePair = false;
        bool looping = false;

        uint8_t pan = 128; // 0 (hard left) .. 255 (hard right), 128 == center

        // Matches ModPlayer::ModChannel::CHUNK_SIZE -- see its comment on
        // why this must be sized generously, not for the typical case.
        // An earlier attempt at raising this (256/512/1024) on real
        // hardware showed no improvement and was reverted -- but that
        // test was confounded by a decode-desync bug elsewhere (a shared
        // FsFile handle between pattern and sample reads, since fixed by
        // giving pattern reads their own _patternFile) that was silently
        // suppressing most channels from ever actually playing
        // simultaneously, so real multichannel SD load was never
        // genuinely exercised. With that fixed and instrumented (see
        // S3mPlayer::update()'s diagnostics), 256->512 measured ~900->520
        // refills/sec while per-refill cost only grew ~260->300us --
        // i.e. the fixed per-seek overhead (~260us) still dominates over
        // the ~0.08us/byte transfer cost even at 512, so there's more
        // room to trade RAM for fewer, larger reads before hitting a
        // wall. 1024 is estimated to roughly halve chunkUs again.
        static const int CHUNK_SIZE = 1024;
        int8_t chunkBuf[CHUNK_SIZE];
        uint32_t chunkStart = 0;
        uint32_t chunkLen = 0;
        // Which instrument (1-based, matching S3mChannel::sample) chunkBuf
        // currently holds raw bytes for -- 0 means "none". A retrigger of
        // the SAME sample (the common case for percussive/rhythmic
        // content, which retriggers a short sample very frequently) does
        // NOT invalidate this cache -- restartSamplePosition() resets
        // playback position, not what's cached, since a cached read of
        // the same sample's same byte range is exactly as valid after a
        // retrigger as before it. readRawSample() checks this field
        // before trusting chunkStart/chunkLen, so a genuine sample change
        // still invalidates correctly. This is the same real-hardware-
        // motivated fix XmPlayer's identical chunkSampleIndex got first
        // (see its comment) -- every note retrigger was forcing a fresh
        // synchronous SD seek+read even when the exact same short sample
        // had just been read moments earlier, a real, measured
        // contributor to "The Reflex.s3m"'s busy-passage underruns.
        uint8_t chunkSample = 0;

        // Per-effect "last used" memory, same convention/reasoning as
        // ModPlayer::ModChannel's equivalent fields -- these hold the
        // remembered PARAMETER VALUE (for when a row reuses an effect
        // with param 00), and are never auto-reset.
        uint8_t portaUpSpeed = 0, portaDownSpeed = 0;
        uint16_t portaTarget = 0; // tone portamento target period
        uint8_t tonePortaSpeed = 0;
        uint8_t vibratoSpeed = 0, vibratoDepth = 0, vibratoPos = 0;
        uint8_t tremoloSpeed = 0, tremoloDepth = 0, tremoloPos = 0;
        uint8_t panbrelloSpeed = 0, panbrelloDepth = 0, panbrelloPos = 0;
        uint8_t volSlideParam = 0;
        uint8_t retrigParam = 0; // Qxy low nibble
        uint8_t retrigVolSlide = 0; // Qxy high nibble
        uint8_t arpeggioParam = 0; // reset each row
        uint8_t tremorOnParam = 0, tremorOffParam = 0, tremorCounter = 0; // Ixy
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0; // reset each row
        uint8_t pendingNote = 0xFF, pendingSample = 0; // for a delayed trigger (S3M's SDy)
        uint8_t noteCutParam = 0xFF; // reset each row
        bool glissando = false;

        // Whether each per-tick continuing effect is actually running
        // THIS row -- separate from the memory fields above, which only
        // hold a remembered value and must never gate whether the effect
        // fires. Real tracker semantics (documented for ProTracker's Axy
        // and inherited by S3M's equivalents) are "this effect must be
        // given each row you wish it to continue" -- the memory is only
        // for filling in an omitted param (00), not for making the
        // effect outlive the row it stops being specified on. Reset to
        // false at the start of every row (see advanceRow()), set true
        // in applyCell() only when that row's command actually matches.
        bool volSlideActiveRow = false;
        bool portaDownActiveRow = false, portaUpActiveRow = false;
        bool tonePortaActiveRow = false;
        bool vibratoActiveRow = false;
        bool tremoloActiveRow = false;
        bool retrigActiveRow = false;
    };

    FsFile _file;        // header/order-table/parapointer reads at load(), plus sample data (readRawSample()) during playback
    // A second, independent handle to the same file, used exclusively for
    // pattern-data reads (decodeRow()/advanceToRow()) during playback.
    // decodeRow() reads a row as several separate small _file.read() calls
    // relying on the file position continuing sequentially between them
    // (unlike ModPlayer's pattern reads, which are a single self-contained
    // seek+read computed by arithmetic every time) -- interleaving that
    // multi-step sequential read with readRawSample()'s frequent seeks to
    // far-away sample data on the SAME handle, many times per row (every
    // active channel's chunk refill lands in between), was a real
    // candidate for desyncing the pattern read. A separate handle removes
    // that interaction entirely regardless of the exact mechanism.
    FsFile _patternFile;
    State _state = STATE_IDLE;
    char _error[64] = {0};

    S3mSample _samples[MAX_INSTRUMENTS];
    uint32_t _patternOffsets[MAX_PATTERNS]; // absolute file offsets, computed once from the parapointer table
    uint8_t _orderTable[MAX_ORDERS] = {0};
    int _orderCount = 0;
    int _numInstruments = 0;
    int _numChannels = 0; // count of enabled channels -- UI display, and see _mixShift below
    bool _channelEnabled[MAX_CHANNELS] = {false}; // channels can be sparsely enabled, not just "first N"
    // Headroom shift for mixOneSample() -- unconditionally 0 now (see
    // MIX_SHIFT_TYPICAL_CHANNELS's comment in s3m_file.cpp for why a
    // channel-count-based shift was dropped entirely: even capping it
    // undershot how quiet it made typical, non-worst-case tracker content,
    // by a wide enough margin that real-hardware A/B listening against
    // WAV/MIDI could still hear it). softClampMix() in mixOneSample() is
    // the only headroom management left, applied unconditionally
    // regardless of this value.
    uint8_t _mixShift = 0;
    uint8_t _globalVolume = 64;
    uint8_t _globalVolSlideParam = 0; // W's own slide memory -- global, not shared with any channel's D/volSlideParam

    S3mChannel _channels[MAX_CHANNELS];

    // Sequencer position.
    int _orderPos = 0;
    int _row = -1; // see currentRow()'s comment
    int _tick = 0;
    uint8_t _speed = 6;
    uint16_t _tempo = 125;
    uint32_t _samplesUntilNextTick = 0;
    uint8_t _patternDelayRepeatsLeft = 0;

    bool _patternBreakPending = false;
    int _patternBreakRow = 0;
    bool _positionJumpPending = false;
    int _positionJumpTarget = 0;

    // Sequential-scan pattern reading state (see this class's header
    // comment) -- which pattern is currently "open" for scanning, a
    // cache of every row's byte offset within it discovered so far (only
    // entries up to the highest row visited are valid), and the read
    // cursor for continuing a forward scan.
    int _openPatternNum = -1;
    uint32_t _rowOffsets[64];
    int _rowOffsetsValid = 0; // how many entries of _rowOffsets are populated
    uint32_t _scanCursor = 0; // current read position, valid while scanning _openPatternNum forward

    uint32_t _elapsedMsFrozen = 0;
    uint32_t _lastResumeMicros = 0;

    static uint32_t ticksToSamples(uint16_t tempo);
    // Converts an S3M note byte ((octave<<4)|noteInOctave) plus an
    // instrument's C2SPD into an equivalent Amiga-style pseudo-period --
    // see S3mChannel::period's comment on why. Reference point is octave
    // 4, note C (the standard convention every S3M-compatible player
    // uses for C2SPD).
    static uint16_t s3mNoteToPeriod(uint8_t note, uint32_t c2spd);

    void recomputeStep(S3mChannel& c); // sets sampleStep16 from c.period, same formula as ModPlayer::recomputeStep()
    int16_t readRawSample(S3mChannel& c, const S3mSample& s, uint32_t index);
    void restartSamplePosition(int ch); // resets position/re-primes the interpolation pair, WITHOUT touching pitch -- see triggerNote()
    void triggerNote(int ch, uint8_t note); // sets pitch from `note`, then restartSamplePosition()
    int16_t readChannelSample(S3mChannel& c);
    void mixOneSample(int16_t& outL, int16_t& outR);

    // Seeks to the start of pattern `num` for scanning and resets the
    // row-offset cache -- called whenever the sequencer moves to a
    // different pattern (see advanceRow()).
    void openPatternForScan(int num);
    // Ensures row `targetRow` of the currently-open pattern has been
    // reached and applied (triggering its notes/effects) -- an O(1)
    // direct seek if already cached from an earlier visit to this
    // pattern, otherwise scans forward from wherever the cache currently
    // ends, decoding (and discarding, via decodeRow(false)) every row in
    // between along the way, caching each one's start offset as it goes.
    void advanceToRow(int targetRow);
    // Reads one row's packed per-channel entries from the file at the
    // current position, advancing past the row's 0x00 terminator either
    // way. `apply` selects whether decoded entries actually trigger
    // anything (see advanceToRow()'s comment) or are just parsed to find
    // the row's byte length.
    void decodeRow(bool apply);
    // Dispatches one decoded channel entry -- note/instrument trigger,
    // volume column, and effect command -- the S3M analog of
    // ModPlayer::processCellTriggers().
    void applyCell(int ch, uint8_t note, uint8_t sample, bool hasNote,
                    uint8_t volume, bool hasVolume, uint8_t command, uint8_t param, bool hasCommand);

    void advanceTick();
    void advanceRow();
    void processExtendedTickZero(int ch, uint8_t command, uint8_t param); // S-prefixed special commands
    void processTickEffects();
};

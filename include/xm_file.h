#pragma once
#include "tracker_base.h"

// FastTracker 2 .xm player on the shared tracker base (see tracker_base.h).
//  - Linear or Amiga frequency table, chosen per file (_linearFreqTable).
//    Slides work on XmChannel::period either way; only the note/period
//    conversion differs.
//  - Instrument volume and panning envelopes with sustain, loop and fadeout
//    (processEnvelopes()).
//  - Instruments map 96 notes into a flat, file-wide sample table.
//  - Sample data is delta-encoded; refillChunk() decodes it into the cache.
//
// Effects: the pattern effect column 0-33 as FT2 numbers it, plus the
// volume column. Instrument auto-vibrato isn't played. Pingpong loops play
// as forward loops.
class XmPlayer : public TrackerPlayer {
public:
    // Real-world channel counts (surveyed across 110 .xm files on hand):
    // min 4, median 14, p95 ~32, one outlier at 40. Capped at 32 -- p95
    // plus a margin, not a razor-thin fit -- with the single 40-channel
    // outlier degrading gracefully the same way S3M's own above-MAX_CHANNELS
    // case does (decodeRow()'s apply gate silently ignores out-of-range
    // channels).
    static const int MAX_CHANNELS = 32;
    // Instrument-count survey: median 32, p95 == max == 128 -- 128 is the
    // format's own hard cap, and real files do use all of it, so this is
    // sized to the format ceiling rather than a statistical margin.
    static const int MAX_INSTRUMENTS = 128;
    static const int MAX_PATTERNS = 256;
    // Flat, file-wide sample pool (not nested per-instrument -- an
    // instrument's 96-entry keymap can reuse a small handful of samples,
    // and some real files legitimately put 16+ samples in one
    // drum-kit-style instrument, so bounding by
    // MAX_INSTRUMENTS*MAX_SAMPLES_PER_INSTRUMENT would blow up
    // combinatorially for no real-content reason). Sized against surveyed
    // total-samples-per-file (median 27, p95 79) with ~2x margin.
    static const int MAX_SAMPLES = 160;
    static const int MAX_ORDERS = 256;
    // XM patterns can have up to 256 rows (vs S3M's fixed 64) -- sizes
    // the per-pattern row-offset scan cache (see _rowOffsets).
    static const int MAX_ROWS = 256;
    static const int MAX_ENV_POINTS = 12;

    bool load(const char* path) override;

private:
    // A piecewise-linear envelope: up to 12 (tick, value) points, with
    // optional sustain (holds at a point while a note is still held) and
    // loop (wraps between two points once playback passes the loop end).
    // Value range is 0-64 for volume, roughly 0-64 centered at 32 for
    // panning -- same units either way, just interpreted differently by
    // the caller. See processEnvelopes().
    struct XmEnvelope {
        uint16_t pointX[MAX_ENV_POINTS] = {0};
        uint16_t pointY[MAX_ENV_POINTS] = {0};
        uint8_t numPoints = 0;
        uint8_t sustainPoint = 0;
        uint8_t loopStartPoint = 0;
        uint8_t loopEndPoint = 0;
        // bit0 = enabled, bit1 = sustain enabled, bit2 = loop enabled --
        // matches the file's own flag byte layout directly, no remapping.
        uint8_t flags = 0;
        bool enabled() const { return (flags & 0x01) != 0; }
        bool sustainOn() const { return (flags & 0x02) != 0; }
        bool loopOn() const { return (flags & 0x04) != 0; }
    };

    // One instrument's metadata -- envelopes and the note->sample keymap
    // live here (once per instrument), not per-channel; only envelope
    // *playback position* is per-channel (see XmChannel).
    struct XmInstrument {
        uint8_t keymap[96] = {0}; // 0-based, added to firstSampleIndex at trigger time
        XmEnvelope volEnv;
        XmEnvelope panEnv;
        uint16_t volFadeout = 0; // subtracted from XmChannel::fadeoutVolume per tick, once keyOff is set
        uint8_t firstSampleIndex = 0; // into the flat _samples[] table
        uint8_t numSamplesUsed = 0;
    };

    struct XmSample : TrackerSample {
        uint32_t loopLength = 0; // in frames; 0 == no loop
        // Delta-decode accumulator at loopStart, worked out at load() so a
        // loop wrap can resume decoding without rescanning from frame 0.
        int32_t accumAtLoopStart = 0;
        uint8_t volume = 64;    // this sample's own default volume (0-64), applied on trigger
        int8_t finetune = 0;    // signed, raw byte units (both pitch formulas divide it themselves)
        uint8_t panning = 128;
        int8_t relativeNoteNumber = 0;
        bool is16Bit = false;
        // Pingpong loops (loopType == 2) are treated as forward loops --
        // a documented simplification, same spirit as S3M's panbrello/Qxy
        // approximations: pingpong is rare, and MOD/S3M support neither.
        bool looping = false;
    };

    struct XmChannel : TrackerVoice {
        uint8_t instrument = 0;  // 1-based into _instruments[], 0 == none
        uint8_t sampleIndex = 0; // into _samples[], from instrument+keymap at trigger time
        uint8_t volume = 64;     // what defaults, the volume column and slides set
        uint16_t period = 0;     // FT2 period, 0 == silent, see xmNoteToPeriod()
        uint16_t stepPeriod = 0; // period sampleStep16 was worked out from (base plus vibrato/arpeggio)
        uint8_t pan = 128;       // 0 (left) .. 255 (right); mixPan adds the panning envelope

        // Envelope playback position (elapsed ticks since trigger),
        // independent per envelope -- the envelope *shape* itself lives
        // once per instrument (XmInstrument::volEnv/panEnv), not here.
        uint16_t volEnvTick = 0, panEnvTick = 0;
        bool keyOff = false; // set by the Key Off note (97) or the Kxx effect
        // Full-scale at trigger, decremented by the instrument's
        // volFadeout rate every tick once keyOff is set, floors at 0
        // (silence). FT2's scale, 32768 == full.
        uint32_t fadeoutVolume = 32768;
        uint8_t volEnvValue = 64; // volume envelope value this tick, 0-64

        // Per-effect "last used" memory -- same convention as
        // S3mChannel's equivalent fields (remembered PARAMETER VALUE for
        // when a row reuses an effect with param 00; never auto-reset).
        uint8_t portaUpSpeed = 0, portaDownSpeed = 0;
        uint16_t portaTarget = 0; // tone portamento target period
        uint8_t tonePortaSpeed = 0;
        uint8_t vibratoSpeed = 0, vibratoDepth = 0, vibratoPos = 0;
        uint8_t tremoloSpeed = 0, tremoloDepth = 0, tremoloPos = 0;
        uint8_t volSlideParam = 0;
        uint8_t panSlideParam = 0;
        uint8_t retrigParam = 0;         // Rxy low nibble -- ticks between retriggers
        uint8_t retrigVolChangeType = 0; // Rxy high nibble -- see processTickEffects()'s comment
        uint8_t arpeggioParam = 0;       // reset each row
        uint8_t tremorOnParam = 0, tremorOffParam = 0, tremorCounter = 0; // Txy
        bool tremorMute = false; // in tremor's off phase, held after T ends until the volume is set, as FT2
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0;      // reset each row
        uint8_t pendingNote = 0, pendingInstrument = 0, pendingVolume = 0; // the cell held by EDy, 0 == none
        uint8_t noteCutParam = 0xFF;     // reset each row
        uint8_t keyOffTickParam = 0xFF;  // reset each row -- Kxx's "fire at tick xx", same shape as note-delay
        bool glissando = false;

        // Whether each per-tick continuing effect is actually running
        // THIS row -- see S3mChannel's identical fields for the full
        // rationale (real tracker semantics: "must be given each row you
        // wish it to continue", not "outlives the row it stops being
        // specified on"). Reset to false at the top of every row (see
        // advanceRow()), set true in applyCell() only when that row's
        // command actually matches.
        bool volSlideActiveRow = false;
        bool portaDownActiveRow = false, portaUpActiveRow = false;
        bool tonePortaActiveRow = false;
        bool vibratoActiveRow = false;
        bool tremoloActiveRow = false;
        bool retrigActiveRow = false;
        // XM-only continuing effect (Pxy / volume-column D0-E0) -- S3M
        // has no per-tick continuing panning effect, only one-shot Xxx.
        bool panSlideActiveRow = false;
        bool tremorActiveRow = false;
    };

    // Selected once from the header's flags bit 0 at load(), never
    // rechecked per-tick -- see xmNoteToPeriod()/periodToFreq().
    bool _linearFreqTable = true;

    XmInstrument _instruments[MAX_INSTRUMENTS];
    XmSample _samples[MAX_SAMPLES];
    int _numSamplesTotal = 0; // how many of _samples[] are populated -- also the next-free allocator cursor during load()

    uint32_t _patternOffsets[MAX_PATTERNS]; // absolute file offset of each pattern's cell data (computed once via a sequential walk, see load())
    uint16_t _patternRowCounts[MAX_PATTERNS];
    // True for a pattern whose file-declared packedSize is 0 (every row
    // implicitly blank, nothing stored) -- also the default for any
    // pattern index never actually scanned (out-of-range order-table
    // entry, or beyond MAX_PATTERNS), so decodeRow() degrades to a safe
    // no-op instead of misreading unrelated file bytes as cell data.
    bool _patternEmpty[MAX_PATTERNS];
    bool _openPatternEmpty = false;
    uint8_t _orderTable[MAX_ORDERS] = {0};
    // From the header, unclamped -- decodeRow() must read exactly this
    // many cells per row regardless of MAX_CHANNELS, or the byte stream
    // desyncs for every row of a file with more channels than we mix.
    int _fileNumChannels = 0;
    uint8_t _globalVolSlideParam = 0; // H's own memory
    bool _globalVolSlideActiveRow = false;

    // Decoded frames per voice cache. A refill reads up to 2 bytes per
    // frame, so this must stay at or under RAW_SCRATCH_BYTES / 2.
    static const int CHUNK_FRAMES = 512;

    XmChannel _channels[MAX_CHANNELS];
    int16_t _chunks[MAX_CHANNELS][CHUNK_FRAMES];

    // Pattern scan state, same as S3mPlayer's but for up to 256 rows.
    int _openPatternNum = -1;
    uint32_t _rowOffsets[MAX_ROWS];
    int _rowOffsetsValid = 0;
    uint32_t _scanCursor = 0;
    int _openPatternRowCount = 64;

    // Converts an absolute note number (pattern note + sample's
    // relativeNoteNumber) plus finetune into XmChannel::period, using
    // whichever of the two formulas _linearFreqTable selects -- both
    // documented in xm_file.cpp. Not static (depends on _linearFreqTable).
    uint16_t xmNoteToPeriod(int note, int8_t finetune) const;
    float periodToFreq(uint16_t period) const; // inverse of xmNoteToPeriod(), same mode selection

    void recomputeStep(XmChannel& c) { recomputeStep(c, c.period); }
    void recomputeStep(XmChannel& c, uint16_t period); // sampleStep16 for `period`, leaves c.period alone
    // Decodes `frameCount` delta-encoded frames starting at `fileOffset`
    // (using `startAccum` as the carry-in accumulator), discarding the
    // decoded values and returning only the final accumulator. Used at
    // load() for accumAtLoopStart and by refillChunk() for a far seek.
    // Reads CHUNK_FRAMES at a time through _rawScratch.
    int32_t decodeDeltaRun(uint32_t fileOffset, uint32_t frameCount, bool is16Bit, int32_t startAccum);
    void restartSamplePosition(int ch); // resets position/re-primes the interpolation pair, WITHOUT touching pitch -- see triggerNote()
    void resolveSampleIndex(XmChannel& c, int note); // instrument+keymap -> sampleIndex, called from triggerNote()
    void triggerNote(int ch, int note); // sets pitch/sample from `note`, resets envelopes/fadeout, then restartSamplePosition()

    // Seeks to the start of pattern `num` for scanning and resets the
    // row-offset cache -- called whenever the sequencer moves to a
    // different pattern (see advanceRow()).
    void openPatternForScan(int num);
    // Ensures row `targetRow` of the currently-open pattern has been
    // reached and applied -- same O(1)-if-cached / scan-forward-otherwise
    // shape as S3mPlayer::advanceToRow().
    void advanceToRow(int targetRow);
    // Reads one row's packed per-channel cells from the file at the
    // current position. Unlike S3M, there's no per-row terminator byte --
    // exactly _numChannels cells are read, back to back. `apply` selects
    // whether decoded cells actually trigger anything or are just parsed
    // to find the row's byte length (see advanceToRow()'s comment).
    void decodeRow(bool apply);
    // Dispatches one decoded channel cell -- note/instrument trigger,
    // volume column, and effect command -- the XM analog of
    // S3mPlayer::applyCell().
    // Instrument, note and volume column of one cell, on its row or on the
    // EDy delay tick. note is the raw pattern byte (0 == none, 97 == key off),
    // instrument and volume 0 == none.
    void applyNoteColumns(int ch, uint8_t note, uint8_t instrument, uint8_t volume, bool isTonePorta);
    void applyVolumeColumn(XmChannel& c, uint8_t volume);
    void keyOff(XmChannel& c);
    void applyCell(int ch, uint8_t note, uint8_t instrument, bool hasNote, bool hasInstrument,
                    uint8_t volume, bool hasVolume, uint8_t effect, uint8_t param, bool hasEffect);

    void advanceRow() override;
    void processExtendedCommand(int ch, uint8_t param); // Exy sub-commands, FT2's equivalent of S3M's S-prefixed specials
    void processTickEffects() override;
    void tickStart() override { processEnvelopes(); } // envelopes run on every tick, row ticks included
    void processEnvelopes();
    // mixVolume from volume, the envelope value and the fadeout, for a
    // volume change after this tick's processEnvelopes().
    void updateMixVolume(XmChannel& c) {
        c.mixVolume = c.tremorMute ? 0 : (uint8_t)(((uint32_t)c.volume * c.volEnvValue / 64) * c.fadeoutVolume / 32768);
    }
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;
};

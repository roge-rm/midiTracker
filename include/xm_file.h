#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Temporary real-hardware diagnostic counter (see xm_file.cpp's own
// g_xmDiag* block) -- declared here too so readRawSample()'s inlined
// cache-hit fast path (below) can still count hits without giving up the
// inlining that fast path exists for. Removed along with the rest of the
// diagnostics once no longer needed.
extern uint32_t g_xmDiagChunkHits;

// Streams a FastTracker 2 .xm file directly off the SD card and mixes it
// down to a stereo stream fed into Synth's WAV playback ring buffer --
// same streaming-from-SD architecture as ModPlayer/S3mPlayer (neither
// pattern nor sample data is ever loaded into RAM in full). A separate
// class, not a subclass/shared base of either -- see mod_file.h's header
// comment for why forcing a shared abstraction across formats has
// consistently meant guessing at what the next format needs before it's
// built.
//
// Everything S3mPlayer's own real-hardware debugging saga surfaced is
// designed in here from the start rather than left to be rediscovered:
//  - A dedicated second FsFile handle for pattern-data reads (see
//    _patternFile below) -- S3M's worst bug was pattern reads and sample
//    reads sharing one handle and desyncing under interleaved access.
//  - Continuing per-tick effects (volume slide, portamento, vibrato,
//    tremolo, retrigger, panning slide) are gated by an explicit
//    "specified THIS row" flag, not just a nonzero remembered parameter
//    -- see XmChannel's *ActiveRow fields.
//  - float/powf, not double/pow; shifts, not divides, on the hot
//    pitch/mixing paths -- the RP2040 has neither hardware divide nor an
//    FPU.
//  - The hottest per-sample functions are placed in RAM via
//    __not_in_flash_func() (see xm_file.cpp).
//
// New relative to MOD/S3M, genuinely required by this format:
//  - Two frequency-table modes, selected per file from the header (see
//    _linearFreqTable) -- portamento/vibrato/arpeggio all operate on a
//    shared XmChannel::period abstraction regardless of mode (same
//    convention S3M/MOD already use), only the note<->period conversion
//    differs.
//  - Per-instrument volume/panning envelopes with sustain/loop/fadeout
//    (see XmEnvelope, XmChannel's volEnvTick/panEnvTick/keyOff/
//    fadeoutVolume, and processEnvelopes()).
//  - Instruments hold a 96-note keymap into a flat, file-wide sample
//    table (XM separates "instrument" from "sample", unlike MOD/S3M).
//  - Sample PCM is delta-encoded, not plain PCM -- see XmChannel::
//    chunkAccum and readRawSample()'s comment.
//
// Full effect set (pattern effect column 0-33 as FT2 numbers it, plus
// the volume column's own mini-language) -- matches the completeness
// already given to MOD/S3M rather than a reduced subset. Instrument
// auto-vibrato (the per-instrument vibType/vibSweep/vibDepth/vibRate
// fields) is intentionally out of scope, same treatment S3M already
// gives its own out-of-scope Z (MIDI macro) effect -- rare in practice,
// and orthogonal to the pattern-effect vibrato this class does support.
//
// No seek/scrub, same reasoning as ModPlayer/S3mPlayer.
class XmPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

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

    struct XmSample {
        uint32_t fileOffset = 0; // absolute file offset of this sample's PCM data
        uint32_t length = 0;     // in frames (converted from the header's byte-length field at load time)
        uint32_t loopLength = 0; // in frames; 0 == no loop
        uint32_t loopStart = 0;  // in frames
        // Delta-decode accumulator value valid exactly at index loopStart
        // -- precomputed once at load() by decoding-and-discarding from
        // index 0, so a loop-wrap chunk refill has a free, exact anchor
        // to resume decoding from instead of needing to rescan from the
        // sample's start every time. See readRawSample()'s comment.
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

    struct XmChannel {
        uint8_t instrument = 0;   // 1-based index into _instruments[], 0 == none assigned
        uint8_t sampleIndex = 0;  // resolved via instrument+keymap at trigger time, into _samples[]
        uint8_t volume = 64;      // "true" volume -- instrument default/volume column/slides
        // Synced from `volume` (scaled by the volume envelope and
        // fadeout) at the top of each tick; see S3mChannel::mixVolume's
        // identical comment on why this must stay separate from the
        // persistent `volume` slides operate on.
        uint8_t mixVolume = 64;
        uint16_t period = 0; // 0 == silent; see XmPlayer::xmNoteToPeriod()'s comment on the shared period abstraction

        uint32_t samplePos16 = 0;
        uint32_t sampleStep16 = 0;
        uint32_t srcIndex = 0;
        int16_t lastSrcSample = 0, nextSrcSample = 0;
        bool havePair = false;
        bool looping = false;

        uint8_t pan = 128;    // 0 (hard left) .. 255 (hard right)
        uint8_t mixPan = 128; // pan blended with the panning envelope for this tick's mix only

        // Pre-decoded samples, not raw file bytes (unlike S3M/MOD's
        // chunkBuf) -- XM's delta encoding must be decoded sequentially
        // regardless, so there's no benefit to caching undecoded bytes.
        // See readRawSample()'s comment for the full refill algorithm.
        //
        // Real-hardware diagnostics found refill cost averages ~800us
        // whether the read is a plain sequential continuation or a real
        // seek (seqAvgNs ~= seekAvgNs, measured directly) -- meaning cost
        // is driven by NUMBER of SD calls, not seek distance, so a bigger
        // window genuinely reduces total SD time by needing fewer calls.
        // A same-session jump straight to 1024 (matching S3M's own tuned
        // value) caused a real-hardware crash with corrupted/screeching
        // audio, root-caused NOT to a decode bug (the refill algorithm
        // re-validated clean against ground truth at 1024) or heap
        // exhaustion (measured freeHeap comfortably covered it) but most
        // likely a worst-case single-update()-call latency spike -- the
        // ring buffer's free space was already sitting at 0 essentially
        // continuously even at 384 (near-zero real-time margin), and a
        // bigger CHUNK_SIZE raises the cost of a burst of several
        // channels needing a refill in the same call. Since then,
        // XmPlayer::update()'s own per-call output-frame cap was lowered
        // (512->128, see its own comment) specifically to bound that
        // burst cost independent of CHUNK_SIZE -- with that safety net in
        // place, this is a smaller, incremental step up from 384 (not
        // straight back to 1024), re-validated against ground truth
        // again at this exact value before being applied.
        static const int CHUNK_SIZE = 512;
        int16_t chunkBuf[CHUNK_SIZE];
        uint32_t chunkStart = 0;
        uint32_t chunkLen = 0;
        // Running delta-decode accumulator, valid exactly at index
        // chunkStart+chunkLen (i.e. "resume decoding from here for the
        // next raw delta byte"). No MOD/S3M analog -- their raw PCM needs
        // no running decode state at all.
        int32_t chunkAccum = 0;
        // Which sample chunkBuf/chunkStart/chunkLen/chunkAccum currently
        // hold decoded data for -- 0xFF ("none") means the cache is empty.
        // A retrigger of the SAME sample (the common case for percussive/
        // rhythmic content, which retriggers a short sample very
        // frequently) does NOT invalidate this cache -- restartSamplePosition()
        // resets playback position, not what's cached, since a cached
        // decode of the same sample's same byte range is exactly as valid
        // after a retrigger as before it. readRawSample() checks this
        // field before trusting chunkStart/chunkLen/chunkAccum, so a
        // genuine sample change (a different instrument/keymap slot)
        // still invalidates correctly. See readRawSample()'s comment --
        // this was a real, measured fix: without it, every single note
        // retrigger forced a fresh synchronous SD seek+read even when the
        // exact same short sample had just been read moments earlier,
        // which showed up as audible pops right on rhythmic/percussive
        // hits (the worst case: frequent retriggers of a short sample).
        uint8_t chunkSampleIndex = 0xFF;

        // Envelope playback position (elapsed ticks since trigger),
        // independent per envelope -- the envelope *shape* itself lives
        // once per instrument (XmInstrument::volEnv/panEnv), not here.
        uint16_t volEnvTick = 0, panEnvTick = 0;
        bool keyOff = false; // set by the Key Off note (97) or the Kxx effect
        // Full-scale at trigger, decremented by the instrument's
        // volFadeout rate every tick once keyOff is set, floors at 0
        // (silence). 16.16-style fixed point (65536 == full scale).
        uint32_t fadeoutVolume = 65536;

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
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        uint8_t noteDelayParam = 0;      // reset each row
        uint8_t pendingNote = 0xFF, pendingInstrument = 0; // for a delayed trigger (EDy)
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
    };

    FsFile _file;        // header/instrument/order-table reads at load(), plus sample data (readRawSample()) during playback
    // A second, independent handle to the same file, dedicated to
    // pattern-cell reads (decodeRow()/advanceToRow()) during playback --
    // see this class's header comment. XM's cell decode is a flag byte
    // plus up to 4 more conditional sequential reads, at least as much
    // interleaving risk with readRawSample()'s frequent far-away seeks as
    // S3M had, so this is built in from the first commit rather than
    // added after a corruption report.
    FsFile _patternFile;
    State _state = STATE_IDLE;
    char _error[64] = {0};

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
    int _orderCount = 0;
    int _numInstruments = 0;
    // From the header, unclamped -- decodeRow() must read exactly this
    // many cells per row regardless of MAX_CHANNELS, or the byte stream
    // desyncs for every row of a file with more channels than we mix.
    int _fileNumChannels = 0;
    int _numChannels = 0; // min(_fileNumChannels, MAX_CHANNELS) -- UI display and the mixing/array-index bound; unlike S3M, XM channels are always 0..N-1 contiguous, no sparse-enable table needed

    uint8_t _globalVolume = 64;
    uint8_t _globalVolSlideParam = 0; // H's own slide memory -- global, not shared with any channel's A/volSlideParam
    // Headroom shift for mixOneSample() -- unconditionally 0 now (see
    // MIX_SHIFT_TYPICAL_CHANNELS's comment in xm_file.cpp for why a
    // channel-count-based shift was dropped entirely: even capping it
    // undershot how quiet it made typical, non-worst-case tracker content,
    // by a wide enough margin that real-hardware A/B listening against
    // WAV/MIDI could still hear it). softClampMix() is the only headroom
    // management left, applied unconditionally regardless of this value.
    uint8_t _mixShift = 0;

    XmChannel _channels[MAX_CHANNELS];

    // Sequencer position.
    int _orderPos = 0;
    int _row = -1; // see currentRow()'s comment
    int _tick = 0;
    uint8_t _speed = 6;
    uint16_t _bpm = 125;
    uint32_t _samplesUntilNextTick = 0;
    uint8_t _patternDelayRepeatsLeft = 0;

    bool _patternBreakPending = false;
    int _patternBreakRow = 0;
    bool _positionJumpPending = false;
    int _positionJumpTarget = 0;

    // Sequential-scan pattern reading state -- same shape/reasoning as
    // S3mPlayer's identical fields, just sized for XM's up-to-256-row
    // patterns and built from a linear header walk instead of
    // parapointers (XM patterns aren't parapointer-addressable).
    int _openPatternNum = -1;
    uint32_t _rowOffsets[MAX_ROWS];
    int _rowOffsetsValid = 0;
    uint32_t _scanCursor = 0;
    int _openPatternRowCount = 64;

    uint32_t _elapsedMsFrozen = 0;
    uint32_t _lastResumeMicros = 0;

    // Shared scratch buffer for raw (still delta-encoded) bytes read off
    // SD during a chunk refill, decoded into a channel's own chunkBuf in
    // the same pass -- not per-channel, since only one channel's chunk is
    // ever being refilled at a time inside update()'s single call chain,
    // unlike chunkBuf itself (which must stay per-channel, holding live
    // interpolation state between refills).
    uint8_t _rawScratch[XmChannel::CHUNK_SIZE * 2]; // worst case 2 bytes/frame (16-bit samples)

    static uint32_t ticksToSamples(uint16_t bpm);

    // Converts an absolute note number (pattern note + sample's
    // relativeNoteNumber) plus finetune into XmChannel::period, using
    // whichever of the two formulas _linearFreqTable selects -- both
    // documented in xm_file.cpp. Not static (depends on _linearFreqTable).
    uint16_t xmNoteToPeriod(int note, int8_t finetune) const;
    float periodToFreq(uint16_t period) const; // inverse of xmNoteToPeriod(), same mode selection

    void recomputeStep(XmChannel& c); // sets sampleStep16 from c.period via periodToFreq()
    // Decodes `frameCount` delta-encoded frames starting at `fileOffset`
    // (using `startAccum` as the carry-in accumulator), discarding the
    // decoded values and returning only the final accumulator -- used
    // both at load() time (to precompute XmSample::accumAtLoopStart for
    // every looped sample) and by readRawSample()'s bounded catch-up path
    // for a chunk-cache miss that isn't a known anchor. Reads via _file
    // in CHUNK_SIZE-sized bursts through _rawScratch, not byte-by-byte --
    // a real, measured cost at S3M's ~260us/seek fixed overhead otherwise
    // (see S3mChannel::CHUNK_SIZE's comment).
    int32_t decodeDeltaRun(uint32_t fileOffset, uint32_t frameCount, bool is16Bit, int32_t startAccum);
    // readRawSample() itself is now just a tiny, always-inlined dispatcher
    // (loop-wrap normalize + length check + cache-hit check) so the
    // overwhelmingly common case (a cache hit) never pays a real function-
    // call's worth of overhead -- real-hardware diagnostics found pure
    // per-sample mixing cost (readChannelSample()+this) dominating the
    // real-time budget even more than SD refills once refills themselves
    // were already tuned. refillChunk() holds the actual (unchanged, only
    // ever reached on an actual miss) refill algorithm -- same
    // sequential/known-anchor/bounded-catch-up/random-seek logic as
    // before this split, just moved out of the hot inlined path.
    int16_t refillChunk(XmChannel& c, const XmSample& s, uint32_t index, uint32_t loopEnd);
    inline __attribute__((always_inline)) int16_t readRawSample(XmChannel& c, const XmSample& s, uint32_t index) {
        uint32_t loopEnd = s.loopStart + s.loopLength;
        if (c.looping && index >= loopEnd) {
            index = s.loopStart + ((index - loopEnd) % s.loopLength);
        }
        if (index >= s.length) return 0;
        if (c.chunkLen != 0 && c.chunkSampleIndex == c.sampleIndex &&
            index >= c.chunkStart && index < c.chunkStart + c.chunkLen) {
            g_xmDiagChunkHits++;
            return c.chunkBuf[index - c.chunkStart];
        }
        return refillChunk(c, s, index, loopEnd);
    }
    void restartSamplePosition(int ch); // resets position/re-primes the interpolation pair, WITHOUT touching pitch -- see triggerNote()
    void resolveSampleIndex(XmChannel& c, int note); // instrument+keymap -> sampleIndex, called from triggerNote()
    void triggerNote(int ch, int note); // sets pitch/sample from `note`, resets envelopes/fadeout, then restartSamplePosition()
    int16_t readChannelSample(XmChannel& c);
    void mixOneSample(int16_t& outL, int16_t& outR);

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
    void applyCell(int ch, uint8_t note, uint8_t instrument, bool hasNote, bool hasInstrument,
                    uint8_t volume, bool hasVolume, uint8_t effect, uint8_t param, bool hasEffect);

    void advanceTick();
    void advanceRow();
    void processExtendedCommand(int ch, uint8_t param); // Exy sub-commands, FT2's equivalent of S3M's S-prefixed specials
    void processTickEffects();
    void processEnvelopes(); // volume+panning envelope playback, called every tick unconditionally (not ActiveRow-gated)
};

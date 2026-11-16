#pragma once
#include "tracker_base.h"

// FastTracker 2 .xm player on the shared tracker base (see tracker_base.h).
// Plays the way FT2 does, as OpenMPT emulates it:
//  - Linear or Amiga frequency table, chosen per file. Periods are FT2's in
//    both modes, so slides move them by FT2's amounts.
//  - Instrument volume and panning envelopes (sustain, loop, Lxx), fadeout,
//    auto-vibrato and FT2's square-root pan law.
//  - Instruments map 96 notes into a flat, file-wide sample table.
//  - Sample data is delta-encoded 8 or 16 bit, or ModPlug's 4-bit ADPCM;
//    refillChunk() decodes it into the cache. Ping-pong loops are unrolled:
//    frames past the loop end read the loop backwards.
//
// Effects: the pattern effect column 0-33 as FT2 numbers it, plus the
// volume column. Tick 0 runs rowCell(), the other ticks tickEffects(), and
// every tick ends with the envelopes in tickEnd(), in FT2's order.
class XmPlayer : public TrackerPlayer {
public:
    // Channel counts across real files: median 14, p95 about 32, one at 40.
    // Channels past 32 are read and not played.
    static const int MAX_CHANNELS = 32;
    static const int MAX_INSTRUMENTS = 128; // the format's own limit
    static const int MAX_PATTERNS = 256;
    // Flat, file-wide sample pool: files have a median of 27 and p95 of 79.
    static const int MAX_SAMPLES = 160;
    static const int MAX_SAMPLES_PER_INSTRUMENT = 16; // FT2's limit, keymap entries are 4 bits
    static const int MAX_ORDERS = 256;
    static const int MAX_ROWS = 256;
    static const int MAX_ENV_POINTS = 12;
    static const int MAX_ADPCM = 32; // ADPCM samples with their delta table kept, 16 bytes each

    bool load(const char* path) override;

private:
    // Up to 12 (tick, value) points, values 0-64 (pan centre 32).
    struct XmEnvelope {
        uint16_t pointX[MAX_ENV_POINTS] = {0};
        uint8_t pointY[MAX_ENV_POINTS] = {0};
        uint8_t numPoints = 0;
        uint8_t sustainPoint = 0;
        uint8_t loopStartPoint = 0;
        uint8_t loopEndPoint = 0;
        uint8_t flags = 0; // the file's: bit0 on, bit1 sustain, bit2 loop
        bool enabled() const { return (flags & 0x01) && numPoints > 0; }
        bool sustainOn() const { return (flags & 0x02) != 0; }
        bool loopOn() const { return (flags & 0x04) != 0; }
    };

    struct XmInstrument {
        uint8_t keymap[48] = {0}; // two notes a byte, low nibble first: sample within the instrument
        XmEnvelope volEnv;
        XmEnvelope panEnv;
        uint16_t volFadeout = 0;
        uint8_t firstSampleIndex = 0; // into _samples[]
        uint8_t numSamplesUsed = 0;
        uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0; // auto-vibrato
        uint8_t sampleFor(int note) const { return (keymap[note >> 1] >> ((note & 1) * 4)) & 0x0F; }
    };

    enum { FMT_8BIT, FMT_16BIT, FMT_ADPCM };

    // fileOffset is the first data byte (past the ADPCM table). For a
    // ping-pong loop loopEnd is realLoopEnd plus the loop length, and
    // length is at least loopEnd.
    struct XmSample : TrackerSample {
        uint32_t realLoopEnd = 0;
        int16_t accumAtLoopStart = 0; // decoded value of frame loopStart - 1
        int16_t accumAtLoopEnd = 0;   // decoded value of frame realLoopEnd - 1
        uint8_t volume = 64;
        int8_t finetune = 0;
        uint8_t panning = 128;
        int8_t relativeNoteNumber = 0;
        uint8_t format = FMT_8BIT;
        uint8_t adpcmSlot = 0; // into _adpcmTables
        bool looping = false;
        bool pingpong = false;
    };

    // FT2's envelope state: tick counter, current point, value and step in 1/256ths.
    struct XmEnvState {
        uint16_t cnt = 0;
        int16_t amp = 0, ip = 0;
        uint8_t pos = 0;
    };

    // FT2's channel fields, named after its replayer. Volumes are 0-64,
    // periods FT2's (1-31999, 0 == silent), pan 0-255.
    struct XmChannel : TrackerVoice {
        uint8_t instrNum = 0;    // last instrument number given, 0 == none
        uint8_t envInst = 0;     // instrument of the last note: envelopes, fadeout, auto-vibrato
        uint8_t sampleIndex = 0xFF;
        uint8_t tonNr = 0;       // last note, 1-96
        int8_t relTonNr = 0, fineTune = 0;
        uint8_t realVol = 0, outVol = 0, oldVol = 0, oldPan = 128, outPan = 128;
        uint16_t realPeriod = 0, outPeriod = 0, wantPeriod = 0;
        uint16_t portaSpeed = 0; // period units a tick
        uint8_t portaDir = 0;    // 0 none, 1 up to wantPeriod or arrived, 2 down
        uint16_t stepPeriod = 0; // the period step23 is for
        uint32_t step23 = 0;     // exact step, 16.23
        uint8_t stepAcc = 0;     // step23's low 7 bits carried from tick to tick

        // This row's cell.
        uint8_t effTyp = 0, eff = 0, volCol = 0, rowNote = 0, rowInst = 0;

        // Effect memory.
        uint8_t portaUpSpeed = 0, portaDownSpeed = 0, fPortaUpSpeed = 0, fPortaDownSpeed = 0;
        uint8_t ePortaUpSpeed = 0, ePortaDownSpeed = 0;
        uint8_t volSlideSpeed = 0, fVolUpSpeed = 0, fVolDownSpeed = 0, panSlideSpeed = 0, globVolSlideSpeed = 0;
        uint8_t vibPos = 0, vibSpeed = 0, vibDepth = 0, tremPos = 0, tremSpeed = 0, tremDepth = 0;
        uint8_t waveCtrl = 0; // E4x low nibble, E7x high nibble
        uint8_t retrigSpeed = 0, retrigVol = 0, retrigCnt = 0;
        uint8_t tremorParam = 0, tremorPos = 0;
        uint8_t smpOffset = 0;
        uint8_t patternLoopRow = 0, patternLoopCount = 0;
        bool glissando = false;

        bool sustainActive = true; // false once keyed off
        XmEnvState volEnv, panEnv;
        uint16_t fadeOutAmp = 32768, fadeOutSpeed = 0;
        uint8_t eVibPos = 0;
        uint16_t eVibAmp = 0, eVibSweep = 0;

        uint16_t finalVol = 0; // 0-4096, what the mixer plays
        uint8_t finalPan = 128;
    };

    bool _linearFreqTable = true;
    bool _exactFinetune = false; // old ModPlug files, see detectHeader()

    XmInstrument _instruments[MAX_INSTRUMENTS];
    XmSample _samples[MAX_SAMPLES];
    int _numSamplesTotal = 0;
    int8_t _adpcmTables[MAX_ADPCM][16];
    int _numAdpcm = 0;

    uint32_t _patternOffsets[MAX_PATTERNS]; // file offset of each pattern's cell data
    uint16_t _patternRowCounts[MAX_PATTERNS];
    // Packed bytes of each pattern, cut short by the end of the file. A
    // pattern with none plays as blank rows, and so do cells past the end.
    uint16_t _patternSizes[MAX_PATTERNS];
    uint32_t _openPatternEnd = 0; // file offset just past the open pattern's data
    uint8_t _orderTable[MAX_ORDERS] = {0};
    // From the header, unclamped: decodeRow() reads this many cells a row.
    int _fileNumChannels = 0;
    uint8_t _rowDelay = 0;       // EEx: repeats of this row left
    uint8_t _pendingRowDelay = 0; // EEx given on this row

    // Accumulators found by catch-up decodes, so a repeated 9xx into a long
    // sample doesn't decode from the start again.
    struct SeekHint { uint32_t index; int16_t accum; uint8_t sample; };
    static const int NUM_SEEK_HINTS = 16;
    SeekHint _seekHints[NUM_SEEK_HINTS];
    uint8_t _nextSeekHint = 0;

    // Decoded frames per voice cache, sized to fill SharedRamArena.
    static const int CHUNK_FRAMES = 896;

    XmChannel _channels[MAX_CHANNELS];
    int16_t _chunks[MAX_CHANNELS][CHUNK_FRAMES];
    TrackerTail _tails[MAX_CHANNELS];
    int16_t _tailPool[TAIL_POOL_FRAMES];

    // Pattern scan state, same as S3mPlayer's but for up to 256 rows.
    int _openPatternNum = -1;
    uint32_t _rowOffsets[MAX_ROWS];
    int _rowOffsetsValid = 0;
    uint32_t _scanCursor = 0;
    int _openPatternRowCount = 64;

    // Pitch. A note index is FT2's: note * 16 plus a finetune slot (0-31).
    uint16_t note2Period(int index) const;
    uint16_t notePeriod(int note, int fineTune) const;
    uint16_t relocateTon(uint16_t period, int arpNote, const XmChannel& c) const;
    uint32_t periodToStep23(uint32_t period) const;

    // Sample decoding. accum is the value of the frame before `first`.
    uint32_t decodeForward(const XmSample& s, uint32_t first, uint32_t count, int32_t& accum, int16_t* out, uint32_t skip);
    bool decodeBackward(const XmSample& s, uint32_t low, uint32_t high, int32_t top, bool haveTop, int16_t* out);
    int32_t accumAt(const XmSample& s, uint32_t index); // value of frame index - 1
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;

    // Load-time tracker detection, see detectHeader().
    static uint16_t detectHeader(const uint8_t* hdr);
    static void detectInstrument(uint16_t& m, const uint8_t* ih, uint16_t numSamples, int& lastType, int64_t& lastShs,
                                 bool& withSamples);
    static void detectSample(uint16_t& m, const uint8_t* sh, uint8_t& reservedOr, int& lastReserved);
    bool detectEnd(uint16_t m, const uint8_t* hdr, uint8_t reservedOr);

    void openPatternForScan(int num);
    void advanceToRow(int targetRow);
    void decodeRow(bool apply);

    // FT2's replayer, one channel at a time.
    void rowCell(int ch, uint8_t note, uint8_t inst, uint8_t vol, uint8_t effTyp, uint8_t eff);
    void triggerNote(XmChannel& c, uint8_t ton, uint8_t effTyp, uint8_t eff);
    void resetVolumes(XmChannel& c);
    void triggerInstrument(XmChannel& c);
    void keyOff(XmChannel& c);
    void preparePortamento(XmChannel& c, uint8_t note, uint8_t inst);
    void tickZeroEffects(int ch);
    void tickEffects(int ch);
    void noteDelayTrigger(XmChannel& c);
    void multiRetrig(XmChannel& c, bool tickZero);
    void portamento(XmChannel& c);
    void vibrato(XmChannel& c);
    void volSlide(XmChannel& c, uint8_t param);
    void setEnvelopePos(XmChannel& c, uint8_t param);
    void envSetPos(const XmEnvelope& e, XmEnvState& st, uint8_t param);
    int envTick(const XmEnvelope& e, XmEnvState& st, bool sustainActive);
    const XmInstrument* instrumentOf(uint8_t num) const {
        return (num >= 1 && num <= MAX_INSTRUMENTS) ? &_instruments[num - 1] : nullptr;
    }

    void advanceRow() override;
    void processTickEffects() override;
    void tickEnd() override;
    void voiceGains(const TrackerVoice& v, int32_t& l, int32_t& r) override;
};

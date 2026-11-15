#pragma once
#include "tracker_base.h"

// Impulse Tracker .it player (IT 2.0x-2.15) on the shared tracker base,
// see tracker_base.h and the notes at the top of it_file.cpp.
//  - Sample mode and instrument mode. Pattern channels drive a fixed pool
//    of voices, so a note can keep sounding after the next one starts (NNA).
//  - Volume, panning and pitch envelopes with loops and sustain loops.
//  - Linear or Amiga slides, chosen per file.
//  - Compressed samples (IT 2.14/2.15) are unpacked once into a cache file
//    on the card (cacheDir) and streamed from there like plain samples.
//    Stereo samples are mixed down to mono into the same cache.
class ItPlayer : public TrackerPlayer {
public:
    static const int MAX_CHANNELS = 64;
    // Sounding voices, foreground and NNA. Sets the CPU cost: each one
    // mixes like an XM channel. See it_file.cpp for the numbers.
    static const int MAX_VOICES = 32;
    static const int MAX_INSTRUMENTS = 99; // IT 2.x's own limit
    static const int MAX_SAMPLES = 99;
    static const int MAX_PATTERNS = 240;
    static const int MAX_ORDERS = 256;
    static const int MAX_ENV_POINTS = 25;

    // Folder for unpacked compressed samples, hidden from the browser.
    // It holds at most CACHE_SLOTS files, the least recently used one is
    // replaced. The host harness points this somewhere else.
    static const char* cacheDir;
    static const int CACHE_SLOTS = 4;
    // Called with 0-100 while compressed samples are unpacked in load().
    static void (*progressCallback)(int percent);

    ~ItPlayer() override { _cacheFile.close(); }
    bool load(const char* path) override;

private:
    struct ItEnvelope {
        uint8_t flags = 0; // bit0 on, bit1 loop, bit2 sustain loop, bit7 filter (pitch envelope only)
        uint8_t numPoints = 0;
        uint8_t loopStart = 0, loopEnd = 0, susStart = 0, susEnd = 0; // point indices
        int8_t y[MAX_ENV_POINTS];   // volume 0-64, pan and pitch -32..32
        uint16_t x[MAX_ENV_POINTS]; // tick
        bool on() const { return (flags & 1) && numPoints > 0; }
    };

    struct ItInstrument {
        uint8_t noteMap[120];   // pattern note -> note played
        uint8_t sampleMap[120]; // pattern note -> sample, 1-based, 0 == none
        ItEnvelope volEnv, panEnv, pitchEnv;
        uint16_t fadeout = 0;   // subtracted from a 65536 fade volume per tick, once fading
        uint8_t nna = 0;        // 0 cut, 1 continue, 2 note off, 3 note fade
        uint8_t dct = 0;        // duplicate check: 0 off, 1 note, 2 sample, 3 instrument
        uint8_t dca = 0;        // duplicate action: 0 cut, 1 note off, 2 note fade
        uint8_t globalVolume = 128; // 0-128
        uint8_t defaultPan = 32;    // 0-64
        bool usePan = false;
        int8_t pitchPanSep = 0;
        uint8_t pitchPanCentre = 60;
    };

    struct ItSample;
    // What a voice plays: the sample with one of its loops (sustain or
    // normal). A ping-pong loop is unrolled: frames past loopEnd up to
    // twice the loop length read the loop backwards, see refillChunk().
    struct ItSampleView : TrackerSample {
        uint8_t sampleIndex = 0;
        bool pingpong = false;
        uint32_t realLoopEnd = 0; // loop end in the sample's own frames
    };

    struct ItSample {
        uint32_t length = 0;       // frames
        uint32_t dataOffset = 0;   // first frame (left channel for stereo)
        uint32_t rightOffset = 0;  // right channel data, 0 == mono
        uint32_t c5speed = 8363;
        bool is16Bit = false;
        bool isSigned = true;
        bool inCache = false;      // data is in the cache file (unpacked or mixed down), else the .it file
        bool packed = false;       // IT 2.14/2.15 compressed in the .it file
        uint8_t globalVolume = 64; // 0-64
        uint8_t volume = 64;       // default note volume 0-64
        uint8_t defaultPan = 32;   // 0-64
        bool usePan = false;
        bool hasSustain = false;
        ItSampleView normal;       // plays with the normal loop (or none)
        ItSampleView sustain;      // plays with the sustain loop while the note is held, else same as normal
    };

    // One sounding note. A channel's foreground voice follows the channel
    // every tick; NNA leaves it in the background with its last values.
    struct ItVoice : TrackerVoice {
        int8_t channel = -1;      // owning pattern channel, -1 == free
        bool background = false;
        uint8_t instrument = 0;   // 1-based, 0 == none (sample mode)
        uint8_t sampleIndex = 0;  // into _samples[]
        uint8_t note = 0;         // pattern note, for the duplicate check
        bool keyOff = false;
        bool fading = false;
        bool volEnvOn = false, panEnvOn = false, pitchEnvOn = false;
        uint16_t volEnvPos = 0, panEnvPos = 0, pitchEnvPos = 0;
        uint32_t fadeVolume = 65536;
        uint16_t baseVolume = 0;  // 0-4096: note, channel, sample and instrument volume
        uint16_t basePan = 128;   // 0-256
        float baseHz = 0;         // pitch before the pitch envelope
        float lastHz = -1;        // hz the step was worked out from
        uint16_t loudness = 0;    // last mix volume before envelopes ran out, for stealing
    };

    struct ItChannel {
        int8_t voice = -1;      // foreground voice, -1 == none
        uint8_t instrument = 0; // last instrument, 1-based
        uint8_t sampleIndex = 0xFF;
        uint8_t note = 0;       // last pattern note
        uint8_t volume = 64;    // note volume 0-64
        uint8_t channelVolume = 64;
        uint16_t pan = 128;     // 0-256
        bool surround = false;
        bool muted = false;     // disabled in the header
        int32_t period = 0;     // see it_file.cpp, 0 == none
        int32_t portaTarget = 0;
        int32_t playPeriod = 0; // with vibrato this tick
        int8_t arpeggioSemis = 0;
        int8_t tremoloDelta = 0;
        int16_t panbrelloDelta = 0;
        bool tremorMute = false;
        uint8_t nnaOverride = 0xFF; // S73-S76 for the current note

        // Effect memory.
        uint8_t volSlide = 0, chanVolSlide = 0, panSlide = 0, globalSlide = 0;
        uint8_t volColSlide = 0;
        uint8_t portaMem = 0, tonePortaMem = 0;
        uint8_t vibSpeed = 0, vibDepth = 0, vibPos = 0, vibWave = 0;
        uint8_t tremSpeed = 0, tremDepth = 0, tremPos = 0, tremWave = 0;
        uint8_t panbSpeed = 0, panbDepth = 0, panbPos = 0, panbWave = 0;
        uint8_t tremorParam = 0, tremorCount = 0;
        uint8_t arpParam = 0;
        uint8_t retrigParam = 0, retrigCount = 0;
        uint8_t offsetMem = 0, offsetHigh = 0;
        uint8_t tempoMem = 0;
        uint8_t loopRow = 0, loopCount = 0;

        // This row's effect and what runs on the ticks after the first.
        uint8_t effect = 0, param = 0;
        uint8_t volCol = 0xFF;       // 0xFF == none
        bool vibFine = false;
        uint8_t noteCutTick = 0xFF, noteDelayTick = 0xFF;
        int16_t pendNote = -1;
        uint8_t pendInst = 0, pendVol = 0xFF; // cell held by SDx
        uint8_t pendEffect = 0, pendParam = 0;
    };

    // Cache file layout: a 16-byte header (magic, key, data bytes, use
    // count), then each cached sample's frames as mono signed PCM.
    bool openCache(const char* path, uint32_t key);
    bool mixDown(FsFile& src, uint32_t srcL, FsFile& dst, uint32_t dstAt, uint32_t srcR, uint32_t frames, bool is16, bool isSigned);
    bool unpackSample(FsFile& cache, ItSample& s, uint32_t srcOffset, bool it215, uint32_t& written,
                      uint32_t totalFrames, uint32_t& doneFrames);

    bool loadInstrument(int i, uint32_t offset, bool oldFormat);
    bool loadSample(int i, uint32_t offset);
    void setupViews(ItSample& s, int index, uint8_t flags, uint32_t ls, uint32_t le, uint32_t sls, uint32_t sle);
    void scanChannels();

    // Pattern reading. IT packs each pattern with per-channel memory of
    // the last mask and values, so reading starts at the pattern's first
    // row and goes forward; a jump back within a pattern starts over.
    void openPattern(int num);
    void readRowTo(int row);
    void readRow(bool apply);

    float periodToHz(int32_t period, uint32_t c5) const;
    int32_t noteToPeriod(int note, uint32_t c5) const;
    void slidePitch(ItChannel& c, int32_t amount); // amount in fine units, + is up

    int allocVoice(int forChannel);
    void releaseVoice(ItVoice& v);
    void voiceNoteOff(ItVoice& v);
    void applyNna(int ch, uint8_t newInst, uint8_t newSample, uint8_t newNote);
    void triggerNote(int ch, uint8_t note, uint8_t inst, bool tonePorta);
    static const int NO_NOTE = -1;
    // note: 0-119, 120-253 note fade, 254 note cut, 255 note off, NO_NOTE.
    void handleCell(int ch, int note, uint8_t inst, uint8_t vol, uint8_t effect, uint8_t param);
    void rowEffect(int ch);
    void tickEffect(int ch);
    void volumeColumnRow(int ch);
    void volumeColumnTick(int ch);
    void specialRow(int ch, uint8_t param);
    void doVolSlide(uint8_t& v, uint8_t param, uint8_t max, bool firstTick);
    void updateChannelVoice(int ch);
    void processVoices();

    void advanceRow() override;
    void processTickEffects() override;
    void tickEnd() override { processVoices(); }
    uint32_t tickLength() override;
    uint32_t _tickFraction = 0; // carried part of a frame, in 1/65536ths
    uint8_t _tempoMode = 0;     // OpenMPT's tempo mode: 0 classic, 1 alternative, 2 modern, see readTempoMode()
    uint8_t _rowsPerBeat = 4;
    int readTempoMode(uint32_t at, int numInstruments);
    uint32_t sampleDataEnd(const ItSample& s);
    int16_t refillChunk(TrackerVoice& v, uint32_t index) override;

    bool _linear = true;
    bool _oldEffects = false;
    bool _linkG = false;      // G shares E/F's memory
    bool _instrumentMode = false;
    bool _stereo = true;
    uint8_t _mixVolume = 48;  // header MV
    uint8_t _globalVol = 128; // 0-128
    uint8_t _baseSpeed = 6;   // speed without this row's S6x
    bool _rowDelaySet = false; // an SEx already ran on this row
    int _numSamples = 0;

    ItInstrument _instruments[MAX_INSTRUMENTS];
    ItSample _samples[MAX_SAMPLES];
    uint8_t _orderTable[MAX_ORDERS];
    uint32_t _patternOffsets[MAX_PATTERNS]; // 0 == empty 64-row pattern
    uint16_t _patternRows[MAX_PATTERNS];

    ItChannel _channels[MAX_CHANNELS];
    ItVoice _voices[MAX_VOICES];
    static const int CHUNK_FRAMES = 512; // a refill reads up to 2 bytes a frame per side into _rawScratch
    int16_t _chunks[MAX_VOICES][CHUNK_FRAMES];

    FsFile _cacheFile;

    // Pattern read state.
    int _openPattern = -1;
    int _patRow = 0;          // next row readRow() reads
    uint32_t _patCursor = 0;  // file offset of that row
    uint32_t _patStart = 0;
    int _patRowCount = 64;
    uint8_t _lastMask[MAX_CHANNELS], _lastNote[MAX_CHANNELS], _lastInst[MAX_CHANNELS], _lastVol[MAX_CHANNELS];
    uint8_t _lastCmd[MAX_CHANNELS], _lastParam[MAX_CHANNELS];
    uint8_t _patBuf[64];      // read buffer for pattern bytes
    int _patBufLen = 0, _patBufPos = 0;
    bool patByte(uint8_t& b);
};

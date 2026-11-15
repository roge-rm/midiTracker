#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Shared base for the tracker players (MOD, S3M, XM). Streams pattern and
// sample data from SD and mixes it on core 0 into Synth's WAV ring.
//
// The base holds what every format does the same way: play/pause/elapsed
// time, the update() loop that keeps the ring topped up, the tick/row
// clock, pattern break/jump/loop, the voice mixer (16.16 resampling with
// linear interpolation, volume, pan, global volume, soft clamp) and the
// per-voice sample cache. A format keeps its parsing, cell decoding,
// effect semantics and how sample data is decoded into the cache.
//
// Mixing is a block at a time per voice: linear interpolation, an optional
// resonant filter (IT), then left and right gains that ramp over 16 frames
// up and 42 down (OpenMPT's defaults at 44.1 kHz), so volume changes, cuts
// and new notes don't click. MOD, S3M and XM get their gains from
// mixVolume and mixPan after every tick, and a note that's cut or replaced
// fades out in a tail made from its cached frames. IT sets its own gains.
//
// Voices are separate from pattern channels. MOD, S3M and XM use one voice
// per channel (their channel struct derives from TrackerVoice); a format
// with virtual voices (IT's NNA) can attach a larger pool of its own.
//
// Hot path (update(), the mixer and the inlined voice reads) runs from
// RAM via __not_in_flash_func. A cache miss calls the format's
// refillChunk(), which may read SD.

// What the mixer needs to know about a sample. Each format's sample struct
// derives from this.
struct TrackerSample {
    uint32_t fileOffset = 0; // absolute offset of the sample data in the file
    uint32_t length = 0;     // in frames
    uint32_t loopStart = 0;  // in frames
    uint32_t loopEnd = 0;    // in frames; loopEnd <= loopStart means no loop
};

// One sounding sample: position, step, the interpolation pair, mix volume
// and pan, and a decoded int16 cache of the sample data around the
// position. The cache stays valid across a retrigger of the same sample.
struct TrackerVoice {
    const TrackerSample* smp = nullptr; // sample being played, nullptr == none
    uint32_t samplePos16 = 0;  // fraction between lastSrcSample and nextSrcSample, 16.16
    uint32_t sampleStep16 = 0; // source frames per output frame, 16.16; 0 == silent
    uint32_t srcIndex = 0;     // frame index of lastSrcSample
    int16_t lastSrcSample = 0, nextSrcSample = 0;
    bool havePair = false; // false == voice is silent until restarted
    bool looping = false;
    uint8_t mixVolume = 64; // 0-64, what the mixer scales by this tick
    uint8_t mixPan = 128;   // 0 (left) .. 255 (right)

    int16_t* chunkBuf = nullptr;              // set by attachVoices()
    const TrackerSample* chunkSmp = nullptr;  // sample the cache holds, nullptr == empty
    uint32_t chunkStart = 0;                  // frame index of chunkBuf[0]
    uint32_t chunkLen = 0;                    // valid frames in chunkBuf
    int32_t chunkAccum = 0;                   // format decode state at chunkStart+chunkLen (XM delta)

    // Mixer gains: curL/curR are the gain now << 12 (32640 is full volume
    // on one side), moving by stepL/stepR for rampLeft more frames.
    int32_t curL = 0, curR = 0, stepL = 0, stepR = 0;
    uint16_t rampLeft = 0;
};

// Resonant filter coefficients (Q24) and state, see mixVoiceBlock().
struct TrackerFilter {
    int32_t a0 = 0, b0 = 0, b1 = 0, y1 = 0, y2 = 0;
};

// A cut or replaced note fading out over RAMP_DOWN frames, from a copy of
// the frames it still needs. pos is 16.16 into buf.
struct TrackerTail {
    const int16_t* buf;
    uint32_t pos, step;
    int32_t curL, curR, stepL, stepR;
    uint16_t len, left;
};

class TrackerPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    virtual ~TrackerPlayer() {}

    // Parses the file and leaves the player in STATE_PAUSED, or STATE_ERROR
    // (see errorMessage()). Doesn't touch Synth's stream until play().
    virtual bool load(const char* path) = 0;

    // Closes the files, resets Synth's WAV stream, back to STATE_IDLE.
    void close();

    // Call every loop() whatever the state: tops the ring up, at most
    // MAX_FRAMES_PER_UPDATE frames per call so one call stays short.
    void update();

    void play();  // start/resume, pre-fills the ring then unmutes the stream
    void pause(); // hold in place, mutes the stream, keeps what's buffered
    void stop();  // close()

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }
    uint32_t elapsedMs() const; // wall clock while playing

    // Position for the UI. A tracker has no fixed length (it can loop), so
    // the screen shows the order position and row instead of a time bar.
    int patternOrderPosition() const { return _orderPos; }
    int songLength() const { return _songLength; }
    int currentRow() const { return _row < 0 ? 0 : _row; } // -1 right after load(), before the first row
    int channelCount() const { return _numChannels; }
    int instrumentCount() const { return _numInstruments; }

    // Highest step the resampler takes, 8 source frames per output frame
    // (three octaves above 44.1 kHz). Notes in real files rarely pass
    // 6; a period slid down to 1 would need ~80 cache reads per output
    // frame and starve the ring.
    static const uint32_t MAX_STEP16 = 8u << 16;

#ifdef TRACKER_PROFILE
    // Profile build only (env:pico_profile): time spent in update() calls
    // that produced frames, and in sample cache refills (SD reads).
    struct Profile { uint32_t updateUs, frames, worstUs, refillUs, refills, refillFrames, seekUs, readUs, reads, hist[6], histUs[6]; };
    static Profile profile;
#endif

protected:
    static const size_t MAX_FRAMES_PER_UPDATE = 128;
    static const int RAW_SCRATCH_BYTES = 1024;

    // Points every voice at its slice of chunkStore (chunkFrames each).
    // Voices are `count` objects `stride` bytes apart starting at first,
    // so a format's channel struct can derive from TrackerVoice.
    void attachVoices(TrackerVoice* first, size_t stride, int count, int16_t* chunkStore, uint32_t chunkFrames);

    // Error helper for load(): closes the files, sets the message and STATE_ERROR.
    bool loadFail(const char* msg);
    // Resets the clock and position for a freshly loaded song, ready to play().
    void resetSequencer(uint8_t speed, uint16_t tempo);

    static uint32_t ticksToSamples(uint16_t tempo);
    // Output frames in the tick that just started. IT overrides this to
    // carry the fraction ticksToSamples() drops.
    virtual uint32_t tickLength() { return ticksToSamples(_tempo); }
    // Every step goes through one of these so the MAX_STEP16 floor is applied in one place.
    static uint32_t limitStep(uint32_t step16) { return step16 > MAX_STEP16 ? MAX_STEP16 : step16; }
    static uint32_t stepForHz(float hz);

    // Moves to the next row: applies a pending pattern break/position jump
    // (a break row above maxBreakRow becomes 0), else _row++ wrapping at
    // rowCount to the next order. The format then checks the order range.
    void stepPosition(int rowCount, int maxBreakRow);
    // Pattern loop (MOD E6x, S3M SBx, XM E6x) with the channel's loop memory.
    void patternLoop(uint8_t& loopRow, uint8_t& loopCount, uint8_t val);

    // Per-tick hooks. advanceTick() calls tickStart(), then advanceRow() on
    // the first tick of a row (unless a pattern delay holds it) or
    // processTickEffects() on the others, then tickEnd().
    virtual void tickStart() {}
    virtual void advanceRow() = 0;
    virtual void processTickEffects() = 0;
    virtual void tickEnd() {}

    // Fills v's cache so it holds `index` (already loop-wrapped and below
    // the sample length) and returns that frame, or 0 when the read fails.
    virtual int16_t refillChunk(TrackerVoice& v, uint32_t index) = 0;

    // One cached sample frame, loop-wrapped; 0 past the end of the sample.
    inline __attribute__((always_inline)) int16_t readRawSample(TrackerVoice& v, uint32_t index) {
        const TrackerSample& s = *v.smp;
        if (v.looping && s.loopEnd > s.loopStart && index >= s.loopEnd) {
            index = s.loopStart + ((index - s.loopEnd) % (s.loopEnd - s.loopStart));
        }
        if (index >= s.length) return 0;
        if (v.chunkSmp == v.smp && index - v.chunkStart < v.chunkLen) return v.chunkBuf[index - v.chunkStart];
#ifdef TRACKER_PROFILE
        uint32_t t0 = micros();
        int16_t r = refillChunk(v, index);
        profile.refillUs += micros() - t0;
        profile.refills++;
        profile.refillFrames += v.chunkLen;
        return r;
#else
        return refillChunk(v, index);
#endif
    }

    // Call once a file that sample data is read from is open. A file in one
    // run of sectors is then read with raw multi-sector reads, which skip
    // SdFat's cluster walk and one-sector cache.
    void useSampleFile(FsFile& f);
    // Reads len (<= rawSpan(pos)) sample bytes at pos of f into
    // _rawScratch and returns where they start; got is the bytes read.
    const uint8_t* readSampleBytes(FsFile& f, uint32_t pos, uint32_t len, int& got);
    // Most bytes one readSampleBytes() at pos can take. Reading that much
    // leaves the next read on a sector boundary, so a longer run read in
    // pieces stays one multi-sector read on the card.
    uint32_t rawSpan(uint32_t pos) const { return (uint32_t)sizeof(_rawScratch) - (pos & 511); }

    // Restarts v at frame `index` of its sample and primes the pair.
    void startVoiceAt(TrackerVoice& v, uint32_t index);

    // Returns n interleaved stereo frames (n <= BLOCK_FRAMES, all inside
    // one tick). The base mixes every voice and the tails; IT has its own.
    static const uint32_t BLOCK_FRAMES = 64;
    virtual const int16_t* mixBlock(uint32_t n);
    // Global volume, makeup gain and soft clamp on one channel's voice sum.
    static int16_t finishMix(int32_t mix, uint8_t globalVolume);
    // Adds n frames of v into acc (interleaved stereo), through filter f
    // if not null. With stopAfterRamp the voice is left once its ramp ends.
    void mixVoiceBlock(TrackerVoice& v, TrackerFilter* f, int32_t* acc, uint32_t n, bool stopAfterRamp);
    // Voice sums for one block, then the same memory as the output frames.
    union { int32_t acc[BLOCK_FRAMES * 2]; int16_t out[BLOCK_FRAMES * 2]; } _mix;

    // Ramp lengths in frames, OpenMPT's defaults at 44.1 kHz (363 and 952 us).
    static const int RAMP_UP = 16;
    static const int RAMP_DOWN = 42;
    // Moves v's gains to the new ones over RAMP_UP or RAMP_DOWN frames.
    static void setRamp(TrackerVoice& v, int32_t tgtL, int32_t tgtR);
    // IT sets this and works out its own gains. Otherwise the base sets
    // them from mixVolume and mixPan after every tick.
    bool _formatGains = false;
    // Where tails live, given by the format; none by default. All tails
    // end within a tick, so the pool is reused from the start each tick.
    void attachTails(TrackerTail* tails, int count, int16_t* pool, uint32_t poolFrames);
    // Room for 32 tails at 1x pitch (about 45 frames each). Higher pitches
    // get shorter tails once it runs out.
    static const uint32_t TAIL_POOL_FRAMES = 1536;

    FsFile _file;        // header reads at load(), sample data while playing
    FsFile _patternFile; // pattern reads while playing, so they never share a position with sample reads
    State _state = STATE_IDLE;
    char _error[64] = {0};

    int _songLength = 0;
    int _numChannels = 0;
    int _numInstruments = 0;
    uint8_t _globalVolume = 64; // 0-64

    // Sequencer position.
    int _orderPos = 0;
    int _row = -1;
    int _tick = 0;
    uint8_t _speed = 6;   // ticks per row
    uint16_t _tempo = 125; // BPM, a tick is 2.5/tempo seconds
    uint32_t _samplesUntilNextTick = 0;
    uint8_t _patternDelayRepeatsLeft = 0;
    bool _patternBreakPending = false;
    int _patternBreakRow = 0;
    bool _positionJumpPending = false;
    int _positionJumpTarget = 0;

    // Raw file bytes for one read, decoded into a voice's chunkBuf. A
    // sector over RAW_SCRATCH_BYTES, so a raw read that starts partway
    // into a sector still gets at least that many.
    alignas(4) uint8_t _rawScratch[RAW_SCRATCH_BYTES + 512];

private:
    void advanceTick();
    void applyGains();
    void spawnTail(TrackerVoice& v);
    void mixTail(TrackerTail& t, int32_t* acc, uint32_t n);
    uint32_t resample(TrackerVoice& v, int32_t* out, uint32_t n);
    template <bool FILTER, bool RAMP> void mixVoice(TrackerVoice& v, TrackerFilter* f, int32_t* acc, uint32_t n);
    TrackerVoice& voiceAt(int i) { return *(TrackerVoice*)((uint8_t*)_voices + (size_t)i * _voiceStride); }

    TrackerVoice* _voices = nullptr;
    size_t _voiceStride = sizeof(TrackerVoice);
    int _voiceCount = 0;

    // Files read with raw sector reads: firstSector 0 == read through SdFat.
    struct SampleFile { const FsFile* f; uint32_t firstSector; uint32_t size; };
    SampleFile _sampleFiles[2] = {};

    TrackerTail* _tails = nullptr;
    int _maxTails = 0, _tailCount = 0;
    int16_t* _tailPool = nullptr;
    uint32_t _tailPoolFrames = 0, _tailPoolUsed = 0;

    uint32_t _elapsedMsFrozen = 0;
    uint32_t _lastResumeMicros = 0;
};

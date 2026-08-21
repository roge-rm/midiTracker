#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Streams a RIFF/WAVE file directly off the SD card and feeds it to
// Synth's WAV playback stream (see Synth::wavStream*() in synth.h) --
// never buffers the whole file in RAM, same convention MidiPlayer uses
// for SMF playback. Unlike MidiPlayer, total duration is known immediately
// after load() (the `data` chunk's byte length gives it directly, no
// prescan).
//
// Supports uncompressed PCM (8-bit unsigned or 16-bit signed) and IMA
// ADPCM (4-bit, ~4:1 compressed). Not supported: Microsoft ADPCM or any
// other non-PCM codec, WAVE_FORMAT_EXTENSIBLE, more than 2 channels.
//
// seekTo() cost differs by format: PCM is real O(1) byte-offset
// arithmetic (no reopen/rescan, contrast MidiPlayer::seekTo()); ADPCM
// samples aren't randomly byte-addressable (each compressed block's
// decode state only starts fresh at the block's own header), so an ADPCM
// seek jumps to the target's block boundary directly (still no file
// reopen) and then decodes-and-discards forward to the exact sample
// within that one block -- bounded by a single block's sample count
// (typically a few hundred to a few thousand), not by file size, so still
// nowhere near MidiPlayer's whole-file rescan cost.
class WavPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    // Parses the RIFF/WAVE container, walking chunks until both 'fmt '
    // and 'data' are found (skipping anything else -- LIST/fact/JUNK/etc,
    // real-world files commonly carry extras). Leaves the player in
    // STATE_PAUSED (ready to play()) on success, or STATE_ERROR (see
    // errorMessage()) on failure. Does not touch Synth's stream at all --
    // that only starts once play() is first called (see play()'s comment).
    bool load(const char* path);

    // Closes the file handle and resets to STATE_IDLE. Also resets
    // Synth's WAV stream (see Synth::wavStreamReset()) so nothing from
    // this file keeps sounding after it's closed.
    void close();

    // Call every loop() iteration regardless of play/pause state -- keeps
    // Synth's ring buffer topped up (see synth.h's comment on why this is
    // deliberately not gated on STATE_PLAYING) by reading/converting/
    // resampling a bounded chunk from SD when there's room for it. Cheap
    // no-op when the buffer's already full or nothing's loaded.
    void update();

    void play();  // resume/start -- unmutes Synth's stream (see Synth::wavStreamSetActive())
    void pause(); // hold in place, mutes Synth's stream without discarding buffered audio
    void stop();  // stop + close()

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t elapsedMs() const; // wall-clock based, deliberately independent of
                                  // how far ahead the ring buffer has been filled
    uint32_t totalMs() const;    // known immediately after load() -- no prescan needed

    // See this class's header comment for PCM-vs-ADPCM cost. Either way,
    // no file reopen. Discards whatever's currently buffered
    // (Synth::wavStreamReset()) since it no longer corresponds to the new
    // position; update() refills it on subsequent ticks. Clamped to
    // [0, totalMs()]. Preserves whether playback was PLAYING or PAUSED;
    // lands on STATE_DONE instead if the target is at/past the end.
    void seekTo(uint32_t targetMs);

    uint32_t sampleRate() const { return _srcSampleRate; }
    uint8_t channels() const { return _srcChannels; }
    uint8_t bitsPerSample() const { return _srcBitsPerSample; } // 4 for ADPCM

    // Mirrors Synth::wavStreamTookUnderrun() -- meant to be polled once
    // per UI tick for a small on-screen indicator.
    bool underrun() const;

private:
    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};

    uint32_t _dataChunkStart = 0;
    uint32_t _dataChunkLen = 0;
    uint32_t _srcSampleRate = 0;
    uint8_t _srcChannels = 0;
    uint8_t _srcBitsPerSample = 0;
    uint32_t _bytesPerSrcFrame = 0; // channels * (bitsPerSample/8) -- PCM only, meaningless (0) for ADPCM
    uint32_t _srcFramesRead = 0;    // read cursor, in source frames from data start
    bool _sourceEnded = false;      // wavStreamEnd() already signaled for this position

    // IMA ADPCM (format tag 0x0011) -- see readAdpcmFrame() in wav_file.cpp
    // for the block/decode-state shape these describe.
    bool _isAdpcm = false;
    uint16_t _blockAlign = 0;      // bytes per compressed block (fmt chunk's nBlockAlign)
    uint16_t _samplesPerBlock = 0; // samples per channel per block (fmt chunk's extra field)
    uint32_t _adpcmBlockBytesLeft = 0; // bytes left in the current block; 0 == next call must read a fresh block header
    int32_t _adpcmPredictorL = 0, _adpcmPredictorR = 0;
    int _adpcmStepIndexL = 0, _adpcmStepIndexR = 0;
    // Mono: a byte holds 2 nibbles/samples; holds the second (high)
    // nibble's already-decoded sample between readAdpcmFrame() calls.
    bool _adpcmHavePendingMonoSample = false;
    int16_t _adpcmPendingMonoSample = 0;
    // Stereo: each channel's nibbles arrive in interleaved 4-byte/8-sample
    // groups (L group, then R group, repeating) -- these hold a group's
    // decoded samples not yet consumed. groupPos == 8 means exhausted,
    // next read for that channel starts a fresh group.
    int16_t _adpcmGroupL[8], _adpcmGroupR[8];
    int _adpcmGroupPosL = 8, _adpcmGroupPosR = 8;

    // Wall-clock playback position -- same _elapsedMsFrozen/
    // _lastResumeMicros pattern MidiPlayer uses, deliberately decoupled
    // from _srcFramesRead (see elapsedMs()'s comment).
    uint32_t _elapsedMsFrozen = 0;
    uint32_t _lastResumeMicros = 0;

    // Linear-interpolation resample state, carried across update() calls.
    uint32_t _resampleStep = 0;  // 16.16 fixed-point, srcSampleRate/44100
    uint32_t _resampleFrac = 0;  // 16.16 fixed-point fractional position between _lastSrc/_nextSrc
    int16_t _lastSrcL = 0, _lastSrcR = 0;
    int16_t _nextSrcL = 0, _nextSrcR = 0;
    bool _havePair = false; // false until primeInterpolationPair() has read the first two source frames

    uint32_t totalSrcFrames() const;
    bool readSrcFrame(int16_t& outL, int16_t& outR); // dispatches to the raw-PCM or readAdpcmFrame() path, advances _srcFramesRead
    bool readAdpcmFrame(int16_t& outL, int16_t& outR); // IMA ADPCM block/nibble decode, see the field comments above
    void primeInterpolationPair(); // re-reads _lastSrc/_nextSrc right after load()/seekTo()
    void refillFromSource();       // one bounded batch: read -> convert -> resample -> Synth::wavStreamWrite()
};

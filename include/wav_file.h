#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "stream_player.h"

// Streams a RIFF/WAVE file directly off the SD card and feeds it to
// Synth's WAV playback stream (see Synth::wavStream*() in synth.h) --
// never buffers the whole file in RAM, same convention MidiPlayer uses
// for SMF playback. Unlike MidiPlayer, total duration is known immediately
// after load() (the `data` chunk's byte length gives it directly, no
// prescan).
//
// Supports PCM (8-bit unsigned, 16, 24 or 32-bit signed), 32-bit float
// and IMA ADPCM (4-bit, ~4:1 compressed), plain or WAVE_FORMAT_EXTENSIBLE.
// Not supported: Microsoft ADPCM or any other codec, 64-bit float, more
// than 2 channels. 24 and 32-bit samples play at their top 16 bits.
//
// The data chunk is read in blocks of about 1.5 KB into
// SharedRamArena::scratch (see fill()), which nothing else uses while a
// WAV plays. Core 0 only: load(), update() and seekTo() all read the card.
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
class WavPlayer : public StreamPlayer {
public:

    // Parses the RIFF/WAVE container, walking chunks until both 'fmt '
    // and 'data' are found (skipping anything else -- LIST/fact/JUNK/etc,
    // real-world files commonly carry extras). Leaves the player in
    // STATE_PAUSED (ready to play()) on success, or STATE_ERROR (see
    // errorMessage()) on failure. Does not touch Synth's stream at all --
    // that only starts once play() is first called (see play()'s comment).
    bool load(const char* path) override;

    // Closes the file handle and resets to STATE_IDLE. Also resets
    // Synth's WAV stream (see Synth::wavStreamReset()) so nothing from
    // this file keeps sounding after it's closed.
    void close() override;

    // Call every loop() iteration regardless of play/pause state -- keeps
    // Synth's ring buffer topped up (see synth.h's comment on why this is
    // deliberately not gated on STATE_PLAYING) by reading/converting/
    // resampling a bounded chunk from SD when there's room for it. Cheap
    // no-op when the buffer's already full or nothing's loaded.
    void update() override;

    void play() override;  // resume/start -- unmutes Synth's stream (see Synth::wavStreamSetActive())
    void pause() override; // hold in place, mutes Synth's stream without discarding buffered audio
    void stop() override;  // stop + close()

    // Position of what's being heard: frames core 1 has taken from the
    // ring since the last load or seek, so it stops during an underrun.
    uint32_t elapsedMs() const override;
    uint32_t totalMs() const override; // known immediately after load() -- no prescan needed

    // See this class's header comment for PCM-vs-ADPCM cost. Either way,
    // no file reopen. Discards whatever's currently buffered
    // (Synth::wavStreamReset()) since it no longer corresponds to the new
    // position; update() refills it on subsequent ticks. Clamped to
    // [0, totalMs()]. Preserves whether playback was PLAYING or PAUSED;
    // lands on STATE_DONE instead if the target is at/past the end.
    void seekTo(uint32_t targetMs) override;

    uint32_t sampleRate() const override { return _srcSampleRate; }
    uint8_t channels() const override { return _srcChannels; }
    uint8_t bitsPerSample() const { return _srcBitsPerSample; } // 4 for ADPCM

    // Samples core 1 found the ring empty since load(), from
    // Synth::wavStreamUnderrunSamples().
    uint32_t underrunSamples() const override;

    // Convert: after load(), rewindSource() goes back to the first frame and
    // readSource() reads source frames at the file's own rate, with no
    // resampling and no WAV stream, interleaved L/R (a mono file gives L ==
    // R). Returns the frames read, 0 at the end. readFailed() tells a card
    // error from the end. Don't play() while using these.
    void rewindSource();
    size_t readSource(int16_t* out, size_t maxFrames);
    bool readFailed() const { return _readFailed; }
    uint32_t sourceFramesRead() const { return _srcFramesRead; }
    uint32_t sourceFrames() const { return _totalSrcFrames; }
    // True for 16-bit PCM, the format convert writes.
    bool isPcm16() const { return !_isAdpcm && !_isFloat && _srcBitsPerSample == 16; }

    const char* typeLabel() const override { return "WAV"; }
    void formatText(char* out, size_t size) const override; // "16BIT", "4BIT" for ADPCM

#ifdef TRACKER_PROFILE
    // Card reads since the profile run last cleared it (tracker_profile.cpp).
    struct Profile { uint32_t reads, readBytes, readUs; };
    static Profile profile;
#endif

private:
    FsFile _file;

    uint32_t _dataChunkStart = 0;
    uint32_t _dataChunkLen = 0;
    uint32_t _srcSampleRate = 0;
    uint8_t _srcChannels = 0;
    uint8_t _srcBitsPerSample = 0;
    uint32_t _bytesPerSrcFrame = 0; // channels * (bitsPerSample/8) -- PCM only, meaningless (0) for ADPCM
    uint32_t _srcFramesRead = 0;    // read cursor, in source frames from data start
    uint32_t _totalSrcFrames = 0;
    bool _isFloat = false;          // 32-bit IEEE float samples
    bool _sourceEnded = false;      // wavStreamEnd() already signaled for this position
    bool _readFailed = false;       // fill() got a card error

    // Read buffer state, offsets into SharedRamArena::scratch.bytes, and the
    // file offsets of the next byte to read and of the data chunk's end.
    uint32_t _bufPos = 0, _bufLen = 0;
    uint32_t _filePos = 0, _dataEnd = 0;

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

    // elapsedMs() is _startMs plus the frames played since the ring held
    // _ringBase frames written (the last load or seek).
    uint32_t _startMs = 0;
    uint32_t _ringBase = 0;
    uint32_t _underrunBase = 0;

    // Linear-interpolation resample state, carried across update() calls.
    uint32_t _resampleStep = 0;  // 16.16 fixed-point, srcSampleRate/44100
    uint32_t _resampleFrac = 0;  // 16.16 fixed-point fractional position between _lastSrc/_nextSrc
    int16_t _lastSrcL = 0, _lastSrcR = 0;
    int16_t _nextSrcL = 0, _nextSrcR = 0;
    bool _havePair = false; // false until primeInterpolationPair() has read the first two source frames

    bool fail(const char* msg);            // closes the file, sets STATE_ERROR and the message
    void seekSource(uint32_t byteOffset);  // moves the read cursor in the data chunk, empties the buffer
    void restartClock(uint32_t ms);        // elapsedMs() counts from ms and the ring's current write count
    bool fill(uint32_t n);                 // makes n (<= 16) bytes ready at _bufPos; false at the end of data
    int16_t pcmSample(const uint8_t* p) const;
    bool ringDrained() const;
    bool readSrcFrame(int16_t& outL, int16_t& outR); // dispatches to the raw-PCM or readAdpcmFrame() path, advances _srcFramesRead
    bool readAdpcmFrame(int16_t& outL, int16_t& outR); // IMA ADPCM block/nibble decode, see the field comments above
    void primeInterpolationPair(); // re-reads _lastSrc/_nextSrc right after load()/seekTo()
    void refillFromSource();       // one bounded batch: read -> convert -> resample -> Synth::wavStreamWrite()
};

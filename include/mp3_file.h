#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Plays an MP3 file off the card into Synth's WAV stream (the same ring
// WavPlayer feeds), decoded with the Helix fixed-point decoder in
// lib/helix_mp3. Trial code for the profile build: play/stop/pause only,
// no seek.
//
// MPEG 1/2/2.5 layer III, mono or stereo, CBR or VBR. A leading ID3v2 tag
// is skipped, a Xing/Info header frame gives the length and isn't played,
// a trailing ID3v1 tag isn't read. Rates other than SAMPLE_RATE are
// resampled linearly, as WavPlayer does.
//
// Uses the start of SharedRamArena::data() for the decoder state, the
// compressed input ring and one decoded frame (see arenaBytes()), so nothing
// else may hold the arena while an MP3 is loaded. Only one Mp3Player may be
// loaded at a time, and nothing else may feed the WAV stream meanwhile.
//
// Two cores: core 0 calls load(), update(), play(), pause() and close(),
// and is the only one that reads the card. update() tops up the input ring.
// Core 1 calls core1Push() and core1Decode() from loop1(): they decode
// from the input ring into the WAV stream, so core 1 is the WAV stream's
// producer while an MP3 is loaded. close() waits for core 1 to leave them
// before it frees the decoder.
class Mp3Player {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    bool load(const char* path); // STATE_PAUSED on success, STATE_ERROR otherwise
    void close();
    void update(); // call every loop
    void play();
    void pause();
    void stop() { close(); }

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t elapsedMs() const;
    uint32_t totalMs() const { return _totalMs; } // from the Xing header, else estimated from the first frame's bitrate
    uint32_t sampleRate() const { return _rate; }
    uint8_t channels() const { return _channels; }
    uint32_t bitrate() const { return _bitrate; } // first audio frame's, in bits/s
    uint8_t mpegVersion() const { return _version; } // 1, 2 or 25 (2.5)
    const char* vbrTag() const { return _vbrTag; } // "Xing", "Info", "VBRI" or "" when there's none
    uint32_t id3Bytes() const { return _id3Bytes; }
    uint32_t samplesPerFrame() const { return _samplesPerFrame; }
    uint32_t underrunSamples() const;

    // Arena bytes used: decoder state, input ring, one decoded frame.
    static size_t decoderBytes();
    static size_t arenaBytes();

    // Core 1, between samples. core1Push() resamples decoded audio into the
    // WAV stream and returns true when the next frame is due and its input
    // is in the ring. core1Decode() then decodes that frame, which takes up
    // to about 26 ms on the device, so call it only when the I2S buffers
    // hold more than that. Both return at once when no MP3 is loaded.
    static bool core1Push();
    static void core1Decode();

#ifdef TRACKER_PROFILE
    // Card reads (core 0) and decoding (core 1) since the last call.
    // starved counts the times core 1 had room for a frame but the input
    // ring didn't hold one yet.
    struct Profile { uint32_t reads, readBytes, readUs, frames, decodeUs, decodeWorstUs, errors, starved; };
    static Profile profileTake();
#endif

private:
    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};
    void* _dec = nullptr; // HMP3Decoder

    uint32_t _rate = 0, _bitrate = 0, _samplesPerFrame = 0, _id3Bytes = 0, _totalMs = 0;
    uint8_t _channels = 0, _version = 0;
    char _vbrTag[5] = {0};

    // load()'s linear scan buffer (the start of the input ring's bytes):
    // bytes [_inPos, _inLen) are unread.
    uint32_t _inPos = 0, _inLen = 0;
    uint32_t _filePos = 0, _dataEnd = 0;
    bool _eof = false;

    uint32_t _ringBase = 0, _underrunBase = 0;

    bool fail(const char* msg);
    bool fillScan();       // load() only: tops the scan buffer up; false at the end of data
    bool findFirstFrame(); // skips to the first frame, reads its format and any Xing/Info/VBRI header
    void fillRing();       // one card read into the input ring when it has room
    void stopCore1();      // waits until core 1 is out of core1Push()/core1Decode() and stays out
    bool ringDrained() const;
};

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
// Uses the start of SharedRamArena::data() for the decoder state, the card
// read buffer and one decoded frame (see arenaBytes()), so nothing else may
// hold the arena while an MP3 is loaded. Core 0 only: load() and update()
// read the card, and update() decodes at most one frame per call.
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

    // Arena bytes used: decoder state, read buffer, one decoded frame.
    static size_t decoderBytes();
    static size_t arenaBytes();

#ifdef TRACKER_PROFILE
    // Card reads and decode time since the profile run last cleared it.
    struct Profile { uint32_t reads, readBytes, readUs, frames, decodeUs, decodeWorstUs, errors; };
    static Profile profile;
#endif

private:
    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};
    void* _dec = nullptr; // HMP3Decoder
    uint8_t* _in = nullptr;   // read buffer and decoded frame, in the arena
    int16_t* _pcm = nullptr;

    uint32_t _rate = 0, _bitrate = 0, _samplesPerFrame = 0, _id3Bytes = 0, _totalMs = 0;
    uint8_t _channels = 0, _version = 0;
    char _vbrTag[5] = {0};

    // Read buffer (in the arena): bytes [_inPos, _inLen) are unread.
    uint32_t _inPos = 0, _inLen = 0;
    uint32_t _filePos = 0, _dataEnd = 0;
    bool _eof = false;

    // Decoded frame (in the arena), _pcmFrames stereo or mono frames, _pcmPos next.
    uint32_t _pcmFrames = 0, _pcmPos = 0;
    uint8_t _pcmChannels = 0;
    bool _sourceEnded = false;

    // Linear resampler, as WavPlayer's.
    uint32_t _resampleStep = 0, _resampleFrac = 0;
    int16_t _lastL = 0, _lastR = 0, _nextL = 0, _nextR = 0;
    bool _haveLast = false, _needNext = true;

    uint32_t _ringBase = 0, _underrunBase = 0;

    bool fail(const char* msg);
    uint8_t* inBuf() const;
    int16_t* pcmBuf() const;
    bool fillInput();     // tops the read buffer up to a whole frame or more; false at the end of data
    bool findFirstFrame(); // skips to the first frame, reads its format and any Xing/Info/VBRI header
    bool decodeFrame();   // one frame into pcmBuf(); false when the data has run out
    bool takeSrc(int16_t& l, int16_t& r);
    void refill();
    bool ringDrained() const;
};

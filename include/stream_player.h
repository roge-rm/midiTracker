#pragma once
#include <stdint.h>
#include <stddef.h>

// What the file player's audio screen needs from a file that plays through
// Synth's WAV stream: WavPlayer, Mp3Player and FlacPlayer. Core 0 only.
class StreamPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    virtual ~StreamPlayer() {}

    virtual bool load(const char* path) = 0; // STATE_PAUSED on success, STATE_ERROR otherwise
    virtual void close() = 0;
    virtual void update() = 0; // every loop
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0; // stop and close()
    // Clamped to [0, totalMs()]. Keeps playing or paused, STATE_DONE at the end.
    virtual void seekTo(uint32_t targetMs) = 0;

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }
    virtual uint32_t elapsedMs() const = 0;
    virtual uint32_t totalMs() const = 0;
    uint32_t pathKey() const { return _pathKey; } // hash of the loaded path, for the UI's waveform cache
    virtual uint32_t underrunSamples() const = 0;

    // For the player screen: "WAV", "MP3" or "FLAC", and the format line after the
    // sample rate, e.g. "16BIT" or "320K CBR".
    virtual const char* typeLabel() const = 0;
    virtual void formatText(char* out, size_t size) const = 0;
    virtual uint32_t sampleRate() const = 0;
    virtual uint8_t channels() const = 0;

protected:
    State _state = STATE_IDLE;
    char _error[64] = {0};
    uint32_t _pathKey = 0;

    void hashPath(const char* path) {
        _pathKey = 2166136261u;
        for (const char* c = path; c && *c; c++) _pathKey = (_pathKey ^ (uint8_t)*c) * 16777619u;
    }
};

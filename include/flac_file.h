#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "stream_player.h"

// Plays a FLAC file off the card into Synth's WAV stream (the same ring
// WavPlayer and Mp3Player feed), decoded with dr_flac in lib/dr_flac.
//
// Native FLAC (not Ogg), 4 to 24 bits, mono or stereo, 4 to 192 kHz; the
// output is 16-bit (24-bit drops its low 8 bits, 8-bit moves up 8) and
// rates other than SAMPLE_RATE are resampled linearly, as WavPlayer does.
// ID3v2 tags before the stream are skipped. load() refuses more than two
// channels, 32-bit files and files whose largest block (STREAMINFO) won't
// fit the arena with its input ring. A seek table that doesn't fit is left
// out and seeks use dr_flac's binary search.
//
// Uses SharedRamArena::data() for the decoder (dr_flac's state, one
// decoded block as 32-bit samples and the seek table, see arenaUsed()),
// the compressed input ring and one chunk of 16-bit output, so nothing
// else may hold the arena while a FLAC is loaded. Only one FlacPlayer or
// Mp3Player may be loaded at a time, and nothing else may feed the WAV
// stream meanwhile.
//
// Two cores, as Mp3Player: core 0 calls everything public except the
// core1*() functions and is the only one that reads the card; update()
// tops up the input ring. Core 1 calls core1Push() and core1Decode() from
// loop1(), which decode from the ring into the WAV stream. The decoder
// only starts a FLAC frame once the ring holds the largest frame the
// STREAMINFO allows. seekTo() and close() wait for core 1 to leave first;
// seekTo() then runs dr_flac's seek on core 0 straight off the card
// (seek table, else binary search; sample exact), which takes a few frame
// decodes. A file whose STREAMINFO has no length only seeks to the start.
// Frames that don't decode are skipped; 32 in a row end the file.
class FlacPlayer : public StreamPlayer {
public:
    bool load(const char* path) override;
    void close() override;
    void update() override;
    void play() override;
    void pause() override;
    void stop() override { close(); }
    void seekTo(uint32_t targetMs) override;

    // Puts the player in STATE_ERROR with msg, for when load() can't be
    // called (the arena is in use).
    void refuse(const char* msg);

    uint32_t elapsedMs() const override;
    // From STREAMINFO's sample count; when that's 0 (unknown), estimated
    // from the file size and grown as it plays.
    uint32_t totalMs() const override { return _totalMs; }
    uint32_t sampleRate() const override { return _rate; }
    uint8_t channels() const override { return _channels; }
    uint32_t underrunSamples() const override;
    const char* typeLabel() const override { return "FLAC"; }
    void formatText(char* out, size_t size) const override; // "16BIT", "24BIT"

    uint8_t bitsPerSample() const { return _bits; }
    uint16_t maxBlock() const { return _maxBlock; }
    uint32_t maxFrameBytes() const { return _maxFrame; } // 0 when STREAMINFO doesn't say
    uint32_t seekPoints() const;                         // in use, 0 when none or left out
    bool seekTableDropped() const { return _hideSeekTable; }
    size_t arenaUsed() const { return _arenaUsed; }      // after load(): decoder, ring and output chunk
    size_t ringBytes() const { return _ringBytes; }

    // Convert: loads as load() does but leaves core 1 and the WAV stream
    // alone, and reads the card directly. decodeNext() then decodes on the
    // calling core and points pcm at up to 1152 frames: at most one FLAC
    // frame per call. Returns the frames, 0 for a frame that didn't decode
    // (it moves on to the next), -1 at the end of the data;
    // readFailed() tells a card error from the end. Close with close().
    // Core 0 only, never while another FlacPlayer or Mp3Player is loaded.
    bool openOffline(const char* path);
    // The rate, channels and bits from STREAMINFO, without the decoder or
    // the arena. False when it isn't a FLAC this player takes. Core 0, any time.
    static bool probe(const char* path, uint32_t& rate, uint8_t& channels, uint8_t& bits);
    int decodeNext(const int16_t*& pcm, uint8_t& channels);
    bool readFailed() const;
    // Bytes of audio data read so far and in all, for a progress bar.
    void inputProgress(uint32_t& done, uint32_t& total) const;

    // Core 1, between samples. core1Push() resamples decoded audio into the
    // WAV stream and returns true when the next chunk is due and the ring
    // holds what it needs. core1Decode() then decodes it, which takes one
    // FLAC frame's decode when it starts a new frame, so call it only when
    // the I2S buffers hold more than that. Both return at once when no
    // FLAC is loaded.
    static bool core1Push();
    static void core1Decode();
    // True while a FLAC is loaded and core 1 feeds it (any core may ask).
    // Off during a seek.
    static bool core1Active();

#ifdef TRACKER_PROFILE
    // As Mp3Player::Profile. frames counts chunks that started a FLAC
    // frame, and decodeUs and decodeWorstUs time those; rejects counts
    // chunks that gave nothing before the end.
    struct Profile { uint32_t reads, readBytes, readUs, frames, decodeUs, decodeWorstUs, rejects, starved; };
    static Profile profileTake();
#endif

private:
    FsFile _file;

    uint32_t _rate = 0, _totalMs = 0, _maxFrame = 0;
    uint64_t _totalFrames = 0;
    uint16_t _maxBlock = 0;
    uint8_t _channels = 0, _bits = 0;
    bool _offline = false;
    bool _hideSeekTable = false;
    size_t _arenaUsed = 0, _ringBytes = 0;

    // File offsets: the "fLaC" marker (dr_flac's offset 0), the first
    // frame and the end of the file.
    uint32_t _base = 0, _audioStart = 0, _fileEnd = 0;
    uint32_t _filePos = 0; // the next byte fillRing() reads
    bool _eof = false;

    // elapsedMs() is _startMs plus the frames played since the ring held
    // _ringBase frames written (the last load or seek).
    uint32_t _startMs = 0, _ringBase = 0, _underrunBase = 0;
    // Only when the length is an estimate: update() grows _totalMs once the
    // ring has played _growAt frames since _ringBase.
    uint32_t _growAt = 0xFFFFFFFFu;

    bool fail(const char* msg);
    bool open(const char* path, bool offline);
    void startRing();      // input ring from where dr_flac stopped reading, core 1 stopped
    void fillRing();       // one card read into the input ring when it has room
    void stopCore1();      // waits until core 1 is out of core1Push()/core1Decode() and stays out
    void restartClock(uint32_t ms);
    bool ringDrained() const;
};

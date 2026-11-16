#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "stream_player.h"

// Plays an MP3 file off the card into Synth's WAV stream (the same ring
// WavPlayer feeds), decoded with the Helix fixed-point decoder in
// lib/helix_mp3.
//
// MPEG 1/2/2.5 layer III, mono or stereo, CBR or VBR. Layer I/II and free
// format files are refused at load. A leading ID3v2 tag is skipped, a
// Xing/Info or VBRI header frame gives the length and seek table and isn't
// played, a trailing ID3v1 tag isn't read. A LAME tag in the Xing/Info
// frame gives the encoder delay and padding, which are trimmed as ffmpeg
// does (the delay plus 529 at the start, the padding at the end), so the
// clock and length cover only the music; without one nothing is trimmed. Rates other than SAMPLE_RATE are
// resampled linearly, as WavPlayer does.
//
// seekTo() finds the byte position from the Xing TOC or the VBRI table when
// there's one, else in proportion to the length, then looks for two or
// three frame headers in a row, 16 KB at a time, up to 256 KB ahead, then
// back from there towards the first frame. The first 16 KB is searched in
// seekTo(), the rest one piece per update(). The decoder starts fresh there,
// so the frames whose bit reservoir is missing give no audio, and the first
// frame that decodes is dropped too; the clock counts those frames.
//
// Uses the start of SharedRamArena::data() for the decoder state, the
// compressed input ring and one decoded frame (see arenaBytes()), so nothing
// else may hold the arena while an MP3 is loaded. Only one Mp3Player may be
// loaded at a time, and nothing else may feed the WAV stream meanwhile.
//
// Two cores: core 0 calls everything public except the core1*() functions,
// and is the only one that reads the card. update() tops up the input ring.
// Core 1 calls core1Push() and core1Decode() from loop1(): they decode
// from the input ring into the WAV stream, so core 1 is the WAV stream's
// producer while an MP3 is loaded. seekTo() and close() wait for core 1 to
// leave them first.
struct Mp3OtherFrames; // mp3_file.cpp

class Mp3Player : public StreamPlayer {
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
    // From the Xing/VBRI header, else estimated from the bitrate. Grows to
    // the elapsed time if playing runs past an estimate.
    uint32_t totalMs() const override { return _totalMs; }
    uint32_t sampleRate() const override { return _rate; }
    uint8_t channels() const override { return _channels; }
    uint32_t underrunSamples() const override;
    const char* typeLabel() const override { return "MP3"; }
    void formatText(char* out, size_t size) const override; // "320K CBR", "245K VBR"

    uint32_t bitrate() const { return _bitrate; } // bits/s: the average for VBR, else the first audio frame's
    bool isVbr() const { return _vbr; }
    uint8_t mpegVersion() const { return _version; } // 1, 2 or 25 (2.5)
    const char* vbrTag() const { return _vbrTag; } // "Xing", "Info", "VBRI" or "" when there's none
    uint32_t id3Bytes() const { return _id3Bytes; }
    uint32_t samplesPerFrame() const { return _samplesPerFrame; }
    // From a LAME tag, in samples: false when there's none and nothing is trimmed.
    bool isGapless() const { return _gapless; }
    uint32_t encoderDelay() const { return _encDelay; }
    uint32_t encoderPadding() const { return _encPad; }

    // Convert: loads as load() does but leaves core 1 and the WAV stream
    // alone. decodeNext() then decodes the next frame on the calling core
    // (about 9 ms on the device) and points pcm at it: returns its frames,
    // 0 for a frame with no audio (bit reservoir filling, junk), -1 at the
    // end of the data. readFailed() tells a card error from the end. Close
    // with close(). Core 0 only, never while another Mp3Player is loaded.
    bool openOffline(const char* path);
    // The rate and channels of the first frames, without the decoder or
    // the arena: skips ID3v2 tags, then looks for three layer III frame
    // headers in a row in the first 64 KB. Core 0, any time.
    static bool probe(const char* path, uint32_t& rate, uint8_t& channels);
    int decodeNext(const int16_t*& pcm, uint8_t& channels);
    bool readFailed() const { return _readFailed; }
    // Bytes of audio data read so far and in all, for a progress bar.
    void inputProgress(uint32_t& done, uint32_t& total) const;

    // Arena bytes used: decoder state, input ring, one decoded frame.
    static size_t decoderBytes();
    static size_t arenaBytes();

    // Core 1, between samples. core1Push() resamples decoded audio into the
    // WAV stream and returns true when the next frame is due and its input
    // is in the ring. core1Decode() then decodes that frame, which takes up
    // to about 10 ms on the device (320 kbps stereo), so call it only when
    // the I2S buffers hold more than that. Both return at once when no MP3
    // is loaded.
    static bool core1Push();
    static void core1Decode();
    // True while an MP3 is loaded and core 1 feeds it (any core may ask).
    // Off for a moment during a seek.
    static bool core1Active();

#ifdef TRACKER_PROFILE
    // Card reads (core 0) and decoding (core 1) since the last call.
    // frames, decodeUs and decodeWorstUs cover the frames that decoded,
    // rejects counts the decoder calls that gave no audio (bit reservoir
    // still filling, bad or cut frames). starved counts the times core 1
    // had room for a frame but the input ring didn't hold one yet.
    struct Profile { uint32_t reads, readBytes, readUs, frames, decodeUs, decodeWorstUs, rejects, starved; };
    static Profile profileTake();
#endif

private:
    FsFile _file;
    void* _dec = nullptr; // HMP3Decoder

    uint32_t _rate = 0, _bitrate = 0, _samplesPerFrame = 0, _id3Bytes = 0, _totalMs = 0;
    uint8_t _channels = 0, _version = 0;
    bool _vbr = false;
    bool _offline = false;    // loaded by openOffline()
    bool _readFailed = false; // fillRing() got a card error
    char _vbrTag[5] = {0};

    // Seek tables. _tagFrame is the file offset of the Xing/Info/VBRI
    // frame, which both tables count from, _audioStart that of the first
    // audio frame. The VBRI table stays on the card and is read at a seek.
    uint32_t _tagFrame = 0, _audioStart = 0, _tagBytes = 0;
    uint32_t _tagMs = 0; // the length the header gives, which the TOC is in proportion to
    // Gapless playback, from the LAME tag: the encoder delay and padding,
    // the Xing frame count and the delay as the clock's offset from the
    // first frame. See setTrim().
    bool _gapless = false;
    uint32_t _encDelay = 0, _encPad = 0, _xingFrames = 0, _skipMs = 0;
    // After a seek the end trim counts frames back from the end of the
    // data: _endPos holds the file offsets of the last _endCount frames,
    // read once at the first seek (_endScanned). _endCount is 0 when they
    // couldn't be found or the file is cut short.
    static const int END_FRAMES = 8;
    uint32_t _endPos[END_FRAMES];
    uint8_t _endCount = 0;
    bool _endScanned = false;
    bool _hasToc = false;
    uint8_t _toc[100];
    uint32_t _vbriTable = 0; // file offset, 0 when there's none
    uint16_t _vbriEntries = 0, _vbriScale = 0, _vbriEntryBytes = 0, _vbriFramesPerEntry = 0;

    // load()'s and seekTo()'s linear scan buffer (the start of the input
    // ring's bytes): bytes [_inPos, _inLen) are unread.
    uint32_t _inPos = 0, _inLen = 0;
    uint32_t _filePos = 0, _dataEnd = 0;
    bool _eof = false;

    // elapsedMs() is _startMs, plus the frames core 1 decoded without
    // output since then (g_sh.lostMs), plus the frames played since the ring
    // held _ringBase frames written (the last load or seek).
    uint32_t _startMs = 0, _ringBase = 0, _underrunBase = 0;
    // update() grows _totalMs once the ring has played _growAt frames since
    // _ringBase, worked out for a lost time of _growLost ms.
    uint32_t _growAt = 0, _growLost = 0;

    // A seek still looking for frames, one SEEK_CHUNK per update(): forward
    // from _seekOrigin while _seekBack is 0, then back from it in steps.
    // Core 1 stays out meanwhile.
    bool _seekPending = false;
    uint32_t _seekOrigin = 0, _seekNext = 0, _seekBack = 0;

    bool fail(const char* msg);
    bool open(const char* path, bool offline);
    bool fillScan();       // tops the scan buffer up; false at the end of data
    // Moves _inPos to two or three layer III frames in a row at rate (any
    // rate when 0), within maxBytes. other, when given, counts the layer
    // I/II and free format frames seen on the way.
    bool syncScan(uint32_t maxBytes, uint32_t rate, Mp3OtherFrames* other);
    bool findFirstFrame(); // skips to the first frame, reads its format and any Xing/Info/VBRI header
    bool testDecode();     // load() only: decodes the first few frames on core 0
    uint32_t seekOffset(uint32_t ms); // file offset to look for a frame from
    uint32_t offsetMs(uint32_t pos);  // about the reverse of seekOffset()
    // Looks for frames in maxBytes from pos; found is the first one's offset.
    bool findSync(uint32_t pos, uint32_t maxBytes, uint32_t& found);
    void scanEnd();        // fills _endPos, core 1 stopped
    bool startStream(uint32_t pos);   // input ring from the frame at pos, core 1 stopped
    void seekStep();       // one chunk of a pending seek
    void startAt(uint32_t pos, uint32_t ms); // ends a seek: plays from the frame at pos, the clock at ms
    void growTotal(uint32_t played, uint32_t lost);
    void fillRing();       // one card read into the input ring when it has room
    void stopCore1();      // waits until core 1 is out of core1Push()/core1Decode() and stays out
    void restartClock(uint32_t ms);
    bool ringDrained() const;
    // Core 1's trim for a stream whose next frame starts at sample srcPos
    // (from the first audio frame's start, the trimmed delay included).
    void setTrim(uint32_t srcPos);
};

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <SdFat.h>
#include "sd_browser.h" // FileKind

class WavPlayer;
class Mp3Player;

// File conversion from the browser: a file goes through a ConvertSource,
// which gives 16-bit stereo frames at its own rate, and Converter writes
// them out as a 16-bit WAV next to the file. Core 0 only. Converter works
// in bounded steps (step()), so the UI keeps running between them.
//
// Adding a kind (MIDI or a module to WAV): a ConvertSource that renders
// it a block per read(), its targets in convertTargets(), and a case in
// the file player's convertSourceFor(). A source with usesArena() goes
// through the looper's free-tracks gate first.

// What a file can be turned into.
enum ConvertTarget : uint8_t {
    CONVERT_WAV,            // 16-bit at the source's own rate and channels
    CONVERT_WAV_44K_STEREO, // 16-bit 44.1 kHz stereo
    CONVERT_WAV_44K_MONO,   // 16-bit 44.1 kHz mono
};
const char* convertTargetLabel(ConvertTarget t);

// The source format, as far as the target list needs it.
struct ConvertFormat {
    uint32_t rate = 0;
    uint8_t channels = 0;
    bool pcm16 = false;
};

// Reads the format of a file of `kind` into fmt: the WAV header (through
// wav, which must be idle) or the first MP3 frame headers. False when the
// file can't be read or the kind needs nothing.
bool convertProbe(FileKind kind, const char* path, WavPlayer& wav, ConvertFormat& fmt);

// The targets for a file of `kind`, fmt from convertProbe() or null when
// that failed. Two targets that would make the same file aren't both
// listed. Fills out (up to max) and returns how many.
int convertTargets(FileKind kind, const ConvertFormat* fmt, ConvertTarget* out, int max);

// The target list's row for t: title ("WAV 48K STEREO", Silkscreen at
// double size, 19 characters at most) and note ("16-bit · file is 44.1k
// mono"). fmt from convertProbe() or null.
void convertTargetText(ConvertTarget t, const ConvertFormat* fmt, char* title, size_t titleSize, char* note, size_t noteSize);

// Where the frames come from. read() is called from Converter::step() and
// should do a bounded piece of work each call (a block or one MP3 frame).
class ConvertSource {
public:
    virtual ~ConvertSource() {}
    // Opens path. False with error() set.
    virtual bool open(const char* path) = 0;
    virtual const char* error() const = 0;
    virtual uint32_t rate() const = 0;
    virtual uint8_t channels() const = 0; // 1 or 2, what CONVERT_WAV writes
    // Up to max frames into out, always stereo (L, R). Returns the frames,
    // 0 for none this call with more to come, READ_END or READ_FAILED.
    static const int READ_END = -1, READ_FAILED = -2;
    virtual int read(int16_t* out, int max) = 0;
    // Work done so far and in all, in any unit, for the bar and time left.
    virtual void progress(uint32_t& done, uint32_t& total) const = 0;
    virtual void close() = 0;
    // Holds SharedRamArena while open, so the looper's tracks must be freed first.
    virtual bool usesArena() const = 0;
};

class WavConvertSource : public ConvertSource {
public:
    explicit WavConvertSource(WavPlayer& p) : _p(p) {}
    bool open(const char* path) override;
    const char* error() const override;
    uint32_t rate() const override;
    uint8_t channels() const override;
    int read(int16_t* out, int max) override;
    void progress(uint32_t& done, uint32_t& total) const override;
    void close() override;
    bool usesArena() const override { return false; }

private:
    WavPlayer& _p;
};

// Decodes on the calling core, one frame per read() call that needs one
// (about 9 ms on the device), see Mp3Player::decodeNext().
class Mp3ConvertSource : public ConvertSource {
public:
    explicit Mp3ConvertSource(Mp3Player& p) : _p(p) {}
    bool open(const char* path) override;
    const char* error() const override;
    uint32_t rate() const override;
    uint8_t channels() const override;
    int read(int16_t* out, int max) override;
    void progress(uint32_t& done, uint32_t& total) const override;
    void close() override;
    bool usesArena() const override { return true; }

private:
    Mp3Player& _p;
    const int16_t* _pcm = nullptr;
    int _frames = 0, _pos = 0;
    uint8_t _pcmChannels = 2;
};

// Leftover temp output from a power cut: ".NAME.wav.cvt", the leaf name.
// Deleted when the folder is listed, see SdBrowser::sawTempFiles().
bool isConvertTempName(const char* leaf);

// One conversion. Output goes to a hidden temp file next to the source
// (".NAME.wav.cvt"), renamed to a free name at the end: Song.mp3 gives
// Song.wav, or Song-2.wav, Song-3.wav when taken. The source is only read.
// On a failure or cancel the temp file is removed.
//
// When the rates differ the audio goes through a windowed-sinc low-pass
// (Kaiser, 32 zero crossings each side, cut off at 0.46 of the lower rate)
// evaluated at each output frame's source position, so downsampling
// doesn't alias. Its cost grows with the source rate over the output's:
// about 70 taps per output frame from 48 kHz to 44.1k, 150 from 96 kHz.
class Converter {
public:
    ~Converter();
    enum State : uint8_t { STATE_IDLE, STATE_RUNNING, STATE_DONE, STATE_FAILED, STATE_CANCELLED };

    // How long step() works by default. A step can run over by one read()
    // and one card write.
    static const uint32_t STEP_US = 10000;
    // Bytes per card write, whole sectors.
    static const size_t OUT_BYTES = 2048;

    // Starts converting srcPath (as SdBrowser builds it) through src, which
    // must not be open. displayName is the browser's name for the file, the
    // new name comes from it. False with state STATE_FAILED and message().
    bool begin(ConvertSource& src, const char* srcPath, const char* displayName, ConvertTarget target);
    // One bounded piece of work. Ends with finish() at the end of the source.
    void step(uint32_t budgetUs = STEP_US);
    // Fixes the header, syncs and renames the temp file. Called by step().
    bool finish();
    // Stops, removes the temp file. State STATE_CANCELLED.
    void cancel();
    // STATE_FAILED with msg without starting, for when begin() can't be
    // called (the arena is in use).
    void refuse(ConvertTarget target, const char* msg);

    State state() const { return _state; }
    ConvertTarget target() const { return _target; }
    const char* message() const { return _msg; } // a one-clause failure or "Cancelled"
    const char* outputName() const;               // leaf name of the new file
    uint32_t elapsedMs() const;
    // Rough time left from the rate so far, UINT32_MAX until there's enough to go on.
    uint32_t leftMs() const;
    int progressPermille() const;
    uint32_t outRate() const { return _outRate; }
    uint8_t outChannels() const { return _outChannels; }

private:
    ConvertSource* _src = nullptr;
    State _state = STATE_IDLE;
    ConvertTarget _target = CONVERT_WAV;
    char _msg[40] = {0};
    char _tmpPath[232] = {0};
    char _outPath[232] = {0};
    FsFile _out;
    uint32_t _startMs = 0, _endMs = 0;
    uint32_t _outRate = 0;
    uint8_t _outChannels = 0;
    uint32_t _dataBytes = 0; // audio bytes written or in _buf
    int _permille = 0;

    // Card writes go out OUT_BYTES at a time from here, the first one
    // starting with the 44-byte header.
    uint8_t _buf[OUT_BYTES];
    size_t _bufLen = 0;

    // Resampling, see feed(). Source frames go into _ring (mono when the
    // output is), each at slot (index & _ringMask) and again R slots on, so
    // the taps around any position read in one piece. The output position
    // is _pos0 (index of the frame at or before it) plus _frac (32-bit
    // fraction); _stepInt/_stepFrac are source frames per output frame.
    // Indices count _pad silent frames before the source's first.
    bool _resample = false;
    int16_t* _ring = nullptr;      // heap, 2 * R frames
    uint32_t _ringMask = 0;
    int _taps = 0;                 // most taps on each side of the position
    uint32_t _kStep = 0;           // kernel table steps per source frame, 16.16
    uint32_t _gain = 0;            // the low-pass's gain, 16.16
    uint32_t _stepInt = 0, _stepFrac = 0, _frac = 0;
    uint32_t _pos0 = 0;            // source index at or before the next output
    uint32_t _next = 0;            // source index the next frame in goes to
    uint32_t _pad = 0;
    uint32_t _taken = 0;           // real source frames taken

    // The output's leaf name: _stem, a "-N" suffix past 1, ".wav".
    char _stem[41] = {0};
    int _dirLen = 0;

    void fail(const char* msg);
    void closeSource();
    // Sets _stem and _dirLen from the source. False when the folder's path is too long.
    bool setStem(const char* srcPath, const char* displayName);
    // The first free name from _stem into _outPath and its temp into
    // _tmpPath. Null, or the reason there isn't one.
    const char* pickNames();
    void writeHeader(uint8_t* h, uint32_t dataBytes) const;
    bool putFrame(int32_t l, int32_t r); // to _buf, flushing when full
    bool flush();
    // Feeds n source frames; returns false on a write failure.
    bool feed(const int16_t* in, int n);
    bool drainEnd(); // the last outputs past the source's end
    bool pushIn(int16_t l, int16_t r); // one frame into the ring, then the outputs it completes
    bool emitOne();
};

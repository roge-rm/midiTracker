#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Path buffers for both recorders, big enough for any path the file
// player builds (its own buffers are 224).
const size_t RECORDER_PATH_MAX = 224;

// Builds the temp name a replacing take records into: "/dir/name.mid"
// becomes "/dir/.name.mid.part". The leading dot keeps it out of the
// browser and out of the REC###/DUMP### numbering. False if it won't fit.
bool recorderTempPath(const char* path, char* out, size_t outSize);

// Next free REC###.mid (DUMP###.syx for a .syx path) in `path`'s folder.
bool recorderFreeName(const char* path, char* out, size_t outSize);

// Renames a closed take `tmp` to `path`, removing the old file first
// unless `keepOld`. If `path` stays taken, uses recorderFreeName() and
// puts that leaf name in `savedAs` ("" when it went to `path`). False if
// the take is still at `tmp`.
bool recorderPlaceTake(const char* tmp, const char* path, bool keepOld, char* savedAs, size_t savedAsSize);

// Shared end of both recorders' finish(): places a closed take written to
// `written` and sets `err` for anything other than a clean save. With a
// write or sync failure a replacing take keeps the old file. True only
// for a clean save.
bool recorderSettleTake(const char* written, const char* path, bool replace, bool failed, bool synced,
                        const char* what, char* err, size_t errSize);

// Moves a leftover temp take (full path to a ".NAME.part") to NAME, or to
// a free name if NAME exists. Removes it if empty. False if it couldn't.
bool recorderRecoverTemp(const char* tmpPath);

// Streams incoming MIDI channel-voice messages straight to a Standard
// MIDI File (format 0, single track) on the SD card as they arrive,
// rather than buffering a take in RAM. Ticks are derived from a fixed
// synthetic 120 BPM tempo against wall-clock micros() -- we're recording
// real-time performance, not reading tempo off anything.
//
// Everything here runs on core 0: feed()/feedSysEx() from the MIDI input
// handler (MidiOutput::update(), called from the main loop and from the
// UI's draw pumps), service()/stop()/cancel() from the file player's
// update loop. The input side only writes into SdFat's 512-byte sector
// cache; a write still reaches the card when that sector fills (one
// sector write, plus a FAT update when a new cluster is needed). sync()
// and all failure handling happen in service().
class MidiRecorder {
public:
    enum State { STATE_IDLE, STATE_ARMED, STATE_RECORDING, STATE_ERROR };

    // Arms the recorder to record to `path`. Nothing is created on disk
    // yet -- see feed(). With `replace` false, fails if `path` already
    // exists. With `replace` true the take goes to a temp file next to it
    // (see recorderTempPath()) and only replaces `path` when stop() saves
    // it, so cancel() leaves the old file alone. Returns false (STATE_ERROR,
    // see errorMessage()) on failure.
    bool arm(const char* path, bool replace = false);

    // Feeds one incoming MIDI channel-voice event (status includes the
    // channel nibble; len is 1 for Program Change/Channel Pressure, 2
    // otherwise). No-op outside STATE_ARMED/STATE_RECORDING. The first
    // call after arm() creates the file and writes the MThd/MTrk/tempo
    // header before writing this event.
    void feed(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len);

    // Feeds one incoming System Exclusive message, embedded in the same
    // MTrk stream feed() writes to (as a native SMF SysEx event, so it
    // stays correctly timed against everything else in the take) rather
    // than a separate file. `data` must be boundary-inclusive (data[0]==
    // 0xF0, data[len-1]==0xF7) -- the same shape MidiOutput::SysExHandler
    // hands back. Same ARMED/RECORDING behavior as feed() otherwise,
    // including counting toward eventCount().
    void feedSysEx(const uint8_t* data, size_t len);

    // Call from the main loop while recording: syncs the file every few
    // seconds, and after a failed write or sync keeps what was good and
    // moves to STATE_ERROR.
    void service();

    // Writes the End-of-Track event, patches the final MTrk length, closes
    // the file and, for a replacing take, swaps it in for the old file.
    // No-op if nothing was ever fed (STATE_ARMED) or already idle. False
    // (STATE_ERROR) if the save failed; otherwise STATE_IDLE.
    bool stop();

    // Abandons the current recording instead of finalizing it: closes and
    // deletes the file if one was created. A replaced file is left as it
    // was. After STATE_ERROR the kept part stays. Resets to STATE_IDLE.
    void cancel();

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t eventCount() const { return _eventCount; }
    uint32_t elapsedMs() const; // 0 until the first event starts the clock

    // ms since the last event (of any kind -- feed() or feedSysEx()) was
    // fed, or UINT32_MAX if none has yet -- drives the UI's receive-
    // activity indicator (see Ui::drawRecording()), same as
    // SysExRecorder::msSinceLastMessage().
    uint32_t msSinceLastEvent() const;

private:
    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};
    char _path[RECORDER_PATH_MAX] = {0}; // where the take ends up
    bool _replace = false;               // writing to recorderTempPath(_path), see arm()

    uint16_t _division = 480;        // ticks per quarter note
    uint32_t _usPerQuarter = 500000; // fixed synthetic tempo, 120 BPM

    uint64_t _startMicros = 0; // time_us_64() of the first event (tick origin); 64-bit so it never wraps
    uint32_t _lastTick = 0;    // absolute tick of the last event written
    uint32_t _mtrkLenPos = 0;  // file offset of the MTrk length field
    uint32_t _bytesWritten = 0; // bytes written after the length field
    uint32_t _eventCount = 0;
    uint32_t _lastSyncMs = 0;
    uint32_t _lastEventMs = 0; // millis() of the most recent event; 0 = none yet

    // Write failure tracking. _writeOk goes false on the first short
    // write or failed sync; the rest of the take is dropped and service()
    // or stop() saves the file up to _goodPos (the end of the last whole
    // event, _goodBytes after the length field).
    bool _writeOk = true;
    uint32_t _goodPos = 0;
    uint32_t _goodBytes = 0;

    void writeHeader();
    void writeBytes(const uint8_t* buf, size_t n);
    void writeVarLen(uint32_t value);
    void endEvent();
    // Closes the file as a valid .mid and moves it into place, see
    // recorderSettleTake(). Removes it only if no event made it. False
    // with _error set on any problem.
    bool finish();
    // The file actually written: _path, or its temp name when replacing.
    const char* writePath(char* buf, size_t size) const;

    // Shared by feed()/feedSysEx(): transitions ARMED->RECORDING on the
    // first call (writing the header) and reports whether the event
    // should actually be written (false while ARMED's writeHeader()
    // failed, after a failed write, or already idle/error).
    bool prepareEvent();
    // Computes this event's delta-tick from the wall clock and advances
    // _lastTick -- shared so feed()/feedSysEx() agree on one clock.
    uint32_t nextDeltaTick();
};

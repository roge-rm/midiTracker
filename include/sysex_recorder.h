#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Captures incoming System Exclusive messages to a raw .syx dump: just
// the messages themselves, concatenated back to back (each already
// boundary-inclusive, F0...F7), with no header and no timing information
// -- the standard format for a "patch backup" file, as opposed to
// MidiRecorder's SMF output which exists to preserve *when* things
// happened in a performance. Same arm/feed/stop/cancel state-machine
// shape as MidiRecorder (see its own header comment) for a consistent
// capture UX, but a much simpler body: no MThd/MTrk, no delta-time, no
// var-len event framing, since there's nothing to time here.
class SysExRecorder {
public:
    enum State { STATE_IDLE, STATE_ARMED, STATE_RECORDING, STATE_ERROR };

    // Arms the recorder to capture to `path`. Nothing is created on disk
    // yet -- see feed(). Returns false (STATE_ERROR, see errorMessage())
    // if `path` already exists.
    bool arm(const char* path);

    // Feeds one incoming System Exclusive message. `data` must be
    // boundary-inclusive (data[0]==0xF0, data[len-1]==0xF7) -- the same
    // shape MidiOutput::SysExHandler hands back. No-op outside
    // STATE_ARMED/STATE_RECORDING. The first call after arm() creates the
    // file.
    void feed(const uint8_t* data, size_t len);

    // Closes the file. No-op if nothing was ever fed (STATE_ARMED) or
    // already idle -- there's no header/length field to finalize, unlike
    // MidiRecorder::stop().
    void stop();

    // Abandons the current capture instead of finalizing it: if a file
    // was already created on disk (STATE_RECORDING -- see feed()), closes
    // and deletes it. STATE_ARMED has no file yet to clean up. Resets to
    // STATE_IDLE either way.
    void cancel();

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t messageCount() const { return _messageCount; }
    uint32_t elapsedMs() const; // 0 until the first message starts the clock

    // ms since the last message arrived, or UINT32_MAX if none has yet --
    // drives the UI's receive-activity indicator (see Ui::drawSysExCapture()).
    uint32_t msSinceLastMessage() const;

private:
    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};
    char _path[192] = {0};

    uint32_t _startMs = 0;      // millis() of the first message
    uint32_t _lastMessageMs = 0; // millis() of the most recent message; 0 = none yet
    uint32_t _lastSyncMs = 0;   // last _file.sync() -- see feed()'s periodic-sync comment
    uint32_t _messageCount = 0;
};

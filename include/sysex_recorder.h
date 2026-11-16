#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "midi_recorder.h" // RECORDER_PATH_MAX, recorderTempPath()

// Captures incoming System Exclusive messages to a raw .syx dump: just
// the messages themselves, concatenated back to back (each already
// boundary-inclusive, F0...F7), with no header and no timing information
// -- the standard format for a "patch backup" file, as opposed to
// MidiRecorder's SMF output which exists to preserve *when* things
// happened in a performance. Same arm/feed/stop/cancel state-machine
// shape as MidiRecorder (see its own header comment) for a consistent
// capture UX, but a much simpler body: no MThd/MTrk, no delta-time, no
// var-len event framing, since there's nothing to time here. Only whole
// messages stay in the file. Runs on core 0 the same way MidiRecorder
// does: the *Message() calls from the MIDI input handler, service()/stop()/cancel() from the file player's update loop.
class SysExRecorder {
public:
    enum State { STATE_IDLE, STATE_ARMED, STATE_RECORDING, STATE_ERROR };

    // Arms the recorder to capture to `path`. Nothing is created on disk
    // yet -- see beginMessage(). `replace` works as in MidiRecorder::arm(): the
    // capture goes to a temp file and replaces `path` only on stop().
    // Returns false (STATE_ERROR, see errorMessage()) if `path` already
    // exists and `replace` is false, or the path is too long.
    bool arm(const char* path, bool replace = false);

    // Streamed input from MidiOutput's SysEx stream handler (see
    // sysex_stream.h), any length. The first beginMessage() after arm()
    // creates the file. Bytes go straight to the file; endMessage() keeps
    // the message and is true if it did, abortMessage() drops it. While
    // one source's message is open, a message starting on the other
    // source is dropped. No-ops outside STATE_ARMED/STATE_RECORDING.
    void beginMessage(uint8_t source);
    void writeMessage(uint8_t source, const uint8_t* data, size_t len);
    bool endMessage(uint8_t source);
    void abortMessage(uint8_t source);

    // One whole message (data[0]==0xF0, data[len-1]==0xF7) through the
    // calls above, from source 0.
    void feed(const uint8_t* data, size_t len);

    // Call from the main loop while capturing: syncs the file every few
    // seconds between messages, drops a message that has had no bytes for
    // 1 s, and after a failed write keeps the whole messages already
    // written and moves to STATE_ERROR.
    void service();

    // Closes the file (and swaps it in when replacing). No-op if nothing
    // was ever fed (STATE_ARMED) or already idle. False (STATE_ERROR) if
    // the save failed; otherwise STATE_IDLE.
    bool stop();

    // Abandons the current capture instead of finalizing it: if a file
    // was already created on disk (STATE_RECORDING -- see beginMessage()), closes
    // and deletes it. A file being replaced is left as it was. Resets to
    // STATE_IDLE either way.
    void cancel();

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t messageCount() const { return _messageCount; }
    // Messages cut off by another status byte, the other source, 1 s
    // without bytes or stopping mid-message.
    uint32_t droppedCount() const { return _droppedCount; }
    uint32_t bytesKept() const { return _goodPos; } // whole messages in the file
    uint32_t elapsedMs() const; // 0 until the first message starts the clock

    // ms since the last message arrived, or UINT32_MAX if none has yet --
    // drives the UI's receive-activity indicator (see Ui::drawSysExCapture()).
    uint32_t msSinceLastMessage() const;

private:
    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};
    char _path[RECORDER_PATH_MAX] = {0}; // where the capture ends up
    bool _replace = false;               // writing to recorderTempPath(_path)
    bool _writeOk = true;                // false after a short write or failed sync
    uint32_t _goodPos = 0;               // end of the last whole message written

    uint32_t _startMs = 0;      // millis() of the first message
    uint32_t _lastMessageMs = 0; // millis() of the most recent message; 0 = none yet
    uint32_t _lastSyncMs = 0;   // last _file.sync(), see service()
    uint32_t _messageCount = 0;
    uint32_t _droppedCount = 0;
    static const uint8_t NO_SOURCE = 0xFF;
    uint8_t _msgSource = NO_SOURCE; // source of the message being written

    const char* writePath(char* buf, size_t size) const;
    bool finish();
};

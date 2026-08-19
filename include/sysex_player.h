#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "config.h"

// Plays back a raw .syx dump: sends each concatenated F0...F7 message in
// the file, one at a time, spaced by a fixed inter-message gap (real
// hardware synths often need a brief pause to digest/store each message
// before the next one arrives -- there's no tempo/timing data in a raw
// .syx file to derive spacing from, unlike MidiPlayer's SMF playback).
// Not a MidiPlayer subclass/variant: the two file formats and playback
// models (tick-scheduled multi-track merge vs. a flat message queue) are
// different enough that sharing a base would mostly just be indirection.
class SysExPlayer {
public:
    enum State { STATE_IDLE, STATE_SENDING, STATE_DONE, STATE_ERROR };

    // Opens `path` and starts sending immediately (no separate play()) --
    // matches FilePlayerMode's own convention of a fresh file open
    // starting playback right away. STATE_ERROR (see errorMessage()) on
    // failure to open, or an empty file.
    bool load(const char* path);

    // Closes the file and resets to STATE_IDLE.
    void close();

    // Call every loop() iteration. Sends the next message once the
    // inter-message gap has elapsed. Cheap no-op outside STATE_SENDING.
    void update();

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t totalBytes() const { return _totalBytes; }
    uint32_t bytesSent() const { return _bytesSent; } // progress bar: bytesSent()/totalBytes()
    uint32_t messagesSent() const { return _messagesSent; }
    uint32_t elapsedMs() const;

    // ms since the last message was sent, or UINT32_MAX if none has yet --
    // drives the UI's send-activity pulse (see Ui::drawSysExPlayer()).
    uint32_t msSinceLastSend() const;

private:
    // Many synths need a moment to digest/store each dump message before
    // the next one arrives; this is a conservative default (comparable to
    // what most patch librarian tools use), not a measured requirement.
    static const uint32_t INTER_MESSAGE_GAP_MS = 20;

    FsFile _file;
    State _state = STATE_IDLE;
    char _error[64] = {0};

    uint32_t _totalBytes = 0;
    uint32_t _bytesSent = 0;
    uint32_t _messagesSent = 0;
    uint32_t _startMs = 0;
    uint32_t _lastSendMs = 0; // 0 = none sent yet

    static uint8_t _msgBuf[MIDI_SYSEX_MAX_LEN];

    // Reads and sends the next F0...F7 message from the current file
    // position. Sets STATE_DONE at a clean EOF (right after a message's
    // trailing F7) or STATE_ERROR on a malformed/truncated file. A
    // message longer than MIDI_SYSEX_MAX_LEN still counts toward
    // progress but isn't sent (would only be a truncated, corrupt
    // message) -- see config.h's comment on that cap.
    bool sendNextMessage();
};

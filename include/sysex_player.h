#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "config.h"

// Plays back a raw .syx dump: sends each F0...F7 message in the file, one
// at a time, spaced by a fixed inter-message gap (real hardware synths
// often need a brief pause to digest/store each message before the next
// one arrives -- there's no timing data in a raw .syx file). Messages can
// be any length: each is checked to the end first, then streamed through
// MidiOutput::sysExOutWrite() a few bytes per update(). Bytes outside a
// message are skipped and counted as junk. A message cut short by a status
// byte, a new F0 or the end of the file is skipped whole, so a receiver
// never sees a broken one. Realtime bytes inside a message aren't sent and
// count as junk.
class SysExPlayer {
public:
    enum State { STATE_IDLE, STATE_SENDING, STATE_DONE, STATE_ERROR };

    // Opens `path` and starts sending immediately (no separate play()) --
    // matches FilePlayerMode's own convention of a fresh file open
    // starting playback right away. STATE_ERROR (see errorMessage()) on
    // failure to open, or an empty file.
    bool load(const char* path);

    // Closes the file and resets to STATE_IDLE. A message cut off here
    // is ended with F7.
    void close();

    // Call every loop() iteration. Reads at most a few hundred bytes and
    // never waits on the outputs. Cheap no-op outside STATE_SENDING.
    // STATE_DONE at the end of the file, or STATE_ERROR if it held no
    // SysEx at all or a card read failed.
    void update();

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t totalBytes() const { return _totalBytes; }
    uint32_t bytesSent() const { return _bytesSent; } // progress bar: bytesSent()/totalBytes()
    uint32_t messagesSent() const { return _messagesSent; }
    uint32_t messagesSkipped() const { return _messagesSkipped; }
    uint32_t junkBytes() const { return _junkBytes; }
    uint32_t elapsedMs() const;

    // ms since bytes last went out, or UINT32_MAX if none have yet --
    // drives the UI's send-activity pulse (see Ui::drawSysExPlayer()).
    uint32_t msSinceLastSend() const;

private:
    // Many synths need a moment to digest/store each dump message before
    // the next one arrives; this is a conservative default (comparable to
    // what most patch librarian tools use), not a measured requirement.
    // Counted from when DIN has sent the whole message.
    static const uint32_t INTER_MESSAGE_GAP_MS = 20;
    // Most bytes read from the card per update() while looking for or
    // checking a message.
    static const uint32_t SCAN_BYTES_PER_UPDATE = 512;
    static const size_t BUF_SIZE = 64;

    enum Phase { PH_SEEK, PH_SCAN, PH_SEND, PH_DRAIN, PH_GAP };

    FsFile _file;
    State _state = STATE_IDLE;
    Phase _phase = PH_SEEK;
    char _error[64] = {0};

    // Read buffer while seeking and scanning, bytes waiting to go out
    // while sending.
    uint8_t _buf[BUF_SIZE];
    uint8_t _bufLen = 0;
    uint8_t _bufPos = 0;
    uint32_t _filePos = 0;  // file offset of the byte after the buffered ones
    uint32_t _msgStart = 0; // the message's F0
    uint32_t _msgEnd = 0;   // just past its F7

    uint32_t _totalBytes = 0;
    uint32_t _bytesSent = 0;
    uint32_t _messagesSent = 0;
    uint32_t _messagesSkipped = 0;
    uint32_t _junkBytes = 0;
    uint32_t _startMs = 0;
    uint32_t _lastSendMs = 0; // 0 = nothing sent yet
    uint32_t _gapStartMs = 0;

    int readByte();
    bool seekTo(uint32_t pos);
    void readFailed();
    void finish();
    void seek(uint32_t& budget);
    void scan(uint32_t& budget);
    void send();
};

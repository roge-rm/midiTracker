#pragma once
#include <Arduino.h>
#include <SdFat.h>

// Streams incoming MIDI channel-voice messages straight to a Standard
// MIDI File (format 0, single track) on the SD card as they arrive,
// rather than buffering a take in RAM. Ticks are derived from a fixed
// synthetic 120 BPM tempo against wall-clock micros() -- we're recording
// real-time performance, not reading tempo off anything.
class MidiRecorder {
public:
    enum State { STATE_IDLE, STATE_ARMED, STATE_RECORDING, STATE_ERROR };

    // Arms the recorder to record to `path`. Nothing is created on disk
    // yet -- see feed(). Returns false (STATE_ERROR, see errorMessage())
    // if `path` already exists.
    bool arm(const char* path);

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

    // Writes the End-of-Track event, patches the final MTrk length, and
    // closes the file. No-op if nothing was ever fed (STATE_ARMED) or
    // already idle.
    void stop();

    // Abandons the current recording instead of finalizing it: if a file
    // was already created on disk (STATE_RECORDING -- see feed()),
    // closes and deletes it rather than salvaging a truncated recording,
    // since the whole point is to discard what's been captured so far.
    // STATE_ARMED has no file yet to clean up. Resets to STATE_IDLE
    // either way.
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
    char _path[192] = {0};

    uint16_t _division = 480;        // ticks per quarter note
    uint32_t _usPerQuarter = 500000; // fixed synthetic tempo, 120 BPM

    uint32_t _startMicros = 0; // micros() of the first event (tick origin)
    uint32_t _lastTick = 0;    // absolute tick of the last event written
    uint32_t _mtrkLenPos = 0;  // file offset of the MTrk length field
    uint32_t _bytesWritten = 0; // bytes written after the length field
    uint32_t _eventCount = 0;
    uint32_t _lastSyncMs = 0;
    uint32_t _lastEventMs = 0; // millis() of the most recent event; 0 = none yet

    void writeHeader();
    void writeBytes(const uint8_t* buf, size_t n);
    void writeVarLen(uint32_t value);

    // Shared by feed()/feedSysEx(): transitions ARMED->RECORDING on the
    // first call (writing the header) and reports whether the event
    // should actually be written (false while ARMED's writeHeader()
    // failed, or already idle/error).
    bool prepareEvent();
    // Computes this event's delta-tick from the wall clock and advances
    // _lastTick -- shared so feed()/feedSysEx() agree on one clock.
    uint32_t nextDeltaTick();
    // Records this event's timestamp (_lastEventMs, unconditionally) and
    // periodically syncs to disk so a power loss loses at most
    // SYNC_INTERVAL_MS of data -- see midi_recorder.cpp's header comment
    // on why the sync itself isn't needed every event.
    void maybeSync();
};

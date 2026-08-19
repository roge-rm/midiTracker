#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "config.h"

// Streams a Standard MIDI File (format 0 or 1) directly off the SD card
// and merges all tracks into a single playback timeline, without ever
// loading the whole file into RAM. Each track keeps its own open file
// handle + a small amount of read-ahead state ("primed" next event).
//
// Not supported: format 2 (independent sequences), SMPTE time division.
class MidiPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    // Opens and parses `path`, priming every track's first event.
    // Leaves the player in STATE_PAUSED (ready to play()) on success,
    // or STATE_ERROR (see errorMessage()) on failure.
    bool load(const char* path);

    // Closes all track file handles and resets to STATE_IDLE.
    void close();

    // Call every loop() iteration. Sends any MIDI events whose time has
    // arrived, in track-merged order. Cheap no-op when not playing.
    void update();

    void play();  // resume/start
    void pause(); // hold, sends all-notes-off
    void stop();  // stop + close()

    State state() const { return _state; }
    const char* errorMessage() const { return _error; }

    uint32_t elapsedMs() const;
    uint16_t trackCount() const { return _numTracks; }
    uint16_t format() const { return _format; }

    // User-driven playback speed multiplier, independent of the file's
    // own tempo meta events (which keep affecting _usPerQuarter as
    // usual -- this just scales whatever that is). Clamped to [0.5, 2.0].
    // Takes effect on the very next update() call; safe to change at any
    // time since the tick origin is re-anchored after every event.
    // NOTE: load() deliberately leaves this untouched (so restarting a
    // just-finished file via play() again preserves it) -- callers doing
    // a genuinely fresh open should call setTempoScale(1.0f) themselves.
    void adjustTempoScale(float delta);
    void setTempoScale(float scale); // same clamping as adjustTempoScale
    float tempoScale() const { return _tempoScale; }

    // Effective tempo actually being played back right now (file tempo
    // scaled by tempoScale()), in beats per minute.
    uint16_t currentBPM() const;

    // Jumps playback to `targetMs`. If currently playing, keeps playing
    // from the new position; if paused, stays paused there; either way,
    // silences whatever's currently sounding first. There is no known
    // total duration (see this class's header comment -- nothing
    // prescans the file), so a `targetMs` beyond the end of the file
    // just lands at EOF (STATE_DONE) instead.
    //
    // Forward seeks (targetMs >= elapsedMs()) continue scanning from the
    // current track cursors -- cheap, proportional to the seek distance.
    // Backward seeks (or seeking after STATE_DONE) have no random-access
    // index to jump to, so they reopen the file and re-scan every track
    // from the start up to targetMs -- cost is proportional to how far
    // into the file the target is, not to the seek distance. Either way,
    // every Set Tempo/Program Change/Control Change/Pitch Bend/Pressure
    // event passed over is still applied (so the landing point's
    // instrument/tempo state is correct) -- only Note On/Off is
    // suppressed, and SysEx is skipped entirely (not re-sent).
    void seekTo(uint32_t targetMs);

private:
    struct TrackReader {
        FsFile file;
        uint32_t chunkEnd = 0;
        uint32_t absoluteTick = 0; // cumulative tick position of the read cursor
        uint8_t runningStatus = 0;
        bool ended = false;

        // Next staged (not-yet-fired) event for this track.
        uint32_t pendingTick = 0;
        bool pendingIsTempo = false;
        uint32_t pendingTempoUsPerQuarter = 500000;
        uint8_t pendingStatus = 0, pendingData1 = 0, pendingData2 = 0, pendingLen = 0;
        // Set instead of the channel-voice fields above for a staged SysEx
        // event -- pendingSysExPos/Len locate the payload within `file`
        // (position of the first byte after the length field, and its
        // byte count including the trailing F7) rather than holding the
        // bytes themselves, since a SysEx payload can be arbitrarily long.
        // Re-read from disk on dispatch (see sendPendingSysEx()).
        bool pendingIsSysEx = false;
        uint32_t pendingSysExPos = 0;
        uint32_t pendingSysExLen = 0;
    };

    TrackReader _tracks[MIDI_MAX_TRACKS];
    uint16_t _numTracks = 0;
    uint16_t _format = 0;
    uint16_t _division = 480; // ticks per quarter note
    uint32_t _usPerQuarter = 500000; // tempo, default 120 BPM
    float _tempoScale = 1.0f;        // user speed multiplier, see adjustTempoScale()
    uint32_t _globalTick = 0;

    // Timeline origin used to convert the next due tick to a micros()
    // deadline; reset after every event so the extrapolated delta (and
    // thus rounding error) stays small.
    uint32_t _originTick = 0;
    uint32_t _originMicros = 0;

    uint32_t _elapsedMsFrozen = 0;
    uint32_t _lastResumeMicros = 0;

    State _state = STATE_IDLE;
    char _error[64] = {0};
    char _path[192] = {0}; // set by load(); lets seekTo() reopen without the caller re-supplying it

    static bool readChunkHeader(FsFile& f, char id[5], uint32_t& length);
    static bool readVarLen(FsFile& f, uint32_t& value);
    static uint8_t channelMessageDataBytes(uint8_t status);

    // Reads events for track idx until one worth stopping on (a channel
    // voice message or a tempo meta) is staged in `pending*`, or the
    // track chunk ends. Returns false and sets ended=true in the latter
    // case.
    bool primeTrack(int idx);

    // Index of the un-ended track with the smallest pendingTick, or -1.
    int findNextTrack() const;

    // Re-reads t's staged SysEx payload from disk (see TrackReader's
    // pendingSysExPos/Len comment) and sends it. Restores the file's read
    // cursor to wherever primeTrack() left it afterward, so the next
    // primeTrack(idx) call continues scanning from the right place. A
    // payload longer than MIDI_SYSEX_MAX_LEN is silently skipped (see
    // config.h's comment on that cap) rather than sent truncated.
    void sendPendingSysEx(TrackReader& t);

    // Used by seekTo(): advances _globalTick/_usPerQuarter/track cursors
    // from wherever they currently are, up to `targetMs` of *virtual*
    // elapsed time -- same event-merge logic update() uses, just driven
    // by a simulated clock instead of real time, with Note On/Off
    // suppressed (see seekTo()'s comment). Sets `reachedEnd` and returns
    // early if EOF is hit before targetMs (nothing further to simulate);
    // otherwise returns targetMs itself, since the position clock should
    // read exactly where it was asked to land even if no event happens
    // to fall exactly there (same as ordinary playback's elapsedMs(),
    // which advances continuously between events, not just when one fires).
    uint32_t advanceSilentlyTo(uint32_t targetMs, bool& reachedEnd);
};

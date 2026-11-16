#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "config.h"

// Streams a Standard MIDI File (format 0 or 1) directly off the SD card
// and merges all tracks into a single playback timeline, without ever
// loading the whole file into RAM. Each track keeps its own open file
// handle + a small amount of read-ahead state ("primed" next event).
// Ticks are either PPQ (tempo meta events apply) or SMPTE frames.
//
// Not supported: format 2 (independent sequences).
class MidiPlayer {
public:
    enum State { STATE_IDLE, STATE_PLAYING, STATE_PAUSED, STATE_DONE, STATE_ERROR };

    MidiPlayer() = default;
    ~MidiPlayer();
    MidiPlayer(const MidiPlayer&) = delete;
    MidiPlayer& operator=(const MidiPlayer&) = delete;

    // Opens and parses `path`, priming every track's first event.
    // Sends CC 121 and bend centre on all channels (see resetChannels()).
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
    // silences whatever's currently sounding first. A `targetMs` beyond
    // the end of the file lands at the end (STATE_DONE).
    //
    // Forward seeks continue scanning from the current track cursors.
    // Backward seeks (or seeking after STATE_DONE) reopen the file and
    // re-scan from the start, so the cost grows with how far in the target
    // is. Tempo events passed over are applied; notes, SysEx and pressure
    // are skipped. The last bank, program, RPN 0-2, CC 1/7/10/11/64/91/93
    // and pitch bend of each channel passed over are sent once at the
    // landing point.
    //
    // With `defer` (scrubbing), only the notes this player holds are
    // released and the reset and landing state wait for finishSeek(), so
    // each step doesn't block on DIN.
    void seekTo(uint32_t targetMs, bool defer = false);
    // Sends what deferred seeks held back: a panic, the reset if a step
    // rewound, then the landing state. No-op if nothing is pending.
    void finishSeek();

private:
    // Read-ahead bytes per track: as much as fits in BUF_POOL_MAX across
    // all tracks, between BUF_MIN and BUF_MAX. SdFat has one sector cache
    // per volume, so reading byte by byte from several tracks in turn
    // re-reads a sector on every switch; this cuts that to once per buffer.
    static const uint16_t BUF_POOL_MAX = 2048;
    static const uint8_t BUF_MIN = 32, BUF_MAX = 128;

    // What a track has staged in its pending* fields.
    enum EventKind : uint8_t { EV_CHANNEL, EV_TEMPO, EV_SYSEX, EV_END };

    struct TrackReader {
        FsFile file;
        uint32_t chunkEnd = 0;
        // Read-ahead: buf (_bufSize bytes in _bufPool) holds file bytes
        // from bufFilePos, bufPos is the next unread byte. The FsFile
        // cursor sits at the end of buf.
        uint8_t* buf = nullptr;
        uint32_t bufFilePos = 0;
        uint8_t bufLen = 0;
        uint8_t bufPos = 0;
        uint32_t absoluteTick = 0; // cumulative tick position of the read cursor
        uint8_t runningStatus = 0;
        bool ended = false;

        // Next staged (not-yet-fired) event for this track. EV_END is the
        // End of Track meta, kept as a timed event so its delta counts.
        uint32_t pendingTick = 0;
        uint8_t pendingKind = EV_CHANNEL;
        uint8_t pendingStatus = 0, pendingData1 = 0, pendingData2 = 0, pendingLen = 0;
        uint32_t pendingTempoUsPerQuarter = 500000;
        // EV_SYSEX: pendingSysExPos/Len locate the payload in `file` (first
        // byte after the length field, and its byte count including the
        // trailing F7). Re-read from disk on dispatch (see sendPendingSysEx()).
        bool pendingSysExHasF0 = false; // false for an F7 escape: its bytes go out as they are
        uint32_t pendingSysExPos = 0;
        uint32_t pendingSysExLen = 0;
    };

    // Last values per channel gathered by a seek, 0xFF = not seen.
    struct SeekChannel {
        uint8_t bank0, bank32, program, bendLsb, bendMsb;
        uint8_t progBank0, progBank32; // bank current at the last program change
        uint8_t cc[7]; // SEEK_CCS order
        uint8_t rpn0Msb, rpn0Lsb, rpn1Msb, rpn1Lsb, rpn2Msb;
        bool rpnTouched; // an RPN select or data entry was passed
    };

    // Heap arrays sized to the file's track count by load(), kept and
    // reused while big enough.
    TrackReader* _tracks = nullptr;
    uint16_t _trackCapacity = 0;
    uint8_t* _bufPool = nullptr;
    uint16_t _bufPoolSize = 0;
    uint8_t _bufSize = BUF_MIN;
    uint16_t _numTracks = 0;
    uint16_t _format = 0;
    uint16_t _division = 480; // raw header word
    uint32_t _usPerQuarter = 500000; // tempo, default 120 BPM
    float _tempoScale = 1.0f;        // user speed multiplier, see adjustTempoScale()

    // A tick lasts _tickNum / _tickDen µs: _usPerQuarter / PPQ, or
    // 1e6 / (fps * ticks per frame) for SMPTE (29 fps means 29.97).
    // _effTickNum is _tickNum / _tempoScale.
    bool _smpte = false;
    uint32_t _tickNum = 500000;
    uint32_t _tickDen = 480;
    uint32_t _effTickNum = 500000;

    // Position: _globalTick is the last fired event, _songUs its time in
    // µs of playback (tempo scale included) and _tickRem the leftover
    // fraction of a µs in 1/_tickDen units, so rounding doesn't drift.
    // While playing, _globalTick fell due at micros() == _originMicros;
    // while paused, _pausedOffsetUs is how far past it playback had got.
    uint32_t _globalTick = 0;
    uint64_t _songUs = 0;
    uint32_t _tickRem = 0;
    uint32_t _originMicros = 0;
    uint32_t _pausedOffsetUs = 0;

    // Current RPN select (CC 101, 100) per channel, 0xFF = none. NRPN
    // selects set it to 0x7F 0x7F (null), since seeks don't restore NRPNs.
    uint8_t _rpnSel[16][2];

    // Notes on per channel, bit n of word n/32, so a natural end can
    // release notes whose note-offs never came.
    uint32_t _held[16][4];
    // A track stopped on a parse failure rather than End of Track.
    bool _trackFailed = false;

    // Landing state gathered by seeks and not yet sent, see finishSeek().
    SeekChannel _seek[16];
    bool _seekPending = false;
    bool _seekNeedsReset = false; // a deferred step rewound
    bool _deferSeek = false;      // set during a deferred seekTo()

    State _state = STATE_IDLE;
    char _error[64] = {0};
    char _path[192] = {0}; // set by load(); lets seekTo() reopen without the caller re-supplying it

    bool readVarLen(TrackReader& t, uint32_t& value);

    // Buffered reads on a track (see TrackReader's buf).
    int readByte(TrackReader& t);
    bool readBytes(TrackReader& t, uint8_t* dst, uint32_t n);
    void skipBytes(TrackReader& t, uint32_t n);
    static void seekTrack(TrackReader& t, uint32_t pos);
    static uint32_t trackPos(const TrackReader& t) { return t.bufFilePos + t.bufPos; }

    // Reads events for track idx until one worth stopping on (channel
    // message, tempo, SysEx or End of Track) is staged in `pending*`.
    // Returns false and sets ended=true if the chunk ends without one.
    bool primeTrack(int idx);

    // Index of the un-ended track with the smallest pendingTick, or -1.
    int findNextTrack() const;

    // µs from _globalTick to `tick`, carrying _tickRem; the new remainder
    // goes to remOut (committed when the event fires).
    uint64_t ticksToUs(uint32_t tick, uint32_t& remOut) const;
    void updateEffTick();

    // Applies a fired tempo meta, or tracks an RPN select.
    void applyTempo(uint32_t usPerQuarter);
    void trackRpnSelect(uint8_t ch, uint8_t cc, uint8_t value);

    // CC 121 and bend centre on all channels, so external synths drop
    // the previous song's state.
    static void resetChannels();

    // Re-reads t's staged SysEx payload from disk (see TrackReader's
    // pendingSysExPos/Len comment) and sends it. Restores the file's read
    // cursor to wherever primeTrack() left it afterward, so the next
    // primeTrack(idx) call continues scanning from the right place. A
    // payload longer than MIDI_SYSEX_MAX_LEN is silently skipped (see
    // config.h's comment on that cap) rather than sent truncated.
    void sendPendingSysEx(TrackReader& t);

    // Used by seekTo(): fires events from the current position up to
    // targetMs of playback time without real time passing. Notes, SysEx
    // and pressure are dropped; the channel state passed over goes into
    // _seek. Sets reachedEnd if every track ended first.
    void advanceSilentlyTo(uint32_t targetMs, bool& reachedEnd);
    void gatherSeekEvent(const TrackReader& t);
    void sendSeekState();
    void clearSeekState();
    // Note-offs for every note in _held, then clears it.
    void releaseHeld();
    void trackHeld(uint8_t status, uint8_t note, uint8_t velocity);
};

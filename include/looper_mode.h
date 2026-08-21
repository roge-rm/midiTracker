#pragma once

// Owns the 4-track MIDI looper workflow: live recording/overdub/mute/stop
// per track, Sync vs. Independent playback, and SD persistence under
// /loops/ (whole-session save/load/delete, or a single track at a time).
// Mirrors FilePlayerMode's begin()/enter()/update() shape so main.cpp's
// mode dispatch doesn't need to know anything about what's inside either
// mode.
namespace LooperMode {

void begin(); // one-time setup, call once from the top-level setup()
void enter(); // called every time the top-level mode switches into MODE_LOOPER

// Per-loop(): input handling, this mode's own screen state machine, and
// its own screen redraws. Returns true exactly once, on the tick the user
// backs out of the main screen (BTN_ALT/BTN_LEFT) -- the caller should
// switch back to the top-level mode-select screen when that happens.
bool update();

// Requests importing an arbitrary Standard MIDI File at `path` into track
// `trackIndex` (0-3), merging every track/channel it contains into that
// one track's flat event buffer. Called by main.cpp right after switching
// into this mode (enter() must already have run), in response to
// FilePlayerMode's file-browser "Load to Looper" action -- this is the
// bridge that lets that action reach into looper track state without
// FilePlayerMode needing to know this mode exists at all. Shows its own
// overwrite-confirm screen before actually touching the track, same as
// the Saved Loop load flow, so both entry points warn consistently.
void requestImport(const char* path, int trackIndex);

// -- On-demand RAM for the track event buffers --------------------------
//
// tracks[] (4 tracks x up to 4096 events each) is the single largest
// static allocation in the whole firmware -- over half of the RP2040's
// 256KB. It's allocated lazily by enter() (see looper_mode.cpp) rather
// than existing as a permanent global, and stays resident through
// ordinary trips to Settings/Mode Select, or even through browsing/MIDI
// preview inside File Player -- it is NOT freed just because the user
// leaves this mode or enters File Player generally. It's only ever freed
// via the functions below, called by FilePlayerMode at the one point
// that actually needs the RAM back: opening a .mod/.s3m (or future .xm)
// file specifically, since those are the only File Player formats large
// enough to matter (see file_player_mode.cpp's openSelected() and
// APP_FREE_LOOPER_RAM). This mirrors FilePlayerMode's own
// S3mPlayer/ModPlayer/MidiPlayer, which allocate on open and free on
// their own "back to browser" action -- the same on-demand pattern
// applied symmetrically, not a special case for either side.

// True if tracks[] holds allocated RAM AND at least one track has
// content that hasn't been saved. FilePlayerMode calls this right before
// opening a .mod/.s3m file, to decide whether reclaiming the RAM needs
// an interactive confirm at all -- if false, freeTracksIfAllocated() can
// just be called directly with nothing at risk.
bool hasUnsavedTrackContent();

// Frees tracks[] if allocated, unconditionally, without prompting or
// saving. A no-op if tracks is already null. Only meant to be called
// once the caller has already established nothing is at risk
// (!hasUnsavedTrackContent()) or doesn't care (the gate below, after the
// user has chosen Discard).
void freeTracksIfAllocated();

// Starts the interactive "free this RAM for File Player" gate: a 3-item
// menu (Save & Continue / Discard & Continue / Cancel), drawn via the
// same look SCREEN_MENU already uses. Only meant to be called when
// hasUnsavedTrackContent() is true. Deliberately independent of
// update()'s own screen state machine -- that only ticks while
// MODE_LOOPER is the active top-level mode, but this gate runs while
// FilePlayerMode is active (see APP_FREE_LOOPER_RAM), about to open a
// tracker-format file.
void beginFreeTracksForFilePlayer();

enum FreeTracksResult {
    FREE_TRACKS_PENDING,   // still waiting on the user -- keep polling updateFreeTracksForFilePlayer()
    FREE_TRACKS_DONE,      // resolved (saved-then-freed, or discarded-then-freed); tracks is now null
    FREE_TRACKS_CANCELLED, // user backed out; tracks is untouched, still holding its content
};

// Per-loop() poll while the gate started by beginFreeTracksForFilePlayer()
// is up. Nothing in Looper can be actively playing/recording at this
// point -- reaching File Player at all already required leaving Looper
// first, which already required stopping every track -- so this doesn't
// need to run alongside update()'s own playback/metronome ticking.
FreeTracksResult updateFreeTracksForFilePlayer();

} // namespace LooperMode

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

} // namespace LooperMode

#pragma once
#include <stddef.h>

// Owns the "play/record MIDI files" workflow: the SD card file browser,
// file playback (MidiPlayer), and MIDI recording to a new file
// (MidiRecorder), plus the on-screen keyboard/menus those need (new
// folder, rename, delete, new recording). This used to be the entire
// app; it's now one of several top-level modes (see main.cpp's TopMode),
// so its input handling, screens, and internal state live here instead
// of in main.cpp.
namespace FilePlayerMode {

// One-time setup, call once from the top-level setup() (after the SD
// card and MidiOutput/Synth are ready). Reads the SD card's root
// directory listing.
void begin();

// (Re)claims whatever shared global hooks this mode needs (the MIDI
// input handler, MidiOutput's target/audio settings) and resets to this
// mode's home screen (the file browser, at its root). Call every time
// the top-level mode switches into MODE_FILE_PLAYER.
void enter();

// Per-loop(): input handling, this mode's own internal state machine,
// and its own screen redraws. Returns true exactly once, on the tick
// where the user backs out of the file browser's root screen (BTN_ALT
// with nowhere further to go up) -- the caller should switch back to the
// top-level mode-select screen when that happens.
bool update();

// True exactly once, right after the user picks a destination track in
// the browser's "Load to Looper" -> "Load into Track" picker. main.cpp
// polls this every tick and, when true, calls consumeLooperImport() and
// hands the result to LooperMode::requestImport(), then switches modes.
// A poll-and-consume pair (like Input::justPressed()) rather than a
// callback, so this mode never needs to know LooperMode exists -- keeping
// that dependency one-directional (main.cpp -> both modes) is why this
// isn't just a direct call to LooperMode from in here.
bool hasPendingLooperImport();

// Clears the pending flag and copies out the file path + destination
// track index (0-3) set by the picker. Only valid to call when
// hasPendingLooperImport() is true.
void consumeLooperImport(char* pathOut, size_t pathOutSize, int& trackIndexOut);

// Drops the file browser's heap-allocated directory index (see
// SdBrowser::freeBuffers()) without touching anything else -- disposable,
// cheaply-rebuilt-on-next-refresh scan data, not unsaved user content, so
// this needs no confirmation. main.cpp calls this right before switching
// into MODE_LOOPER, so a browser sitting on a large folder doesn't
// compete with LooperMode::tracks[] (128KB) for RAM -- same
// one-directional "main.cpp mediates, modes don't call each other"
// reasoning as hasPendingLooperImport()'s own comment above.
void freeBrowserBuffers();

} // namespace FilePlayerMode

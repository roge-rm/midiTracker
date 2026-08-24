#pragma once
#include <Arduino.h>

// Instrument/drum-preset editor overlay: the "Edit Instrument" picker, the
// per-field editor for one melodic family or drum type, and the bank
// save/load/reset flow (see synth.h's InstrumentPresetParams/
// DrumPresetParams and Synth::setInstrumentPreset()/setDrumPreset() for
// what's actually being edited, and why it's safe to edit live).
//
// Not a top-level mode (see main.cpp's TopMode) -- an on-demand overlay
// that any host mode can drive from its own screen/input state machine
// without including that mode's header (both PariSynthMode's own play
// screen, via its SCREEN_EDIT_OVERLAY state, and FilePlayerMode's live
// MIDI-file playback screen, via its APP_SYNTH_EDIT state, do this --
// they're peers including a third, lower-level component, same as both
// already include ui.h/synth.h, not modes including each other). Whichever
// mode is currently hosting this overlay is responsible for keeping its
// own background work running underneath it -- e.g. FilePlayerMode keeps
// ticking MidiPlayer::update() every loop() while this has draw/input
// focus, so a playing file doesn't audibly stall just because the user is
// tweaking an instrument. This component only owns editing state and
// screens, never anything about who's hosting it.
namespace SynthEditor {

// One-time setup: populates the picker's label strings and loads whichever
// bank was last active from /synth/ on the SD card. Call once from
// main.cpp's setup(), after Synth::begin() (loading a bank pushes dirty-
// doorbell messages that need core 1 already running to drain).
void begin();

// Opens the top-level "Edit Instrument" picker: 16 melodic families
// followed by 8 drum types. `startCursor` positions the initial highlight
// (e.g. the first drum entry, for a host that has no single "the"
// instrument to jump straight into editing -- see PariSynthMode's own
// percussion-channel handling).
void openPicker(int startCursor = 0);

// Skips the picker, opening the field editor directly for one melodic
// family (0-15) or drum type (0-7). Backing out of the editor from here
// returns control to the host directly (not back to an unopened picker),
// same as backing out of the editor after reaching it through the picker
// does -- there's nothing to "go back" to within this component either way.
void openForInstrument(uint8_t family);
void openForDrum(uint8_t drumType);

// Skips straight to the Save Bank/Load Bank/Reset to Default menu --
// PariSynthMode's own play screen reaches this via plain ENTER, a shortcut
// to bank management without first picking a specific instrument to edit.
// This is the ONLY entry point into this menu -- the field editor's own
// ENTER/PLAY no longer opens it (see handleEditorInput()) -- so backing
// out (NAV/LEFT) returns straight to the host (update() returns true),
// same as the picker/field editor already do, rather than landing on some
// field editor screen that has nothing to do with how this was reached.
void openBankMenu();

// Per-tick: input handling + redraw for whichever of this component's own
// screens is currently open. Returns true exactly once, on the tick the
// user backs all the way out (NAV/LEFT from the picker, or from the field
// editor) -- the host mode should resume its own screen/input then.
bool update();

} // namespace SynthEditor

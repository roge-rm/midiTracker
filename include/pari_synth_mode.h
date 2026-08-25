#pragma once

// Live, multitimbral play mode for the onboard synth (see synth.h):
// incoming MIDI (TRS or USB, merged) drives Synth::noteOn()/noteOff()/
// programChange() directly, one instrument family per channel resolved
// the same GM way file playback already does (program / 8, channel 10
// fixed to the drum kit) -- see MidiOutput::setInputHandler() and this
// mode's own handleIncomingMidi() in pari_synth_mode.cpp. A channel with
// nothing sending it Program Change can also be assigned by hand from the
// play screen; a later real Program Change on that channel always
// overwrites the manual choice, since both paths write the same
// Synth::programChange() state (see updateManualAssign() for why no
// separate "is this manual or PC-set" flag is needed).
//
// Also hosts the instrument/drum preset editor: the same PRESETS[16]/
// DRUM_PRESETS[] tables file/tracker playback renders from (see synth.h's
// long comment on Synth::setInstrumentPreset() for why editing here also
// changes file playback's sound, and why that's safe to do live), plus
// save/load/reset of the whole 24-preset set as a named bank on the SD
// card under /synth/*.syn -- same persistence pattern SettingsMode's
// Theme editor already established for its own .thm files.
//
// Mirrors FilePlayerMode/LooperMode/SettingsMode's begin()/enter()/
// update() shape so main.cpp's mode dispatch doesn't need to know
// anything about what's inside this mode either.
namespace PariSynthMode {

void begin(); // one-time setup, call once from the top-level setup() -- after Synth::begin()
void enter(); // called every time the top-level mode switches into MODE_PARI_SYNTH

// Per-loop(): input handling, this mode's own screen state machine, and
// its own screen redraws. Returns true exactly once, on the tick the user
// backs out of the main (play) screen -- the caller should switch back to
// the top-level mode-select screen when that happens.
bool update();

} // namespace PariSynthMode

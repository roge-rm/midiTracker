#pragma once

// Owns the top-level Settings screen: a scrollable list of user-
// changeable defaults, adjusted with EDIT+LEFT/RIGHT. Every change is
// persisted to /settings.txt immediately (see saveSettings() in the
// .cpp) and loaded back on begin(), so they survive a power cycle.
// "Live apply" is scoped to LooperMode's own starting state, not a
// continuously-synced value: LooperMode reads these via
// applySettingsIfFresh() on both begin() (boot) and enter() (switching
// into the mode), but only while its session is still fresh (no track
// has content yet) -- so tweaking a default doesn't silently yank the
// BPM/sync/etc. out from under a loop already in progress. Mirrors
// FilePlayerMode/LooperMode's begin()/enter()/update() shape.
namespace SettingsMode {

void begin();
void enter();

// Per-loop(): input handling and this mode's own screen redraws. Returns
// true exactly once, on the tick the user backs out (BTN_NAV/BTN_LEFT)
// -- the caller should switch back to the top-level mode-select screen
// when that happens.
bool update();

// -- accessors, read by LooperMode::begin()/enter() -----------------------
int defaultOutputLevel();    // 0=HP Low, 1=HP High, 2=Line Level -- see Synth::setOutputLevel()
int defaultVolume();         // percent, 0-100 -- see FilePlayerMode's `volume`/Synth::setVolume()
bool reverbEnabled();        // see Synth::setReverbEnabled()
int reverbMix();             // percent, 0-100 -- see Synth::setReverbMix()
int reverbType();            // 0-2, see Synth::setReverbType()
bool synthAudioEnabled();    // see Synth::setSynthAudioEnabled()
int lfoRateTenthsHz();       // tenths of a Hz, 0 = Off -- see Synth::setLfoRateTenthsHz()
int lfoVoices();             // 0 = Off, else max simultaneously-modulated voices -- see Synth::setLfoVoices()
float defaultBpm();
int defaultTimeSigNum();     // beats/bar -- see LooperMode's TimeSig comment
int defaultTimeSigDen();     // which note value is one beat (4 = quarter, 8 = eighth, ...)
int defaultBarLength();     // 0 = Freeform, else bars -- same convention as LoopTrack::barLength
bool defaultSyncMode();      // true = Sync, false = Independent
bool defaultMetronomeOn();
bool defaultCountInEnabled();
int defaultCountInBars();
int metronomeVolume();       // percent, 0-100 -- scales the metronome click's velocity

// -- these two are read continuously (every tick), not just at a
// session-fresh boundary like the accessors above -- they gate reactions
// to *external* MIDI transport/clock events, which should take effect
// immediately whenever toggled, same as MIDI Thru (see MidiOutput).
bool clockSourceSlave();      // false = Internal (preset BPM), true = Slave (follow incoming MIDI clock)
bool midiTransportEnabled();  // react to incoming Start/Stop/Continue

} // namespace SettingsMode

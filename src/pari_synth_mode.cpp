#include "pari_synth_mode.h"

#include <Arduino.h>

#include "input.h"
#include "ui.h"
#include "midi_output.h"
#include "synth.h"
#include "synth_editor.h"
#include "pari_synth_grid.h"
#include "settings_mode.h"

namespace PariSynthMode {
namespace {

// MIDI channel 10 (0-indexed) -- GM percussion, same constant/meaning as
// synth.cpp's own (TU-local) PERCUSSION_CHANNEL, looper_mode.cpp's
// METRONOME_CHANNEL, and pari_synth_grid.cpp's own copy (kept there for
// its own, unrelated, percussion-skip logic). Duplicated rather than
// shared for the same reason those are: modes/peer components don't
// include each other's internals (see looper_mode.h's header comment),
// and it's a single small constant.
const uint8_t PERCUSSION_CHANNEL = 9;

// SCREEN_EDIT_OVERLAY means SynthEditor (see synth_editor.h) currently
// owns input/draw -- this mode's own update() just pumps SynthEditor::
// update() and resumes SCREEN_PLAY once it reports done. There's no
// SynthEditor-internal screen tracked here; that's entirely its own state.
enum Screen { SCREEN_PLAY, SCREEN_EDIT_OVERLAY };
Screen screen = SCREEN_PLAY;
bool needsRedraw = true;

// Percent, see Synth::setVolume() -- mirrored here so PariSynthGrid has
// somewhere to write it and the UI can show it, same "each top-level mode
// keeps its own last-known volume, reasserted on every entry" pattern
// FilePlayerMode's own volume/volumeSeededFromSettings already establish.
int volume = 80;
bool volumeSeededFromSettings = false;

// Shown on a fresh entry in place of resetPrograms()'s all-Program-0
// (Piano) default -- a real multitimbral MIDI file spreads its channels
// across many different instrument families via their own Program Change
// events, so starting every channel here on Piano made the play screen a
// poor first look at what the engine can actually do. This is purely
// pariSynth's own starting point (applied in enter(), on top of
// resetPrograms()) -- it doesn't change what a fresh file-playback load()
// defaults to, which correctly stays GM-spec Piano-until-told-otherwise.
// Index by channel (0-15); the percussion channel's entry is unused, see
// the skip in enter(). Only 15 of the 16 melodic families fit across the
// 15 non-percussion channels -- SynthFX is the one left out.
const uint8_t DEFAULT_CHANNEL_FAMILY[16] = {
    0,  // ch1:  Piano
    4,  // ch2:  Bass
    3,  // ch3:  Guitar
    5,  // ch4:  Strings
    7,  // ch5:  Brass
    2,  // ch6:  Organ
    8,  // ch7:  Reed
    9,  // ch8:  Pipe
    6,  // ch9:  Ensemble
    0,  // ch10: percussion channel -- entry unused, always drums
    10, // ch11: SynthLead
    11, // ch12: SynthPad
    1,  // ch13: Chrom. Percussion (mallets)
    13, // ch14: Ethnic
    14, // ch15: Percussive
    15, // ch16: SoundFX
};

// -- Live incoming MIDI -----------------------------------------------------
// Registered with MidiOutput::setInputHandler() in enter(). Drives the
// onboard synth directly (Synth::noteOn/noteOff/programChange) rather than
// through MidiOutput::sendNoteOn/sendProgramChange -- that pair would
// additionally transmit back out over HW/USB, double-sending on top of
// whatever MIDI Thru already independently forwards (see midi_output.h's
// comment on MidiOutput::noteActivityIn(), used here purely for the note
// strip's visualization bookkeeping). Stays registered (and keeps driving
// the synth) even while SynthEditor has draw/input focus -- editing an
// instrument while still playing it live is the whole point. Deliberately
// NOT shared with PariSynthGrid or FilePlayerMode -- see pari_synth_grid.h's
// own header comment on why swapping input handlers between hosts would be
// dangerous.

// Only actually redraws while the grid itself has draw focus -- while
// SynthEditor owns the screen instead (SCREEN_EDIT_OVERLAY),
// MidiOutput::isChannelActive() still reflects the current state
// underneath (it's driven by MidiOutput::noteActivityIn(), called from the
// Note On/Off cases below regardless of which screen is showing), so the
// grid picks up whatever's current for free on its next full redraw when
// the user backs out, without this needing to draw over whatever
// SynthEditor is currently showing.
void redrawChannelCellIfPlaying(uint8_t channel) {
    if (screen == SCREEN_PLAY) {
        Ui::updatePariSynthChannelCell(channel, channel == PariSynthGrid::selectedChannel(),
                                        MidiOutput::isChannelActive(channel));
    }
}

void handleIncomingMidi(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
    (void)len;
    uint8_t type = status & 0xF0;
    uint8_t channel = status & 0x0F;
    switch (type) {
        case 0x90: // Note On (velocity 0 == Note Off, same convention as everywhere else in this app)
            Synth::noteOn(channel, data1, data2);
            MidiOutput::noteActivityIn(channel, data1, data2);
            redrawChannelCellIfPlaying(channel);
            break;
        case 0x80: // Note Off
            Synth::noteOff(data1);
            MidiOutput::noteActivityIn(channel, data1, 0);
            redrawChannelCellIfPlaying(channel);
            break;
        case 0xC0: // Program Change -- overwrites a manual assignment on this channel, see PariSynthGrid's own comment on that
            Synth::programChange(channel, data1);
            redrawChannelCellIfPlaying(channel);
            break;
        default:
            // CC/PitchBend/AfterTouch: no backing DSP for any of these yet
            // (no sustain-hold state, no runtime-modulatable depth) -- see
            // pari_synth_mode.h's header comment. Deliberately ignored
            // rather than half-wired.
            break;
    }
}

} // namespace

void begin() {
    // SynthEditor::begin()/PariSynthGrid::begin() are called directly from
    // main.cpp's setup() -- they're peer components this mode drives on
    // demand, not something this mode owns the lifecycle of (see
    // synth_editor.h's header comment), so there's nothing left for this
    // mode's own begin() to do.
}

void enter() {
    screen = SCREEN_PLAY;
    // Fresh session: reset first (same as a fresh file-playback load(),
    // see synth.h's resetPrograms() comment), then lay down
    // DEFAULT_CHANNEL_FAMILY's own varied starting point instead of
    // leaving every channel on resetPrograms()'s Piano default -- see its
    // comment. Manual/previous-visit assignments don't carry over either
    // way; a real incoming Program Change still overwrites any of this
    // exactly like it would a manual assignment (see PariSynthGrid).
    Synth::resetPrograms();
    for (int ch = 0; ch < 16; ch++) {
        if (ch == PERCUSSION_CHANNEL) continue;
        Synth::programChange((uint8_t)ch, (uint8_t)(DEFAULT_CHANNEL_FAMILY[ch] * 8));
    }
    // Seed from the Settings default exactly once per boot, same "don't
    // clobber a live in-session adjustment" reasoning FilePlayerMode's own
    // volume/volumeSeededFromSettings use -- leaving and re-entering
    // pariSynth within one session keeps whatever volume was last set here.
    if (!volumeSeededFromSettings) {
        volume = SettingsMode::defaultVolume();
        volumeSeededFromSettings = true;
    }
    Synth::setVolume((uint8_t)volume);
    // Output Level/Reverb/LFO Rate/LFO Voices have no in-session live-
    // adjust control of their own (unlike Volume just above), so they're
    // simply re-applied from Settings every entry -- always current,
    // nothing to protect. Same reasoning and the same calls as
    // FilePlayerMode::enter()'s identical block; pariSynth didn't make
    // these calls at all until this was noticed as a gap (a fresh boot
    // straight into pariSynth, never visiting FilePlayerMode first, ran
    // reverb on setup1()'s hardcoded defaults instead of whatever the user
    // last saved in Settings).
    Synth::setOutputLevel((uint8_t)SettingsMode::defaultOutputLevel());
    Synth::setReverbEnabled(SettingsMode::reverbEnabled());
    Synth::setReverbMix((uint8_t)SettingsMode::reverbMix());
    Synth::setReverbType((uint8_t)SettingsMode::reverbType());
    Synth::setLfoRateTenthsHz((uint16_t)SettingsMode::lfoRateTenthsHz());
    Synth::setLfoVoices((uint8_t)SettingsMode::lfoVoices());
    Synth::setStereoSpread((uint8_t)SettingsMode::stereoSpreadPercent());
    MidiOutput::setInputHandler(handleIncomingMidi);
    PariSynthGrid::enter();
    needsRedraw = true;
}

bool update() {
    if (screen == SCREEN_EDIT_OVERLAY) {
        if (SynthEditor::update()) {
            screen = SCREEN_PLAY;
            needsRedraw = true;
        }
        return false; // the overlay itself never exits this whole mode
    }

    PariSynthGrid::Result result = PariSynthGrid::update(volume);
    if (result == PariSynthGrid::RESULT_OPEN_EDITOR) {
        screen = SCREEN_EDIT_OVERLAY;
        return false;
    }
    if (result == PariSynthGrid::RESULT_EXIT) {
        // Leaving pariSynth entirely -- silence whatever's still sounding,
        // same as FilePlayerMode's own stop/panic path (this mode has no
        // stored playback position to preserve, so there's nothing else
        // to clean up). PariSynthGrid itself stays silent on exit on
        // purpose -- see its own header comment on why FilePlayerMode's
        // use of the same component must NOT do this.
        MidiOutput::allNotesOffAllChannels();
        return true;
    }

    if (needsRedraw) {
        needsRedraw = false;
        Ui::drawPariSynthPlay(PariSynthGrid::selectedChannel(), MidiOutput::activeChannelMask());
        if (PariSynthGrid::volumeOverlayVisible()) Ui::updatePariSynthVolumeOverlay(volume);
    }

    // Live note-strip refresh -- same fixed low-rate poll convention
    // FilePlayerMode's updatePlayerLive() uses, so incoming notes visibly
    // light up the strip without a full-screen redraw every tick.
    static uint32_t lastLiveMs = 0;
    uint32_t now = millis();
    if (now - lastLiveMs >= 50) {
        lastLiveMs = now;
        Ui::updatePariSynthPlayLive();
    }

    return false;
}

} // namespace PariSynthMode

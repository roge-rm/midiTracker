#include "pari_synth_grid.h"

#include <Arduino.h>

#include "input.h"
#include "ui.h"
#include "midi_output.h"
#include "synth.h"
#include "synth_editor.h"

namespace PariSynthGrid {
namespace {

// MIDI channel 10 (0-indexed) -- GM percussion, same constant/meaning as
// synth.cpp's own (TU-local) PERCUSSION_CHANNEL, looper_mode.cpp's
// METRONOME_CHANNEL, and pari_synth_mode.cpp's own copy (kept there for
// DEFAULT_CHANNEL_FAMILY seeding, an unrelated purpose). Duplicated rather
// than shared for the same reason those are: modes/peer components don't
// include each other's internals (see looper_mode.h's header comment),
// and it's a single small constant.
const uint8_t PERCUSSION_CHANNEL = 9;

// 0-15, cursor + target of EDIT+LEFT/RIGHT manual assign / EDIT-tap edit.
// Named distinctly from the public selectedChannel() accessor below --
// they'd otherwise collide, since an anonymous namespace's members are
// visible in its enclosing namespace exactly as if declared there
// directly.
int channelCursor = 0;

// True once EDIT-held has already been used as a modifier (manual family
// cycling, or volume adjust) during the current press -- reset on every
// fresh EDIT press. Lets EDIT's release open the field editor only for a
// genuine tap, same tap-vs-hold disambiguation FilePlayerMode uses for
// its own EDIT-tap-vs-EDIT-held actions.
bool editUsedAsModifier = false;

bool volumeOverlayShown = false;
uint32_t volumeOverlayUntilMs = 0;

void moveChannel(int newChannel) {
    int prev = channelCursor;
    channelCursor = newChannel;
    Ui::updatePariSynthSelection(prev, channelCursor, MidiOutput::isChannelActive((uint8_t)prev),
                                  MidiOutput::isChannelActive((uint8_t)channelCursor));
}

// Channel 10 (percussion) has no single "the" instrument to jump straight
// into editing -- its sound is chosen per note, across 8 different drum
// archetypes, not by Program Change -- so this opens the picker jumped to
// the drum section instead of guessing one.
void openEditorForSelectedChannel() {
    if (channelCursor == PERCUSSION_CHANNEL) {
        SynthEditor::openPicker(Synth::INSTRUMENT_FAMILY_COUNT); // first drum entry
        return;
    }
    uint8_t program = Synth::getChannelProgram((uint8_t)channelCursor);
    uint8_t family = (program >> 3) & 0x0F;
    SynthEditor::openForInstrument(family);
}

void showVolumeOverlay(int volume) {
    volumeOverlayShown = true;
    volumeOverlayUntilMs = millis() + 1500;
    Ui::updatePariSynthVolumeOverlay(volume);
}

// Restores channel 9's cell (the volume overlay's rect, see ui.h's
// updatePariSynthVolumeOverlay() comment) once the overlay's fade timer
// has elapsed. Channel index 8, not 15 -- the grid is column-major
// (col = channel/8, row = channel%8, see drawParisynthCell()), so index 8
// is row 0 of the *right* column (displayed "9", top-right, under the
// header) while index 15 is row 7 of the right column (displayed "16",
// bottom-right) -- restoring the wrong one left the overlay never
// actually erased.
void checkVolumeOverlayExpiry() {
    if (volumeOverlayShown && millis() >= volumeOverlayUntilMs) {
        volumeOverlayShown = false;
        Ui::updatePariSynthChannelCell(8, channelCursor == 8, MidiOutput::isChannelActive(8));
    }
}

// EDIT held + LEFT/RIGHT: manually cycles the selected channel's
// instrument family by calling Synth::programChange() -- the exact same
// call a real incoming Program Change makes, so a later real PC message
// on this channel naturally overwrites a manual choice with no extra
// state needed to track which one "wins". EDIT held + UP/DOWN: adjusts
// `volume` on a flat hold-repeat (not the two-speed accelerating kind
// LEFT/RIGHT uses here -- matches FilePlayerMode::handleVolumeHold()'s
// own rate, so every volume control in this app steps the same way).
void handleEditHeld(int& volume) {
    if (channelCursor != PERCUSSION_CHANNEL) {
        const uint32_t NORMAL_INTERVAL_MS = 150;
        const uint32_t FAST_INTERVAL_MS = 60;
        const uint32_t ACCEL_AFTER_MS = 1500;
        static uint32_t rightPressedAtMs = 0, leftPressedAtMs = 0;
        static uint32_t lastRightStep = 0, lastLeftStep = 0;
        uint32_t now = millis();
        if (Input::justPressed(BTN_RIGHT)) rightPressedAtMs = now;
        if (Input::justPressed(BTN_LEFT)) leftPressedAtMs = now;

        int direction = 0;
        if (Input::isDown(BTN_RIGHT)) {
            uint32_t interval = (now - rightPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
            if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval) { direction = 1; lastRightStep = now; }
        }
        if (Input::isDown(BTN_LEFT)) {
            uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
            if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) { direction = -1; lastLeftStep = now; }
        }
        if (direction != 0) {
            editUsedAsModifier = true;
            uint8_t program = Synth::getChannelProgram((uint8_t)channelCursor);
            int family = (int)((program >> 3) & 0x0F) + direction;
            if (family < 0) family = 0;
            if (family > 15) family = 15;
            Synth::programChange((uint8_t)channelCursor, (uint8_t)(family * 8));
            Ui::updatePariSynthChannelCell(channelCursor, true, MidiOutput::isChannelActive((uint8_t)channelCursor));
        }
    }

    const uint32_t VOL_REPEAT_INTERVAL_MS = 120;
    static uint32_t lastUpStep = 0, lastDownStep = 0;
    uint32_t now = millis();
    if (Input::isDown(BTN_UP) &&
        (Input::justPressed(BTN_UP) || now - lastUpStep >= VOL_REPEAT_INTERVAL_MS)) {
        editUsedAsModifier = true;
        volume = volume + 5 > 100 ? 100 : volume + 5;
        Synth::setVolume((uint8_t)volume);
        showVolumeOverlay(volume);
        lastUpStep = now;
    }
    if (Input::isDown(BTN_DOWN) &&
        (Input::justPressed(BTN_DOWN) || now - lastDownStep >= VOL_REPEAT_INTERVAL_MS)) {
        editUsedAsModifier = true;
        volume = volume - 5 < 0 ? 0 : volume - 5;
        Synth::setVolume((uint8_t)volume);
        showVolumeOverlay(volume);
        lastDownStep = now;
    }
}

// While held, moves the channel cursor every REPEAT_INTERVAL_MS instead of
// once per press -- the first step still fires immediately on the press
// edge (Input::justPressed()), then it free-runs until release. Same flat-
// rate hold-to-repeat convention as FilePlayerMode's handleTempoHold()/
// LooperMode's own UP/DOWN row-move handler -- a plain repeat, not the
// two-speed accelerating one handleEditHeld() uses for LEFT/RIGHT (that
// one drags a value across a wide range; this one just steps a
// 16-position cursor, wrapping is enough on its own).
void handleChannelMoveHold() {
    const uint32_t REPEAT_INTERVAL_MS = 120;
    static uint32_t lastUpStep = 0, lastDownStep = 0;
    uint32_t now = millis();
    if (Input::isDown(BTN_UP) &&
        (Input::justPressed(BTN_UP) || now - lastUpStep >= REPEAT_INTERVAL_MS)) {
        moveChannel(channelCursor > 0 ? channelCursor - 1 : 15);
        lastUpStep = now;
    }
    if (Input::isDown(BTN_DOWN) &&
        (Input::justPressed(BTN_DOWN) || now - lastDownStep >= REPEAT_INTERVAL_MS)) {
        moveChannel(channelCursor < 15 ? channelCursor + 1 : 0);
        lastDownStep = now;
    }
}

} // namespace

void begin() {
    // Nothing to do -- no persistent setup, everything here is per-visit
    // state reset by enter().
}

void enter() {
    channelCursor = 0;
    editUsedAsModifier = false;
    volumeOverlayShown = false;
}

int selectedChannel() { return channelCursor; }

bool volumeOverlayVisible() { return volumeOverlayShown; }

Result update(int& volume) {
    checkVolumeOverlayExpiry();

    // EDIT is a tap-vs-hold modifier: a bare tap opens the field editor
    // for the selected channel, but only if this press wasn't also used
    // as a hold-modifier for manual family cycling or volume
    // (handleEditHeld(), which also owns all other input while EDIT is
    // down -- see the isDown check just below).
    if (Input::justPressed(BTN_EDIT)) {
        editUsedAsModifier = false;
    } else if (Input::justReleased(BTN_EDIT)) {
        if (!editUsedAsModifier) {
            openEditorForSelectedChannel();
            return RESULT_OPEN_EDITOR;
        }
    }
    if (Input::isDown(BTN_EDIT)) {
        handleEditHeld(volume);
        return RESULT_NONE;
    }
    handleChannelMoveHold();
    // Plain RIGHT (EDIT already claims the held-modifier case above):
    // jumps to the same row in the other column -- channel^8 flips between
    // the 0-7/8-15 column halves (see PARISYNTH_ROWS's own layout comment
    // in ui.cpp) -- rather than seeking within one column, which UP/DOWN's
    // wraparound already covers on its own.
    if (Input::justPressed(BTN_RIGHT)) {
        moveChannel(channelCursor ^ 8);
    }
    // ENTER: jumps straight to the Save/Load/Reset bank menu, skipping the
    // picker. PLAY still opens the picker -- the two aren't equivalent
    // triggers on this screen the way they are elsewhere.
    if (Input::justPressed(BTN_ENTER)) {
        SynthEditor::openBankMenu();
        return RESULT_OPEN_EDITOR;
    }
    if (Input::justPressed(BTN_PLAY)) {
        SynthEditor::openPicker();
        return RESULT_OPEN_EDITOR;
    }
    // LEFT mirrors RIGHT's column jump when there's a right column to jump
    // back from (channelCursor >= 8) -- only once already on the left
    // column does LEFT fall through to the same exit result NAV uses.
    if (Input::justPressed(BTN_LEFT) && channelCursor >= 8) {
        moveChannel(channelCursor ^ 8);
        return RESULT_NONE;
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        return RESULT_EXIT;
    }
    return RESULT_NONE;
}

} // namespace PariSynthGrid

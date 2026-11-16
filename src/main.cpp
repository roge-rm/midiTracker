#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // TinyUSBDevice, for the USB product name
#include "sd_card.h"
#include "midi_output.h"
#include "synth.h"
#include "input.h"
#include "ui.h"
#include "ui_strip.h"
#include "ui_transition.h"
#include "battery.h"
#include "file_player_mode.h"
#include "looper_mode.h"
#include "pari_synth_mode.h"
#include "pari_synth_grid.h"
#include "settings_mode.h"
#include "synth_editor.h"
#include "usb_drive_mode.h"
#include "screensaver.h"

// Top-level boot screen: the user picks one of a small number of
// self-contained modes, each of which owns its own screens/input/state
// from here on (see file_player_mode.h/looper_mode.h/settings_mode.h).
// main.cpp only knows how to list them and switch between them -- it
// doesn't know anything about what's inside a mode.
namespace {

enum TopMode { MODE_SELECT, MODE_FILE_PLAYER, MODE_LOOPER, MODE_PARI_SYNTH, MODE_SETTINGS };

// Mode list, top to bottom.
const char* MODE_LABELS[] = { "FILES", "LOOPER", "PARISYNTH", "SETTINGS" };
const int MODE_COUNT = 4;

// Parallel to MODE_LABELS -- modeCursor (a position in that list) maps to
// a TopMode through this array, not a hardcoded chain of ternaries, so
// adding/reordering an entry only ever needs a change in one place. See
// handleModeSelectInput().
const TopMode MODE_ORDER[] = { MODE_FILE_PLAYER, MODE_LOOPER, MODE_PARI_SYNTH, MODE_SETTINGS };

TopMode topMode = MODE_SELECT;
int modeCursor = 0;
bool needsRedraw = true; // only used for the mode-select screen itself

// Set when enterMode() arms a transition; the new mode draws on its first
// update, in the next loop.
bool armedThisLoop = false;

void enterMode(TopMode mode) {
    // The looper import goes from one mode straight to another, no slide.
    if (mode == MODE_SELECT) Ui::transitionNext(Ui::TRANSITION_OUT);
    else if (topMode == MODE_SELECT) Ui::transitionNext(Ui::TRANSITION_IN);
    armedThisLoop = true;
    topMode = mode;
    needsRedraw = true;
    switch (mode) {
        case MODE_FILE_PLAYER: FilePlayerMode::enter(); break;
        case MODE_LOOPER:
            // Reclaim FilePlayerMode's file browser RAM before
            // LooperMode::tracks[] allocates -- pure disposable scan data
            // (see FilePlayerMode::freeBrowserBuffers()'s comment), so
            // this is silent/unconditional, unlike LooperMode's own
            // interactive "Save & Continue / Discard & Continue / Cancel"
            // gate for its *unsaved* track content. No longer RAM-
            // motivated for tracks[] itself now that it's placement-newed
            // into SharedRamArena rather than the general heap (see
            // shared_ram_arena.h) -- kept so the browser doesn't sit on a
            // huge folder's index for the rest of the session for no
            // reason.
            FilePlayerMode::freeBrowserBuffers();
            LooperMode::enter();
            break;
        case MODE_PARI_SYNTH:  PariSynthMode::enter(); break;
        case MODE_SETTINGS:    SettingsMode::enter(); break;
        case MODE_SELECT:      break;
    }
}

void moveModeCursor(int to) {
    if (to < 0 || to >= MODE_COUNT || to == modeCursor) return;
    int prev = modeCursor;
    modeCursor = to;
    Ui::updateModeSelectCursor(MODE_LABELS, MODE_COUNT, prev, modeCursor);
}

// UP/DOWN move through the list, wrapping at the ends like the other menus.
// RIGHT, ENTER or PLAY opens, on release so the new mode doesn't see it.
// RIGHT only counts if it was pressed on this screen.
bool rightPressed = false;

void handleModeSelectInput() {
    if (Input::justPressed(BTN_UP)) moveModeCursor(modeCursor > 0 ? modeCursor - 1 : MODE_COUNT - 1);
    if (Input::justPressed(BTN_DOWN)) moveModeCursor(modeCursor < MODE_COUNT - 1 ? modeCursor + 1 : 0);
    if (Input::justPressed(BTN_RIGHT)) rightPressed = true;
    bool rightRelease = Input::justReleased(BTN_RIGHT) && rightPressed;
    if (Input::justReleased(BTN_RIGHT)) rightPressed = false;
    if (rightRelease || Input::justReleased(BTN_ENTER) || Input::justReleased(BTN_PLAY)) {
        enterMode(MODE_ORDER[modeCursor]);
    }
}

// Screensaver: whether the screen up now may go away, and drawing it again
// when it wakes.
bool screensaverAllowed() {
    switch (topMode) {
        case MODE_FILE_PLAYER: return FilePlayerMode::screensaverAllowed();
        case MODE_LOOPER:      return LooperMode::screensaverAllowed();
        case MODE_PARI_SYNTH:  return PariSynthMode::screensaverAllowed();
        case MODE_SETTINGS:    return SettingsMode::screensaverAllowed();
        default:               return true;
    }
}

void redrawScreen() {
    switch (topMode) {
        case MODE_FILE_PLAYER: FilePlayerMode::redrawScreen(); break;
        case MODE_LOOPER:      LooperMode::redrawScreen(); break;
        case MODE_PARI_SYNTH:  PariSynthMode::redrawScreen(); break;
        case MODE_SETTINGS:    SettingsMode::redrawScreen(); break;
        case MODE_SELECT:      needsRedraw = true; break;
    }
}

void drawScreensaver() {
    UiSaver::Info info;
    void (*pump)() = nullptr;
    if (topMode == MODE_FILE_PLAYER) {
        FilePlayerMode::screensaverInfo(info, pump);
    } else if (topMode == MODE_LOOPER && LooperMode::screensaverPlaying()) {
        info.src = UiSaver::SRC_LOOPER;
        info.name = "Looper";
        pump = LooperMode::screensaverPump();
    }
    Screensaver::draw(info, pump);
}

} // namespace

void setup() {
    // Must happen before the host enumerates the device (in practice,
    // before anything else) -- the earlephilhower core already called
    // TinyUSBDevice.begin() ahead of setup(), so this only overrides the
    // descriptor strings, it doesn't (and mustn't) re-init the stack.
    TinyUSBDevice.setProductDescriptor("midiTracker");

    Serial.begin(115200); // USB CDC debug console (separate from USB MIDI)

    Input::begin();
    Ui::begin();
    Battery::begin();
    // The boot animation runs between the load steps below, a frame per
    // step, and bootAnimFinish() plays out what's left.
    Ui::bootAnimStart();

    MidiOutput::begin();
    Ui::bootAnimStep();
    UsbDriveMode::begin();
    Ui::bootAnimStep();

    // The MIDI and drive interfaces are added after the core has
    // enumerated the serial port, so re-enumerate for the host to see them.
    if (TinyUSBDevice.mounted()) {
        TinyUSBDevice.detach();
        delay(10);
        TinyUSBDevice.attach();
    }

    Ui::bootAnimStep();
    bool sdOk = sdCardBegin();
    Ui::bootAnimStep();
    // Only now unblock core 1's I2S/PIO init (see the comment on
    // Synth::begin()) -- it must come strictly after the SD card's own
    // PIO claim attempt above, success or not, since RP2040's 8 PIO state
    // machines are a shared, exhaustible resource and core 1 otherwise
    // starts essentially in parallel with this whole function.
    Synth::begin();

    if (!sdOk && sdCardDetected()) {
        // The card answers but its volume doesn't mount: offer it over USB
        // until it's fixed and mounts.
        Ui::bootAnimFinish();
        UsbDriveMode::enterMountFailed();
        do {
            Input::update();
            MidiOutput::update();
        } while (!UsbDriveMode::update());
        sdOk = true;
    }
    if (!sdOk) {
        Ui::bootAnimFinish();
        Ui::drawMessage("SD card not found", "check card");
        while (true) { delay(1000); }
    }

#ifdef TRACKER_PROFILE
    // Profile build: plays PROFILE_FILES and never returns, see tracker_profile.cpp.
    Ui::bootAnimFinish();
    { extern void trackerProfileRun(); trackerProfileRun(); }
#endif
    FilePlayerMode::begin();
    Ui::bootAnimStep();
    SettingsMode::begin();
    Ui::bootAnimStep();
    LooperMode::begin(); // no ordering requirement on SettingsMode anymore -- see its own comment
    Ui::bootAnimStep();
    PariSynthMode::begin(); // after Synth::begin() above -- see its own comment on why
    Ui::bootAnimStep();
    SynthEditor::begin(); // same ordering requirement -- loading the active bank pushes dirty-doorbell messages core 1 must already be running to drain
    PariSynthGrid::begin(); // no ordering requirement -- no-op today, kept for symmetry with SynthEditor::begin()
    Ui::bootAnimFinish();
    // The home screen comes in over the boot screen on the first loop,
    // well inside the transition's arm time.
    Ui::transitionNext(Ui::TRANSITION_IN);
}

void loop() {
    Ui::sliceBeginLoop();
    Input::update();
    if (Screensaver::update(screensaverAllowed())) redrawScreen();
    MidiOutput::update();

    switch (topMode) {
        case MODE_SELECT:
            handleModeSelectInput();
            break;

        case MODE_FILE_PLAYER:
            // Checked before update() itself: the flag is set by that same
            // update() on a prior tick (once the user confirms a track in
            // the browser's "Load to Looper" picker), so this tick hands
            // off to the looper instead of re-entering the browser.
            if (FilePlayerMode::hasPendingLooperImport()) {
                char path[224];
                int trackIndex;
                FilePlayerMode::consumeLooperImport(path, sizeof(path), trackIndex);
                enterMode(MODE_LOOPER);
                LooperMode::requestImport(path, trackIndex);
            } else if (FilePlayerMode::update()) {
                enterMode(MODE_SELECT);
            }
            break;

        case MODE_LOOPER:
            if (LooperMode::update()) enterMode(MODE_SELECT);
            break;

        case MODE_PARI_SYNTH:
            if (PariSynthMode::update()) enterMode(MODE_SELECT);
            break;

        case MODE_SETTINGS:
            if (SettingsMode::update()) enterMode(MODE_SELECT);
            break;
    }

    if (needsRedraw && topMode == MODE_SELECT) {
        needsRedraw = false;
        Ui::transitionDraw([] { Ui::drawModeSelect(MODE_LABELS, MODE_COUNT, modeCursor); });
    }

    // Battery gauge, bottom-right corner. Battery::update() re-samples the
    // ADC every ~5 s; the gauge redraws only when what it shows changes.
    // Screens draw it with their footer, so a screen change brings it back.
    Battery::update();
    Ui::updateBatteryMeter();

    // A screen that doesn't draw through transitionDraw() on its first
    // update leaves the transition unused; drop it.
    if (!armedThisLoop) Ui::transitionCancel();
    armedThisLoop = false;

    drawScreensaver();
}

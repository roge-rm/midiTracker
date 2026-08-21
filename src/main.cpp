#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // TinyUSBDevice, for the USB product name
#include "sd_card.h"
#include "midi_output.h"
#include "synth.h"
#include "input.h"
#include "ui.h"
#include "battery.h"
#include "file_player_mode.h"
#include "looper_mode.h"
#include "settings_mode.h"

// Top-level boot screen: the user picks one of a small number of
// self-contained modes, each of which owns its own screens/input/state
// from here on (see file_player_mode.h/looper_mode.h/settings_mode.h).
// main.cpp only knows how to list them and switch between them -- it
// doesn't know anything about what's inside a mode.
namespace {

enum TopMode { MODE_SELECT, MODE_FILE_PLAYER, MODE_LOOPER, MODE_SETTINGS };

const char* MODE_LABELS[] = { "Play / Record Files", "MIDI Looper", "Settings" };
const int MODE_COUNT = 3;

TopMode topMode = MODE_SELECT;
int modeCursor = 0;
bool needsRedraw = true; // only used for the mode-select screen itself

void enterMode(TopMode mode) {
    topMode = mode;
    needsRedraw = true;
    switch (mode) {
        case MODE_FILE_PLAYER: FilePlayerMode::enter(); break;
        case MODE_LOOPER:      LooperMode::enter(); break;
        case MODE_SETTINGS:    SettingsMode::enter(); break;
        case MODE_SELECT:      break;
    }
}

void handleModeSelectInput() {
    if (Input::justPressed(BTN_UP)) {
        if (modeCursor > 0) {
            int prev = modeCursor;
            modeCursor--;
            Ui::updateModeSelectCursor(MODE_LABELS, MODE_COUNT, prev, modeCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (modeCursor < MODE_COUNT - 1) {
            int prev = modeCursor;
            modeCursor++;
            Ui::updateModeSelectCursor(MODE_LABELS, MODE_COUNT, prev, modeCursor);
        }
    }
    // Reacts on release, not press: the new mode's update() doesn't run
    // until the next tick, so if this fired on press, that same physical
    // press's *release* would still be pending and land as a fresh edge
    // inside whichever mode just became active -- e.g. LooperMode reads
    // an ENTER release as "tap to open the menu", so pressing ENTER here
    // used to open the looper straight into its menu screen. Triggering
    // on release instead consumes both edges of the gesture before the
    // new mode ever sees an update() tick.
    if (Input::justReleased(BTN_ENTER) || Input::justReleased(BTN_PLAY) ||
        Input::justReleased(BTN_RIGHT)) {
        TopMode target = modeCursor == 0 ? MODE_FILE_PLAYER
                        : modeCursor == 1 ? MODE_LOOPER
                                          : MODE_SETTINGS;
        enterMode(target);
    }
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
    Ui::drawSplash();
    delay(1000);

    MidiOutput::begin();

    bool sdOk = sdCardBegin();
    // Only now unblock core 1's I2S/PIO init (see the comment on
    // Synth::begin()) -- it must come strictly after the SD card's own
    // PIO claim attempt above, success or not, since RP2040's 8 PIO state
    // machines are a shared, exhaustible resource and core 1 otherwise
    // starts essentially in parallel with this whole function.
    Synth::begin();

    if (!sdOk) {
        Ui::drawMessage("SD card not found", "check card");
        while (true) { delay(1000); }
    }

    FilePlayerMode::begin();
    SettingsMode::begin();
    LooperMode::begin(); // no ordering requirement on SettingsMode anymore -- see its own comment
}

void loop() {
    Input::update();
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

        case MODE_SETTINGS:
            if (SettingsMode::update()) enterMode(MODE_SELECT);
            break;
    }

    if (needsRedraw && topMode == MODE_SELECT) {
        needsRedraw = false;
        Ui::drawModeSelect(MODE_LABELS, MODE_COUNT, modeCursor);
    }

    // Battery gauge, bottom-right corner, drawn over whatever the mode
    // above just did -- independent of any mode's own redraw cycle so it
    // shows up everywhere without threading it through every screen.
    // Battery::update() only actually re-samples the ADC every ~5s (see
    // its own comment); this 1s redraw timer is deliberately faster
    // than that so the icon reappears quickly after a screen transition
    // wipes the footer, while still being far too infrequent to matter
    // against the mixing-throughput budget (see S3mPlayer/XmPlayer's
    // real-hardware-measured cost of *frequent* footer redraws).
    Battery::update();
    static uint32_t lastBatteryDrawMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastBatteryDrawMs >= 1000) {
        lastBatteryDrawMs = nowMs;
        Ui::drawBatteryMeter();
    }
}

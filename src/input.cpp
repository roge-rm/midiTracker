#include "input.h"
#include "pins.h"
#include "config.h"

namespace {

const uint8_t kPins[BTN_COUNT] = {
    INPUT_LEFT, INPUT_RIGHT, INPUT_UP, INPUT_DOWN,
    INPUT_ALT, INPUT_EDIT, INPUT_ENTER, INPUT_NAV, INPUT_PLAY,
};

bool stableState[BTN_COUNT];     // debounced, true == pressed
bool lastRawState[BTN_COUNT];    // raw reading from the previous update()
uint32_t lastChangeMs[BTN_COUNT];
bool edgePressed[BTN_COUNT];
bool edgeReleased[BTN_COUNT];
bool muted = false;

bool allUp() {
    for (int i = 0; i < BTN_COUNT; i++)
        if (stableState[i]) return false;
    return true;
}

inline bool readRawPressed(uint8_t pin) {
    return digitalRead(pin) == LOW; // active-low, internal pull-up
}

} // namespace

namespace Input {

void begin() {
    for (int i = 0; i < BTN_COUNT; i++) {
        pinMode(kPins[i], INPUT_PULLUP);
        stableState[i] = false;
        lastRawState[i] = readRawPressed(kPins[i]);
        lastChangeMs[i] = millis();
        edgePressed[i] = false;
        edgeReleased[i] = false;
    }
}

void update() {
    uint32_t now = millis();
    // Checked before reading, so the release that ends the mute stays hidden.
    if (muted && allUp()) muted = false;
    for (int i = 0; i < BTN_COUNT; i++) {
        edgePressed[i] = false;
        edgeReleased[i] = false;

        bool raw = readRawPressed(kPins[i]);
        if (raw != lastRawState[i]) {
            lastRawState[i] = raw;
            lastChangeMs[i] = now;
        } else if ((now - lastChangeMs[i]) >= BUTTON_DEBOUNCE_MS) {
            if (raw != stableState[i]) {
                stableState[i] = raw;
                if (raw) edgePressed[i] = true;
                else edgeReleased[i] = true;
            }
        }
    }
}

bool isDown(ButtonId id) { return !muted && stableState[id]; }
bool justPressed(ButtonId id) { return !muted && edgePressed[id]; }
bool justReleased(ButtonId id) { return !muted && edgeReleased[id]; }

bool anyDown() { return !allUp(); }

void muteUntilReleased() { muted = true; }

bool anyNewPress() {
    for (int i = 0; i < BTN_COUNT; i++)
        if (!stableState[i] && readRawPressed(kPins[i])) return true;
    return false;
}

} // namespace Input

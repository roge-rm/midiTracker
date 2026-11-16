#pragma once
#include <Arduino.h>

enum ButtonId {
    BTN_LEFT = 0,
    BTN_RIGHT,
    BTN_UP,
    BTN_DOWN,
    BTN_ALT,
    BTN_EDIT,
    BTN_ENTER,
    BTN_NAV,
    BTN_PLAY,
    BTN_COUNT
};

// Debounced digital buttons, all wired to GND with internal pull-ups
// (pressed == LOW). Flip the polarity in input.cpp if your board reads
// buttons active-high instead.
namespace Input {
void begin();
void update(); // call once per loop() iteration, before checking buttons

bool isDown(ButtonId id);       // current debounced level
bool justPressed(ButtonId id);  // true for exactly one update() after press
bool justReleased(ButtonId id); // true for exactly one update() after release

// A button reads pressed that update() still has as up. Reads the pins
// only, so the press is still picked up by the next update(). For long
// draws that stop early on a key.
bool anyNewPress();

// Any button down, mute or not.
bool anyDown();

// Hides every button (isDown, justPressed and justReleased read as up)
// until all of them have been let go, for a press that only wakes the
// screen.
void muteUntilReleased();
} // namespace Input

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
} // namespace Input

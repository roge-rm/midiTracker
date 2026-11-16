#pragma once
#include <stdint.h>
#include "ui_saver.h"

// The screensaver. Core 0 only. main.cpp calls update() straight after
// Input::update() and draw() at the end of loop().
//
// After the set time with no button down it dims the backlight to 20% of
// the Brightness setting, holds the screen (Ui::setScreenHeld()) so the
// modes keep running without drawing, and shows a view. The backlight goes
// off 5 minutes after dimming while something plays, 1 minute after with
// nothing playing, and straight away for Black.
namespace Screensaver {

enum Style : uint8_t { STYLE_AUTO, STYLE_NOW_PLAYING, STYLE_METERS, STYLE_BLACK };

void setTimeoutMinutes(int minutes); // 0 = off
void setStyle(Style style);

// allowed: the screen up now may be put away and drawn again. Returns true
// on the loop it wakes, and the caller then has the mode draw its whole
// screen. A button wakes it, and that press is hidden from the modes until
// every button is up. Low battery and a screen that isn't allowed wake it
// too.
bool update(bool allowed);

// What's playing (src SRC_NONE for nothing) and the player's draw pump, or
// nullptr. Puts the backlight back after a wake, and draws a frame when
// one is due.
void draw(const UiSaver::Info& info, void (*pump)());

bool active();

} // namespace Screensaver

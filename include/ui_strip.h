#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// Strip canvas and UI time slice, core 0 only.
//
// The strip is one 320x16 RGB565 buffer in RAM. Draw into it with the usual
// TFT_eSPI calls in local coordinates (0,0 is the strip's top left corner),
// then stripSend() pushes it to the screen by DMA. While it goes out, core 0
// runs the draw pump (the player's update(), which fills the WAV ring or
// sends due MIDI events) instead of waiting on the SPI.
//
//   TFT_eSprite& s = Ui::stripBegin(x, y, w, h, bg); // w * h <= STRIP_PIXELS
//   s.setTextColor(fg, bg);
//   s.drawString("Time: 1:23", 0, 0);
//   Ui::stripSend();
//
// The strip can be any shape that fits in the buffer (320x16, 160x32,
// 280x18, etc). Only one strip is open at a time and stripSend() returns
// once it has gone out, so direct tft calls are safe again straight after.
namespace Ui {

const int STRIP_W = 320;
const int STRIP_H = 16;
const int STRIP_PIXELS = STRIP_W * STRIP_H;

// Ui::begin() calls this once after tft.init().
void stripInit(TFT_eSPI& tft);

// Opens the strip at screen position x, y, size w x h, cleared to bg, with
// font 2 and TL_DATUM set. w * h must be at most STRIP_PIXELS; anything
// bigger is cut down to the rows that fit.
TFT_eSprite& stripBegin(int x, int y, int w, int h, uint16_t bg);

// Rows of width w that fit in the buffer.
inline int stripMaxRows(int w) { return w > 0 ? STRIP_PIXELS / w : 0; }

// Sends the open strip and returns when it's on the screen.
void stripSend();

// Work for core 0 while a strip goes out. Set it only around draws where the
// player is in a steady state (not mid-load or mid-seek), and clear it with
// nullptr afterwards. It never runs nested inside itself.
void setDrawPump(void (*pump)());

// Draw clip, for transitions. While set, strips and direct tft draws only
// change pixels inside x, y, w, h, and Look::band() skips or narrows bands
// to it. clearDrawClip() puts the whole screen back.
void setDrawClip(int x, int y, int w, int h);
void clearDrawClip();

// Cuts x, y, w, h down to the clip. False when nothing is left.
bool clipRect(int& x, int& y, int& w, int& h);

// Screen hold, for the screensaver. While held, strips aren't sent, direct
// tft draws and bands draw nothing and meters wait, so the modes keep
// running without touching the screen. holdBypass() lets the screensaver
// draw through a hold.
void setScreenHeld(bool held);
bool screenHeld();
void holdBypass(bool on);

// 0 for an ordinary draw and a transition's first step, then 1, 2... for
// its later steps, which draw the same screen again. Widgets that take a
// snapshot when drawn keep the first step's.
int drawPass();
void setDrawPass(int pass);

// True while a draw pump is set.
bool hasDrawPump();

// Runs the draw pump once if one is set and it isn't already running.
void pumpOnce();

// UI time slice. main.cpp calls sliceBeginLoop() at the top of each loop().
// Draws charge their time to the slice; once it's spent, or when the WAV
// ring is running low, sliceAllows(DRAW_METER) says no and meters wait for
// a later loop. Essential draws (text, cursor moves) always go ahead.
enum DrawKind { DRAW_ESSENTIAL, DRAW_METER };

const uint32_t UI_SLICE_US = 4000;

void sliceBeginLoop();
bool sliceAllows(DrawKind kind);

// Time left in this loop's slice, whatever the ring is doing.
bool sliceLeft();
void sliceCharge(uint32_t us);

// While on, meters always draw. For the one catch-up draw when playback
// pauses or stops, so a meter skipped on the last tick doesn't stay stale.
void sliceForceMeters(bool on);

// The WAV ring is streaming and less than a quarter full (about 46 ms). A
// stream that has hit the end of its file drains on purpose and doesn't
// count as low.
bool ringLow();

// Charges the time between construction and destruction to the slice.
struct SliceTimer {
    uint32_t start;
    SliceTimer() : start(micros()) {}
    ~SliceTimer() { sliceCharge(micros() - start); }
};

} // namespace Ui

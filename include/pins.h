#pragma once
#include <Arduino.h>

// =====================================================================
// picotracker v2 pinout
// (copied from the user-supplied pin map; the AUDIO_* macros below have
//  been fixed -- the originals used "#define X = value" which defines X
//  as the literal text "= value" and breaks anywhere it's substituted)
// =====================================================================

// ---- button pins ----
// Assumed wiring: momentary buttons to GND, using internal pull-ups
// (INPUT_PULLUP), so a pressed button reads LOW. If your board instead
// pulls buttons HIGH when pressed, flip the logic in input.cpp.
//
// Physical layout, top (nearest the screen) to bottom, left to right --
// every screen's footer button-hint text is grouped/ordered to match
// this, see e.g. drawLooper()/drawPlayer()/drawBrowser() in ui.cpp:
//   UP    PLAY  EDIT
//   LEFT  DOWN  RIGHT  ENTER
//   ALT   NAV
// (BTN_ALT/BTN_NAV read these two pins directly -- see input.cpp; there
// used to be a cross-wiring hack here, removed in favor of swapping the
// call sites instead, for readability.)
#define INPUT_LEFT  8
#define INPUT_RIGHT 10
#define INPUT_UP    11
#define INPUT_DOWN  9
#define INPUT_ALT   12
#define INPUT_EDIT  13
#define INPUT_ENTER 14
#define INPUT_NAV   15
#define INPUT_PLAY  16
#define DISPLAY_BACKLIGHT 23 // Backlight control pin

// ---- SDIO pins ----
// SdFat's RP2040 SDIO driver (SdioConfig) only needs CLK, CMD and DAT0 --
// DAT1..DAT3 are required to be the next three consecutive GPIOs after
// DAT0, which they are here (4,5,6,7). The low-level PIO/DMA state
// machine + channel numbers are allocated internally by SdFat, so the
// SDIO_PIO / *_SM / *_DMA_CH macros from the original pin map aren't
// needed by this firmware and are kept only for reference.
#define SDIO_CLK 2
#define SDIO_CMD 3
#define SDIO_D0  4
#define SDIO_D1  5 // must stay CLK/CMD/D0's neighbor - not referenced directly
#define SDIO_D2  6
#define SDIO_D3  7

// ---- AUDIO pins (not used by the MIDI player; kept for reference) ----
#define AUDIO_SDATA 17
#define AUDIO_BCLK  18 // BCLK and LRCLK HAVE to be consecutive
#define AUDIO_LRCLK 19
#define NUM_SAMPLES 32    // Number of samples in one sine wave period
#define NUM_BLOCKS  8     // Number of blocks to buffer
#define SAMPLE_RATE 44100 // Standard CD quality sample rate

// ---- MIDI pins ----
// UART0 TX/RX default to GPIO0/GPIO1 on the earlephilhower core, which
// matches this pin map, so plain Serial1 (mapped to uart0) can be used
// without re-assigning pins.
#define MIDI_BAUD_RATE 31250
#define MIDI_OUT_PIN 0
#define MIDI_IN_PIN  1

// ---- battery pin ----
#define BATTERY_VOLTAGE_PIN 29

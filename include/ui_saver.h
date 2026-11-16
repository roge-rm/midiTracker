#pragma once
#include <stdint.h>

// Screensaver views. Core 0 only. Everything draws through Look::band(),
// so each frame is a few strip sends with the draw pump running.
//
// screensaver.cpp decides when they run and which one; the host renderer
// draws them from song snapshots.
namespace UiSaver {

// What's playing.
enum Source : uint8_t { SRC_NONE, SRC_TRACKER, SRC_MIDI_FILE, SRC_WAV, SRC_SYSEX, SRC_LOOPER, SRC_MIDI_IN };

// Idle is the card shown with nothing playing.
enum View : uint8_t { VIEW_IDLE, VIEW_NOW_PLAYING, VIEW_METERS, VIEW_RAIN };

struct Info {
    Source src = SRC_NONE;
    const char* name = nullptr;
    uint32_t elapsedMs = 0;
    int progress = -1;   // 0-1000, -1 when the length isn't known
    int channels = 0;    // tracker channels
    int battery = 0;     // percent
    bool charging = false;
};

// The tracker and WAV sources have audio in the WAV ring; the others are
// MIDI notes.
inline bool hasRing(Source s) { return s == SRC_TRACKER || s == SRC_WAV; }

// Starts a view at the current song position. The screen is cleared by
// the first frames.
void start(View v, const Info& info);

// Draws the view's next frame. Clearing and Pattern rain go a strip at a
// time and stop when the slice is spent; it returns true while there's
// more to draw, so call it again on the next loop.
bool frame(const Info& info);

// Time between frames, in ms.
uint32_t frameMs(View v);

} // namespace UiSaver

#pragma once
#include <Arduino.h>

// Screen transitions for going in and out of modes and folders. Core 0 only.
//
// Call transitionNext() just before the screen changes to another mode or
// folder, and draw the new screen through transitionDraw(). With nothing
// pending, transitionDraw() just draws.
//
// Wipe sends the new screen in 16-row bands, top down going in and bottom
// up going out. Slide uses the ST7789's vertical scroll, which runs along x
// in landscape: the old screen moves off sideways with the new one behind
// it, coming in from the right going in and from the left going out. Both
// call draw once per step with the screen clipped to that step's area (see
// Ui::setDrawClip()), so draw has to draw the whole screen the same way
// each time. The scroll is back at 0 when transitionDraw() returns.
//
// Strips run the draw pump as usual, and each step runs it at least once.
// With no pump set, MIDI in is pumped (MidiOutput::update()), so draw must
// be safe with the input handler running between steps. A transition is
// skipped while the WAV ring is low, and a new key press, a low ring or a
// step over 20 ms part way finishes it at once.
namespace Ui {

enum TransitionStyle : uint8_t { TRANSITION_OFF, TRANSITION_WIPE, TRANSITION_SLIDE };
enum TransitionDir : uint8_t { TRANSITION_IN, TRANSITION_OUT };

void setTransitionStyle(TransitionStyle style);

// The next transitionDraw() runs a transition, if it comes within half a
// second.
void transitionNext(TransitionDir dir);

// Drops a transition that wasn't drawn.
void transitionCancel();

void transitionDraw(void (*draw)(void*), void* ctx);

template <class F>
void transitionDraw(F draw) {
    transitionDraw([](void* c) { (*static_cast<F*>(c))(); }, &draw);
}

} // namespace Ui

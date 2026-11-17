#pragma once
#include <stdint.h>

// The player screens: tracker (MOD, S3M, XM, IT), .mid and WAV. Core 0 only.
//
// The screens take plain info structs, so ui.cpp fills them from the
// players and the host renderer from a render. The live parts read the
// players' hand-overs in ui_tap.h, and the scope, waveform and peak meters
// read what's about to play from the WAV ring (Synth::wavStreamPeek()).
//
// The *Live() calls can come every loop: each part keeps its own rate and
// draws only when it changed. Meters, lanes, scope and waveform are
// DRAW_METER draws, so they wait while the slice is spent or the ring runs
// low; time, position and state always draw.
namespace UiPlayer {

enum PlayState : uint8_t { PS_IDLE, PS_PLAYING, PS_PAUSED, PS_STOPPED, PS_DONE, PS_ERROR };

struct TrackerInfo {
    const char* name;
    const char* format; // "XM"
    int channels, instruments;
    int songLength, order, row;
    int speed, tempo;
    uint32_t elapsedMs;
    PlayState state;
    const char* error;
    int volume;
};

struct MidiInfo {
    const char* name;
    int format, tracks;
    uint32_t elapsedMs;
    unsigned bpm;
    int tempoPct;
    PlayState state;
    const char* error;
    int volume;
    const char* out; // "HW+USB"
    bool audioOn;
};

struct WavInfo {
    const char* name;
    uint32_t fileKey; // a hash of the full path
    uint32_t elapsedMs, totalMs;
    uint32_t sampleRate;
    int channels;
    const char* type;   // "WAV" or "MP3", in the status strip
    const char* format; // after the rate: "16BIT", "320K CBR"
    PlayState state;
    const char* error;
    int volume;
};

void drawTracker(const TrackerInfo& t);
void trackerState(PlayState s);
void trackerLive(const TrackerInfo& t);

void drawMidi(const MidiInfo& m);
void midiState(PlayState s);
void midiLive(const MidiInfo& m);
void midiOutput(const char* out, bool audioOn);

void drawWav(const WavInfo& w);
void wavState(PlayState s);
void wavLive(const WavInfo& w);

// True once a tracker or WAV screen has nothing left to show from the ring:
// it has played out and the meters are down. Keep the *Live() calls going
// after the end until then.
bool liveSettled();

// The volume readout on whichever player screen is up.
void setVolume(int volume);

// The play mode tag in the status strip ("ONCE", "RPT", "DIR"), lit for a
// mode that goes on past the end, kept for the next draw*(). With redraw,
// also drawn now on the player screen that was drawn last, so only pass it
// while that screen is up. nullptr shows none.
void setPlayMode(const char* tag, bool lit, bool redraw = false);

// The keylock: a lock icon in the status strip and an unlock-only footer,
// kept for the next draw*(). With redraw, also drawn now on the player
// screen that was drawn last.
void setLocked(bool locked, bool redraw = false);

// A note in the footer, such as "Locked", kept for the next draw*() until
// set to nullptr. With redraw, the footer is also drawn now on the player
// screen that was drawn last. The rest of the screen keeps drawing.
void setLockNote(const char* note, bool redraw = false);

// The .mid screen's keybed somewhere else (pariSynth): y and h take in its
// two rule lines. keybedDraw() puts it up with the notes down now, and
// keybedUpdate() repaints only the keys that changed since. Drawing a
// keybed leaves the player screen, so the *Live() calls stop until the next
// draw*().
void keybedDraw(int y, int h);
void keybedUpdate();

// A tracker note as the lanes show it: "C#4", "===", or dots for none.
// out holds 4 bytes.
void formatNote(uint8_t note, char* out);

// The next frames to play from the WAV ring, stereo, into a buffer shared
// with the player screens. Returns the frame count.
int peek(const int16_t*& frames);

} // namespace UiPlayer

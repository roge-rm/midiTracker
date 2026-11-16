#pragma once
#include <stdint.h>
#include <stddef.h>

// What the players hand the player screens (and the screensaver): the rows
// a tracker just played, already decoded, per-channel levels, and MIDI
// note-ons per channel. No extra SD reads.
//
// Everything here is written and read on core 0: the tracker mixers and
// the MIDI player run there, as does the UI. The draw pump runs a player's
// update() inside a strip send, so the UI copies what it needs out before
// it draws.
//
// Rows and levels are stamped with the WAV ring's write count at the time
// they were made. The ring runs up to 186 ms ahead of what's heard, so the
// UI shows the newest entry at or before Synth::wavStreamPlayed().
namespace UiTap {

const int LANES = 16;     // tracker channels kept, the first 16
// The 8192-frame ring holds 19 ticks at the shortest (tempo 255, 432
// frames), so 32 covers what's queued plus the 3-row trail at speed 1.
const int ROW_RING = 32;
const int LEVEL_RING = 32;

// Notes: 1-120 is C-0 to B-9, so (n - 1) % 12 is the name and
// (n - 1) / 12 the octave.
const uint8_t NOTE_NONE = 0, NOTE_FADE = 0xFC, NOTE_CUT = 0xFD, NOTE_OFF = 0xFE;

struct Row {
    uint32_t at;     // ring write count when the row started
    uint16_t order;
    uint16_t row;
    uint8_t note[LANES];
    uint8_t inst[LANES]; // 0 = none
};

// Channel levels over one tick, 0-255, roughly sample peak times gain.
struct Levels {
    uint32_t at;
    uint8_t level[LANES];
};

struct Tracker {
    Row rows[ROW_RING];
    uint32_t rowCount;  // rows published since the reset, rows[(rowCount - 1) % ROW_RING] is the newest
    Levels levels[LEVEL_RING];
    uint32_t levelCount;
    Row pending;        // the row being decoded
    uint32_t tickAt;    // ring write count at the start of the tick being mixed
};
extern Tracker g_tracker;

// Called by the tracker players.
void trackerReset();
inline void rowBegin() {
    for (int i = 0; i < LANES; i++) g_tracker.pending.note[i] = g_tracker.pending.inst[i] = 0;
}
inline void cell(int ch, uint8_t note, uint8_t inst) {
    if ((unsigned)ch < (unsigned)LANES) {
        g_tracker.pending.note[ch] = note;
        g_tracker.pending.inst[ch] = inst;
    }
}
void rowEnd(uint32_t at, int order, int row);
inline bool rowEmpty() {
    for (int i = 0; i < LANES; i++)
        if (g_tracker.pending.note[i] || g_tracker.pending.inst[i]) return false;
    return true;
}
// Returns the slot to fill for the tick that started at g_tracker.tickAt;
// call levelsDone() once it's filled.
inline Levels& levelsSlot() { return g_tracker.levels[g_tracker.levelCount % LEVEL_RING]; }
inline void levelsDone(uint32_t nextTickAt) {
    g_tracker.levels[g_tracker.levelCount % LEVEL_RING].at = g_tracker.tickAt;
    g_tracker.levelCount++;
    g_tracker.tickAt = nextTickAt;
}

// Newest row and levels at or before `played`, false when there are none
// yet (all of them still to be heard).
bool rowAt(uint32_t played, Row& out);
bool rowsBefore(uint32_t played, Row* out, int n, int& got); // newest last
bool levelsAt(uint32_t played, Levels& out);

// MIDI, from MidiOutput::sendNoteOn() on core 0.
struct Midi {
    uint8_t hits[16]; // note-ons per channel, wraps
    uint8_t vel[16];  // last note-on velocity
    uint8_t note[16]; // last note-on note
};
extern Midi g_midi;
inline void midiNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    if (ch < 16 && vel) {
        g_midi.hits[ch]++;
        g_midi.vel[ch] = vel;
        g_midi.note[ch] = note;
    }
}

} // namespace UiTap

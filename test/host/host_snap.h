// Snapshots of the UI hand-overs from a host render, read by look_render to
// draw the player screens with real song data.
#pragma once
#include "ui_tap.h"

namespace HostSnap {

struct Tracker {
    char magic[4]; // "TSNP"
    int channels, instruments, songLength, order, row, speed, tempo;
    uint32_t written; // ring write count, also the frame index in the .raw
    UiTap::Tracker tap;
};

// One frame of the .mid player's state, FRAMES of them 40 ms apart.
struct MidiFrame {
    uint32_t elapsedMs;
    unsigned bpm;
    UiTap::Midi tap;
    uint16_t noteMask[128];
    uint8_t noteVel[128];
};

struct Midi {
    char magic[4]; // "MSNP"
    int format, tracks;
    static const int FRAMES = 8;
    MidiFrame frames[FRAMES];
};

} // namespace HostSnap

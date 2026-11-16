#pragma once
#include <stdint.h>

// The looper and pariSynth screens. Core 0 only.
//
// Both play live, so the periodic calls draw only what changed, in small
// strips, at capped rates, and their meters and lanes are DRAW_METER draws
// that wait while the slice is spent or the ring runs low. ui.cpp fills
// the structs from LooperMode and the synth; the host renderer fills them
// with made-up data.
namespace UiModes {

// ---- looper ---------------------------------------------------------------

enum LoopTrackState { LOOP_TRACK_EMPTY, LOOP_TRACK_ARMED, LOOP_TRACK_RECORDING, LOOP_TRACK_PLAYING, LOOP_TRACK_MUTED, LOOP_TRACK_STOPPED, LOOP_TRACK_PAUSED };

// One track as the screen shows it.
struct LoopTrackView {
    uint8_t channelIn = 0;   // 0-15, or 16 for OMNI
    uint8_t channelOut = 0;  // 0-15, or 16 for as recorded
    LoopTrackState state = LOOP_TRACK_EMPTY;
    int barLength = 0;       // 0 = freeform, else bars
    uint32_t lengthMs = 0;   // 0 while empty, armed or on an open first pass
    uint16_t pos = 0;        // position in the loop, 0-65535 for 0 to the end
    bool recordActive = false; // captured an event just now
    bool playActive = false;   // sent an event just now
    bool bufferFull = false;   // events are being dropped
};

// The BPM block: the shared tempo, time signature, metronome, count-in and
// sync, one of them focused while the cursor is on the block.
struct LoopBpmRow {
    int bpm = 120;
    int sigNum = 4, sigDen = 4;
    bool sync = false;
    bool metroOn = false;
    int metroVolume = 0;
    bool showMetroVolume = false; // METRO shows the volume while EDIT is held
    bool countIn = false;
    int countInBars = 1;
    bool showCountInBars = false; // COUNT shows the bars while EDIT is held
    int focus = 0;                // 0 BPM, 1 time sig, 2 metro, 3 count in, 4 sync
    bool selected = false;        // the cursor is on the block
    uint32_t barMs = 2000;        // for the lane ticks
};

void looperDraw(const LoopTrackView t[4], int selectedTrack, const LoopBpmRow& row, bool showBeats, int beat);

// Playheads, fills, event dots and activity lights. Call at the frame
// rate, 40 ms; the looper keeps that rate. selectedTrack -1 when the BPM
// block has the cursor.
void looperLive(const LoopTrackView t[4], int selectedTrack);

// A whole track row, after a state, channel or length change.
void looperTrackRow(const LoopTrackView& t, int idx, bool selected);

// Just the label, state, route and bar length on the left of a row.
void looperTrackInfo(const LoopTrackView& t, int idx, bool selected);

// Just a track's time, and its lane if the tick count changed (a BPM change).
void looperTrackLength(const LoopTrackView& t, int idx, bool selected);

// Moves the cursor between rows and the BPM block. The footer changes with it.
void looperSelection(const LoopTrackView t[4], int prevSel, bool prevOnBpm, int newSel, bool newOnBpm,
                     const LoopBpmRow& row);

void looperBpm(const LoopBpmRow& row);

// A beat flash around the BPM block, amber or the downbeat colour; off
// puts the block's own edge back.
void looperBeatFlash(bool active, bool downbeat);

// The beat squares in the status strip. beat -1 lights none.
void looperBeats(bool visible, int beat, int num);

// BAR and BEAT on the right of the BPM block, 1-based; 0 shows dashes.
void looperBarBeat(int bar, int beat);

// Count-in screen.
void looperCountIn(int beatsLeft);
void looperCountInBeat(int beatsLeft);

// Event dots for a lane, built by the looper whenever a track's events
// change: begin with the loop length in ms, add each note-on at its offset,
// then end, which repaints only the columns that changed. One lane at a
// time; laneDot is a multiply and a shift, so the walk can be spread over
// loop passes.
void laneDotsBegin(int track, uint32_t lengthMs);
void laneDot(uint32_t offsetMs, uint8_t note);
void laneDotsEnd();

// ---- pariSynth ------------------------------------------------------------

struct SynthCell {
    const char* name; // instrument family, "Drums" on ch 10
    uint8_t family;   // 0-15, 16 for drums
};

// cells has 16 entries. activeMask has a bit per channel with notes down.
void synthDraw(const SynthCell* cells, int selected, uint16_t activeMask);

// One cell, after a program or held-note change.
void synthCell(int ch, const SynthCell& cell, bool selected, bool active);

// Cell meters and the keybed. Call every loop; 40 ms rate.
void synthLive();

// VOL over channel 9's cell while EDIT+UD changes it. Redrawing that cell
// takes it away.
void synthVolume(int volume);

uint16_t synthDrawnActiveMask();

} // namespace UiModes

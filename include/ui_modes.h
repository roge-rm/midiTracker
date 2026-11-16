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

// ---- synth editor -----------------------------------------------------------

// The instrument picker: 16 families then 8 drums, a family icon on each
// row. families[i] is 0-15, or 16 for a drum.
const int SYNTH_PICK_ROWS = 10;
void synthPickDraw(const char* const* labels, const uint8_t* families, int count, int cursor, int scrollOffset);

// Repaints the two rows the cursor moved between, same scroll.
void synthPickCursor(const char* const* labels, const uint8_t* families, int count, int prevCursor,
                     int newCursor, int scrollOffset);

// What a field drives in the panel on the right.
enum SynthEditPart : uint8_t {
    PART_NONE,
    PART_WAVE,
    PART_ATTACK, PART_DECAY, PART_SUSTAIN, PART_RELEASE, // melodic envelope
    PART_AMP, PART_PITCH,                                // drum envelope
};

const int SYNTH_EDIT_MAX_ROWS = 9;

struct SynthEditRow {
    const char* label;
    char value[12];
    int16_t fill;      // bar, 0-1000, -1 for none
    SynthEditPart part;
};

// One instrument or drum as the field editor shows it: the field list on the
// left, the envelope and one cycle of the wave on the right.
struct SynthEditView {
    const char* name;  // "Piano", "Drum:Kick"
    uint8_t family;    // 0-15, 16 for drums
    bool drum;
    uint8_t wave;      // 0 sine, 1 triangle, 2 saw, 3 square, 4 noise
    uint16_t attack, decay, release; // samples
    uint8_t sustain;                 // percent
    uint16_t dropTime;               // drum pitch drop, samples (decay is the amp decay)
    int16_t pitchFrom, pitchTo;      // drum pitch at the start and end of the drop, 0-1000
    int count;
    SynthEditRow rows[SYNTH_EDIT_MAX_ROWS];
};

void synthEditDraw(const SynthEditView& v, int cursor);

// Moves the cursor: the two rows, and the panel parts either row drives.
void synthEditCursor(const SynthEditView& v, int prevCursor, int newCursor);

// After the value at idx (the cursor's row) changes: that row and the panel
// part it drives.
void synthEditValue(const SynthEditView& v, int idx);

// ---- synth editor bank screens ----------------------------------------------
// bankName is the loaded bank, "" for the built-in sounds.

// The bank menu: Save, Load, Reset.
const int SYNTH_BANK_ITEMS = 3;
void synthBankDraw(const char* bankName, int cursor);
void synthBankCursor(int prevCursor, int newCursor);

// The saved banks, SYNTH_PICK_ROWS to a page. An empty list shows a card.
void synthLoadDraw(const char* const* names, int count, int cursor, int scrollOffset, const char* bankName);
void synthLoadCursor(const char* const* names, int count, int prevCursor, int newCursor, int scrollOffset,
                     const char* bankName);

// Naming a bank: the name in big cells over the KEYBOARD_ROWS grid. error
// shows in red over the name, null for none.
const int SYNTH_NAME_MAX = 16;
void synthNameDraw(const char* name, const char* error, int keyRow, int keyCol);
void synthNameKey(int prevRow, int prevCol, int newRow, int newCol);
// After a key typed or deleted: the cells from prevLen to the new length.
void synthNameText(const char* name, int prevLen);
void synthNameError(const char* name, const char* error);

// Cards over the screen below.
void synthOverwriteCard(const char* name);
void synthResultCard(const char* title, const char* line, bool ok);

} // namespace UiModes

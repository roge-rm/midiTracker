#include "looper_mode.h"

#include <SdFat.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <new> // std::nothrow, see tracks' declaration below

#include "sd_card.h"
#include "sd_browser.h"
#include "keyboard_layout.h"
#include "midi_output.h"
#include "input.h"
#include "ui.h"
#include "config.h"
#include "synth.h"
#include "settings_mode.h"

// A 4-track MIDI looper. Each track is filtered to one MIDI channel: any
// channel-voice message arriving on that channel gets captured into that
// track (and echoed live so you can hear yourself while recording); other
// channels are ignored by that track. Multiple tracks can record at once.
//
// This intentionally does NOT reuse MidiRecorder/MidiPlayer -- those are
// built around a single continuous stream to/from an SD file, whereas a
// live loop needs an in-RAM buffer that supports overdub (merging new
// events into an already-looping cycle), mute, and erase, none of which
// map onto "append to a file" or "read a file forward once". SD is only
// touched at explicit Save time, where each track's buffer is serialized
// to its own small SMF (reusing the same var-len/MThd/MTrk writing
// approach MidiRecorder uses, just as a one-shot whole-buffer dump
// instead of a stream).
namespace LooperMode {
namespace {

const int NUM_TRACKS = 4;
// 4096 * 8 bytes/event = 32KB/track, 128KB total for 4 tracks -- chosen to
// leave a comfortable ~34% RAM margin for stack/heap/USB-audio-SD runtime
// overhead at 4 tracks specifically; an 8-track future would need this
// revisited rather than assumed to still fit.
const int MAX_EVENTS_PER_TRACK = 4096;
const char* LOOPS_ROOT = "/loops";
// Bundle (whole-session) saves live one level down from individual loop
// saves -- LOOPS_ROOT itself holds loose single-track .mid files (see
// saveSelectedLoop()), while BUNDLES_ROOT holds the LOOPnnn folders
// saveSession() has always written (just relocated a level deeper, same
// naming convention).
const char* BUNDLES_ROOT = "/loops/bundles";
const uint8_t OMNI_CHANNEL = 16; // sentinel input-channel value: matches any incoming channel
const uint8_t CHANNEL_AS_RECORDED = 16; // sentinel output-channel value: send each event on whatever channel it was captured on, unremapped

enum TrackState { TRACK_EMPTY, TRACK_ARMED, TRACK_RECORDING, TRACK_PLAYING, TRACK_MUTED, TRACK_STOPPED, TRACK_PAUSED };

struct LoopEvent {
    uint32_t offsetMs; // position within the loop cycle, 0..lengthMs-1
    uint8_t status, data1, data2;
};

struct LoopTrack {
    uint8_t channelIn = OMNI_CHANNEL; // 0-15, or OMNI_CHANNEL (default: record from any incoming channel)
    // 0-15, or CHANNEL_AS_RECORDED (send each event on whatever channel it
    // was captured on -- see updatePlayback()). Set to a real per-track
    // default (not CHANNEL_AS_RECORDED) right after tracks[] is allocated,
    // see enter() -- a default member initializer here can't vary by this
    // track's own index.
    uint8_t channelOut = 0;
    TrackState state = TRACK_EMPTY;

    // 0 = freeform (current recording pass, or all prior passes, just ran
    // until manually stopped); otherwise the fixed length this track
    // records into, in bars against manualBpm/timeSigNum/timeSigDen -- see
    // barsToMs() and updateBarQuantizedRecording(). Sticky across erase/
    // re-record so picking a length once doesn't need re-picking every take.
    int barLength = 0;

    uint32_t lengthMs = 0;     // 0 until this track's first recording pass is committed
    uint32_t epochMs = 0;      // this track's own position-zero reference (Independent mode)
    uint32_t recordStartMs = 0; // valid while state==TRACK_RECORDING and lengthMs==0 (the open-ended first pass)

    // Shared by TRACK_STOPPED and TRACK_PAUSED -- both freeze position the
    // same way (see elapsedRunMs()), and a track is never both at once.
    uint32_t stopAccumMs = 0; // total time spent frozen so far, excluded from position calc
    uint32_t stopStartMs = 0; // when the current frozen period began (valid while STOPPED or PAUSED)
    // Set while a Sync-mode restart is waiting for a clock boundary
    // instead of happening immediately (see togglePauseOnSelected()/
    // updatePendingResumes()) -- two cases, told apart by `state` at the
    // time it's checked: PAUSED means "resume in place" (waiting for the
    // next beat, see nextBeatBoundaryAfter()); STOPPED means "restart
    // from the top" for a bar-quantized track (waiting for the next bar,
    // see nextBarBoundaryAfter()). pendingResumeAtMs is a fixed target
    // computed once, when resumePending is set -- it must NOT be
    // recomputed fresh from "now" on every poll, since both
    // nextBeatBoundaryAfter()/nextBarBoundaryAfter() are defined to
    // always return a point strictly after whatever `now` they're given,
    // so re-deriving either from an ever-advancing `now` would never
    // actually be reached (a moving target chasing itself).
    bool resumePending = false;
    uint32_t pendingResumeAtMs = 0;

    // millis() this track last captured an incoming event / last sent an
    // event out during playback, 0 = never. Separate because a track can
    // be doing both at once (overdub: TRACK_RECORDING still plays back
    // its existing content -- see updatePlayback()), and the two are
    // shown as distinct color-coded indicators (see buildTrackViews()).
    uint32_t lastRecordActivityMs = 0;
    uint32_t lastPlaybackActivityMs = 0;

    uint32_t prevPos = 0; // last computed playback position, for wrap detection
    int playCursor = 0;   // index of the next not-yet-fired event this cycle

    LoopEvent events[MAX_EVENTS_PER_TRACK];
    int eventCount = 0;
    bool bufferFull = false; // events[] hit MAX_EVENTS_PER_TRACK and further events are being silently dropped
};

// Allocated lazily by enter() (see below), not a permanent static global
// -- at 131KB+ this is by far the single largest RAM consumer in the
// firmware, and stays resident through ordinary trips to Settings/Mode
// Select rather than being freed on every exit. Only freed via
// freeTracksIfAllocated() (see looper_mode.h), which main.cpp calls at
// the one point that actually needs the RAM back: entering File Player
// mode. All 37 existing tracks[i]/LoopTrack& call sites throughout this
// file are untouched by this -- T* and T[N] support operator[] identically.
LoopTrack* tracks = nullptr;
bool tracksAllocFailed = false; // set by enter() if the lazy allocation fails; see update()'s first line
int selectedTrack = 0;
// The row cursor is really 5 positions (4 tracks + BPM), circular --
// selectedTrack always holds a valid track index (0..3) regardless, and
// onBpmRow separately tracks whether the *cursor* is actually parked on
// the BPM row right now. Kept separate rather than folding BPM into
// selectedTrack's range so every existing tracks[selectedTrack] access
// stays valid without a bounds check at every call site.
bool onBpmRow = false;

bool syncMode = true; // Sync (shared epoch) vs Independent (per-track epoch)
uint32_t sessionEpochMs = 0; // Sync mode's shared position-zero reference
bool sessionEpochSet = false;
// Used both for display and to compute bar-quantized track lengths (see
// barsToMs()). Still not derived from incoming MIDI clock -- see the
// header comment's note on that being a deferred piece.
float manualBpm = 120.0f;

// Time signature: numerator = beats (clicks) per bar, denominator = which
// note value counts as one beat (4 = quarter, 8 = eighth, etc). Applied
// literally rather than modeling compound-meter "feel" (e.g. 6/8 clicks
// 6 times a bar here, not twice at a dotted-quarter pulse) -- enough for
// what the metronome/count-in/bar-quantized recording actually need
// (a click grid and a bar length), without the added complexity compound
// meter's alternate beat-grouping would bring for little payoff on a
// tracker. See barsToMs(), updateMasterClock(), updateCountIn().
struct TimeSig { int num, den; };
const int TIME_SIG_PRESET_COUNT = 6;
const TimeSig TIME_SIG_PRESETS[TIME_SIG_PRESET_COUNT] = {
    {4, 4}, {3, 4}, {2, 4}, {6, 8}, {5, 4}, {7, 8},
};
int timeSigNum = 4;
int timeSigDen = 4;

// Finds timeSigNum/timeSigDen's index within TIME_SIG_PRESETS -- falls
// back to 4/4 (index 0) if the current value somehow isn't one of the
// presets (e.g. a hand-edited or corrupted settings/session file), same
// graceful-fallback approach readSessionMeta() uses elsewhere.
int timeSigPresetIndex() {
    for (int i = 0; i < TIME_SIG_PRESET_COUNT; i++) {
        if (TIME_SIG_PRESETS[i].num == timeSigNum && TIME_SIG_PRESETS[i].den == timeSigDen) return i;
    }
    return 0;
}

// -- metronome ------------------------------------------------------------
// Off by default; ticks over the onboard synth's audio-out only (never
// HW/USB MIDI -- see updateMasterClock()), on the same bar grid
// sessionEpochMs already anchors bar-quantized recording to, so it stays
// musically locked to whatever's being recorded/played regardless of
// Sync vs Independent (a metronome is inherently one shared clock, not a
// per-track thing).
bool metronomeOn = false;
// The "master clock" -- this session's one shared beat/bar position,
// independent of whether the audible click (metronomeOn) is switched on:
// it's what actually drives the header's beat-indicator squares and the
// BPM row's beat-flash for a running loop even with Metro off (see
// updateMasterClock()'s header comment), so "the metronome" isn't quite
// the right name for it even though updateMasterClock() is also what
// sounds the click when Metro is on. masterClockNextDueMs runs
// continuously (advancing every beat while something's running) instead
// of being recomputed from scratch on every call; 0 means the clock isn't
// running at all right now (nothing to restart in place -- see
// restartMasterClockFromStart() for how a fresh start is told apart from
// resuming something already in progress).
uint32_t masterClockNextDueMs = 0; // wall-clock deadline for the next beat; 0 = not running
int masterClockBeatInBar = 0;      // which beat of the bar masterClockNextDueMs is (0 = downbeat)
int metronomeVolumePercent = 80; // scales METRONOME_VELOCITY(_DOWNBEAT) below -- see SettingsMode::metronomeVolume()
const uint8_t METRONOME_CHANNEL = 9;   // GM percussion (MIDI channel 10) -- see PERCUSSION_CHANNEL in synth.cpp
const uint8_t METRONOME_NOTE = 76;     // GM Hi Wood Block -- a resonant "knock" (synth.cpp's classifyDrum()), trying this in place of the previous Closed Hi-Hat tick
const uint8_t METRONOME_VELOCITY = 90;
// Exactly 40% louder than METRONOME_VELOCITY on beat 1 of each bar (both
// still get scaled by metronomeVolumePercent below, so the ratio between
// them holds regardless of the overall metronome volume setting).
const uint8_t METRONOME_VELOCITY_DOWNBEAT = (uint8_t)(METRONOME_VELOCITY * 1.40f + 0.5f);

// Forces the master clock to the downbeat, right now -- the "restart from
// the beginning" half of this app's one rule for it: a *fresh* start
// (arming a track and triggering it with nothing else already
// running/counting-in, starting a click-only pre-roll, or resuming a
// Stopped -- not Paused -- track with nothing else running) always begins
// at bar 1 beat 1, never wherever stale elapsed session time would
// otherwise put it. Every call site below gates this behind
// `masterClockNextDueMs == 0` first (checked *before* anything that
// call site might do could change what's advancing) -- that's what tells
// a genuine fresh start apart from joining/continuing something already
// running, which must never get yanked out of phase by this. A resumed
// Pause is the one case that deliberately does NOT call this at all (see
// togglePauseOnSelected()/finishResumeFromPause()): that path leaves the
// master clock to resync itself from real elapsed time instead, which is
// what makes it resume in place, in phase with wherever the track it's
// unfreezing actually left off.
void restartMasterClockFromStart(uint32_t now) {
    masterClockBeatInBar = 0;
    masterClockNextDueMs = now;
}

// -- count-in -------------------------------------------------------------
// Only ever triggers when metronomeOn && countInEnabled; when the
// metronome is on but count-in is off, arming a track still just waits
// for its first note the way it always has (see handleIncomingMidi()) --
// PLAY can still start the click running early, though, see
// startMetronomeClickOnly() just below.
bool countInEnabled = true;   // see SettingsMode::defaultCountInEnabled()
int countInBars = 1;          // see SettingsMode::defaultCountInBars()
bool countInActive = false;
uint32_t countInStartMs = 0;
int countInBeatShown = -1;    // -1 = count-in not currently showing anything yet

// True while a "click-only" pre-roll is running -- Metro's click ticking
// (and the shared epoch/clock established) with nothing actually armed
// yet or already recording, so the user can start hearing/feeling the
// tempo before committing to anything. Only reachable when Metro is on
// but Count In is off (see startMetronomeClickOnly(), PLAY's own handler
// in updateMainScreen()) -- with Count In on, that overlay already serves
// this same "hear the tempo before you play" purpose. Keeps
// updateMasterClock()'s clock running the same way countInActive/
// anyTrackAdvancing() do; cleared the moment real recording actually
// starts (see startArmedTracksTogether()) or Metro turns back off.
bool metronomeClickOnlyActive = false;

// The BPM row packs five independently-adjustable things into one row --
// this is which of them ALT+UP/DOWN/LEFT/RIGHT currently targets.
// RIGHT/LEFT (without ALT) step forward/back through them in this same
// order; reset to BPM_FOCUS_VALUE whenever the row cursor leaves or
// re-enters the BPM row (see handleRowCursor()) so it never lands
// somewhere stale. See handleBpmRowFocus()/handleBpmAdjust(). Sync mode
// lives here now, not as its own RIGHT-tap on a track row -- same
// on/off-style ALT+UP/DOWN gesture as Metronome/Count In, just a
// two-name toggle (Sync/Independent) instead of On/Off. Time signature
// sits right after the BPM value, before Metro -- ALT+UP/DOWN cycles
// TIME_SIG_PRESETS (clamped at either end, not wrapped, same convention
// SettingsMode's Loop Length preset cycling uses).
enum BpmRowFocus { BPM_FOCUS_VALUE, BPM_FOCUS_TIME_SIG, BPM_FOCUS_METRONOME, BPM_FOCUS_COUNT_IN, BPM_FOCUS_SYNC };
BpmRowFocus bpmRowFocus = BPM_FOCUS_VALUE;

// Header beat-indicator row's own visibility -- independent of metronomeOn
// (see updateMetronomeBeatIndicator()), so the squares stay available as a
// static time-signature reference even with the metronome's click off. On
// by default; toggled with an EDIT tap while BPM value has focus (see
// handleBpmRowEditToggle()).
bool showBeatIndicator = true;

// How long the BPM row's beat-flash outline stays lit (see
// updateBpmRowBeatFlash()) -- brief enough to read as a pulse rather than
// a steady state even at the fastest supported tempo (300 BPM = 200ms/beat),
// short of the header beat-square flash's own 250ms ACTIVITY_FLASH_MS
// precedent since that one isn't competing with the next beat's timing.
const uint32_t BPM_ROW_BEAT_FLASH_MS = 100;
uint32_t bpmRowFlashUntilMs = 0; // 0 = not currently flashing

bool needsRedraw = true;

bool enterUsedAsModifier = false; // disambiguates ENTER-tap (open menu) from ENTER-held+PLAY (erase), see updateMainScreen()
bool editUsedForBarLength = false; // disambiguates EDIT-tap (arm/record) from EDIT-held+LEFT/RIGHT (cycle bar length), see handleBarLengthAdjust()/handleRecordInput()

// How long a track's MIDI-activity indicator stays lit after it actually
// captures an incoming event -- see recordEvent() and buildTrackViews().
const uint32_t ACTIVITY_FLASH_MS = 250;

// The looper's own small screen stack, separate from the top-level
// FilePlayer/Looper mode switch in main.cpp. SCREEN_MAIN is the always-
// visible 4-track view; everything else is a modal prompt/menu that
// suspends the normal track controls while showing.
enum Screen {
    SCREEN_MAIN,
    SCREEN_EXIT_CONFIRM,   // "loops are playing, exit anyway?"
    SCREEN_MENU,           // ENTER-tap: Save / Load / Erase Track N / Erase All Tracks / Delete Loop Bundle
    SCREEN_SAVE_MENU,      // Save Selected Loop / Save All Loops
    SCREEN_SAVE_LOOP_NAME, // on-screen keyboard: name the individual loop file (Save Selected Loop)
    SCREEN_SAVE_LOOP_OVERWRITE, // "overwrite X.mid?" -- the typed name collides with an existing loop file
    SCREEN_SAVE_CONFIRM,   // "save all loops?" (Save All Loops -- the bundle, same behavior as the old plain Save)
    SCREEN_ERASE_CONFIRM,  // "erase Track N?" -- the track selected when the menu was opened
    SCREEN_ERASE_ALL_CONFIRM, // "erase all 4 tracks?"
    SCREEN_DELETE_BROWSE,  // pick a saved bundle to delete (Delete Loop Bundle)
    SCREEN_DELETE_CONFIRM, // "delete LOOPnnn?"
    SCREEN_LOAD_MENU,      // Load Selected Loop / Load All Loops
    SCREEN_LOAD_LOOP_BROWSE, // restricted /loops browser (Load Selected Loop): tap selects/enters, hold ENTER deletes
    SCREEN_LOAD_LOOP_DELETE_CONFIRM, // "delete X.mid?" -- reached by holding ENTER in the browser above
    SCREEN_LOAD_BROWSE,    // pick a saved bundle to load from (Load All Loops)
    SCREEN_LOAD_PICK,      // pick "Load All" or one track within that bundle
    SCREEN_LOAD_CONFIRM,   // "load all from LOOPnnn?" / "load Track X into Track Y?"
    SCREEN_IMPORT_CONFIRM, // "import FILE.mid into Track N?" -- requestImport()'s landing screen
    SCREEN_FLASH_MESSAGE,  // brief result message ("Saved as...", "Deleted...", etc), then back to SCREEN_MAIN
};
Screen screen = SCREEN_MAIN;
uint32_t flashUntilMs = 0; // valid while screen == SCREEN_FLASH_MESSAGE

// Menu labels are built fresh each time the menu opens (see updateMainScreen())
// rather than being constant, since "Erase Track N" names whichever track
// was selected at that moment. BPM lives directly on the main screen (see
// onBpmRow) rather than in this menu, since it's a shared/global setting,
// not a per-track one -- same reasoning is why bar length lives on each
// track row (EDIT+LEFT/RIGHT, see handleBarLengthAdjust()) instead of here.
const int MENU_COUNT = 5;
char menuEraseLabel[20];   // "Erase Track N"
const char* menuLabels[MENU_COUNT];
int menuCursor = 0;

// Bar-length presets, cycled by EDIT+LEFT/RIGHT on the selected track (see
// handleBarLengthAdjust()) -- index 0 (0 bars) means Freeform, matching
// LoopTrack::barLength's convention.
const int BAR_PRESET_COUNT = 9;
const int BAR_PRESETS[BAR_PRESET_COUNT] = {0, 1, 2, 4, 8, 16, 32, 64, 128};

const int MAX_SAVED_SESSIONS = 32; // cap on how many saved loop folders the delete/load browsers list
char savedSessionNames[MAX_SAVED_SESSIONS][16];
int savedSessionCount = 0;
int deleteCursor = 0;

// -- SCREEN_LOAD_BROWSE / SCREEN_LOAD_PICK state -------------------------
// Reuses savedSessionNames/savedSessionCount (populated by
// loadSavedSessions(), same as the delete browser) with its own cursor.
int loadSessionCursor = 0;
char loadSessionName[16]; // session picked in SCREEN_LOAD_BROWSE, used by SCREEN_LOAD_PICK/CONFIRM

// SCREEN_LOAD_PICK's list: "Load All" plus one entry per track the chosen
// session actually has a .mid for (a session saved with an empty track
// never got a file for it -- see saveSession()). loadPickTrackNums[k] is
// the *saved* track number (1-4) that pick-list index (k+1) refers to --
// entry 0 ("Load All") has no corresponding number.
int loadPickTrackNums[NUM_TRACKS];
char loadPickTrackLabels[NUM_TRACKS][10]; // "Track N"
const char* loadPickLabels[NUM_TRACKS + 1];
int loadPickLabelCount = 0;
int loadPickCursor = 0;

// -- SCREEN_SAVE_MENU / SCREEN_LOAD_MENU state ---------------------------
// Two-item submenus opened from SCREEN_MENU's Save/Load rows -- see
// updateMenu(). Neither wraps at the ends, same convention as SCREEN_MENU
// itself.
const int SAVE_MENU_COUNT = 2;
const char* const saveMenuLabels[SAVE_MENU_COUNT] = { "Save Selected Loop", "Save All Loops" };
int saveMenuCursor = 0;

const int LOAD_MENU_COUNT = 2;
const char* const loadMenuLabels[LOAD_MENU_COUNT] = { "Load Selected Loop", "Load All Loops" };
int loadMenuCursor = 0;

// -- SCREEN_SAVE_LOOP_NAME / SCREEN_SAVE_LOOP_OVERWRITE state ------------
// A local re-implementation of FilePlayerMode's shared on-screen-keyboard
// name entry (same Ui::drawNameEntry()/KEYBOARD_ROWS grid), rather than a
// shared helper -- FilePlayerMode's version is tangled up with its own
// NamePurpose/browser-relative-path machinery that doesn't apply here
// (this only ever names one thing: an individual loop file at LOOPS_ROOT).
const int LOOP_NAME_MAX_LEN = 16;
char loopSaveNameBuf[LOOP_NAME_MAX_LEN + 1] = {0};
int loopSaveNameLen = 0;
int loopSaveKeyRow = 0;
int loopSaveKeyCol = 0;
char loopSaveError[40] = {0};
char pendingLoopSaveName[LOOP_NAME_MAX_LEN + 1] = {0}; // trimmed name pending SCREEN_SAVE_LOOP_OVERWRITE's confirm

// -- SCREEN_LOAD_LOOP_BROWSE / SCREEN_LOAD_LOOP_DELETE_CONFIRM state -----
// An independent SdBrowser instance rooted at LOOPS_ROOT (SdBrowser
// supports being instantiated more than once -- FilePlayerMode's own
// `browser` is a separate instance already). NOT the same object as
// FilePlayerMode's browser, and NOT reused for the bundle
// save/load/delete flows above (those still just list session-folder
// names by hand, same as before). Only .mid files and folders ever
// appear here (see selectHighlightedLoopEntry()).
SdBrowser loopBrowser;
int loopBrowseSelectedIndex = 0;
int loopBrowseScrollOffset = 0;
bool loopDeleteFailed = false; // valid while screen == SCREEN_LOAD_LOOP_DELETE_CONFIRM

// -- SCREEN_IMPORT_CONFIRM state -----------------------------------------
// Set by requestImport(), the entry point main.cpp calls in response to
// FilePlayerMode's "Load to Looper" browser action -- see looper_mode.h.
char pendingImportPath[224];
int pendingImportTrack = 0;

// Draws a message and switches to SCREEN_FLASH_MESSAGE for `durationMs`,
// after which update() falls back to SCREEN_MAIN on its own.
void showFlashMessage(const char* line1, const char* line2, uint32_t durationMs) {
    Ui::drawMessage(line1, line2);
    screen = SCREEN_FLASH_MESSAGE;
    flashUntilMs = millis() + durationMs;
}

void buildTrackViews(Ui::LoopTrackView out[4]); // defined below
void rescaleBarQuantizedTracks(); // defined below
void startArmedTracksTogether(); // defined below
void handleBpmValueAdjust(); // defined below
void handleTimeSigAdjust(); // defined below
void beginSaveLoopName(); // defined below
void beginLoadLoopBrowse(); // defined below

// Cheap partial redraw of just track idx's row -- used throughout instead
// of a full needsRedraw pass whenever only that one track's state
// actually changed (record/mute/stop/erase on the selected track, an
// armed track starting to record, a bar-quantized take auto-committing,
// etc). Full-screen redraws are visually jarring and these happen often
// during normal use, so they're worth avoiding wherever only a row or two
// is actually affected.
void redrawTrackRow(int idx) {
    Ui::LoopTrackView views[4];
    buildTrackViews(views);
    Ui::updateLooperTrackRow(views, idx, !onBpmRow && selectedTrack == idx);
}

// Same, for the (rarer) case where multiple tracks changed at once -- All
// Stop/All Start, or several armed tracks starting together -- still just
// 4 small row updates, nowhere near the cost of a full screen redraw
// (header, footer, BPM row, and any untouched track rows all stay put).
void redrawAllTrackRows() {
    Ui::LoopTrackView views[4];
    buildTrackViews(views);
    for (int i = 0; i < NUM_TRACKS; i++) {
        Ui::updateLooperTrackRow(views, i, !onBpmRow && selectedTrack == i);
    }
}

// -- playback engine --------------------------------------------------

uint32_t epochFor(const LoopTrack& t) {
    return syncMode ? sessionEpochMs : t.epochMs;
}

// Milliseconds actually "run" since this track's epoch -- elapsed wall
// time minus however long it's cumulatively spent frozen (TRACK_STOPPED
// or TRACK_PAUSED -- see LoopTrack's stopAccumMs comment -- or TRACK_ARMED
// with lengthMs > 0, an existing stopped track armed for overdub and
// still waiting for its trigger note, see toggleRecordOnSelected()'s
// TRACK_STOPPED case). This is what makes Stop/Pause genuinely freeze
// position (as opposed to Mute, which keeps counting silently underneath
// so it never drifts out of phase with the rest of the group): while
// frozen, the growing "time spent frozen so far" term cancels the growing
// "now" term exactly, so the result -- and therefore the position derived
// from it -- stays constant until resumed.
uint32_t elapsedRunMs(const LoopTrack& t) {
    uint32_t now = millis();
    bool frozen = (t.state == TRACK_STOPPED || t.state == TRACK_PAUSED ||
                   (t.state == TRACK_ARMED && t.lengthMs > 0));
    uint32_t stoppedSoFar = t.stopAccumMs + (frozen ? (now - t.stopStartMs) : 0);
    return now - epochFor(t) - stoppedSoFar;
}

// Bar length in milliseconds at manualBpm/timeSigNum/timeSigDen. A beat is
// one timeSigDen-th note (60000/manualBpm ms is one quarter note, so a
// beat is that scaled by 4/timeSigDen), and a bar is timeSigNum beats.
uint32_t barsToMs(int bars) {
    float beatMs = 60000.0f / manualBpm * (4.0f / timeSigDen);
    return (uint32_t)(bars * timeSigNum * beatMs);
}

// Rounds `now` back to the most recently-passed multiple of `barMs` from
// `origin` (sessionEpochMs, treated as "bar 0" of the whole session's bar
// grid) -- i.e. floor, not true nearest. The next bar line hasn't
// happened yet at the moment this is called (recording starts live, the
// instant a note arrives), so rounding forward would mean snapping to a
// boundary that's still in the future -- not meaningful for "where did
// this take start".
uint32_t snapToBarBoundary(uint32_t now, uint32_t origin, uint32_t barMs) {
    if (barMs == 0) return now;
    uint32_t elapsed = now - origin;
    return origin + (elapsed / barMs) * barMs;
}

// Once a track has a committed lengthMs (overdub, or any already-recorded
// track just playing back), this is simply position-within-the-loop --
// state-agnostic, which is exactly what already makes the bar fill
// correctly during overdub (RECORDING) the same way it does during
// PLAYING, since updatePlayback() advances both identically. STOPPED is
// the one deliberate exception: it always shows empty (0) rather than
// wherever it happened to freeze, since resumeFromStop() always restarts
// from position 0 too (unlike PAUSED, whose whole point is resuming from
// exactly where it froze) -- showing anything but empty here would
// promise a resume point Stop was never going to honor.
//
// While still on the open-ended *first* pass (lengthMs==0) there's no
// established loop yet to show a position within -- but a bar-quantized
// track (barLength > 0) does have a known target duration even before
// that first pass commits, so show progress toward *that* instead of
// leaving the bar empty until the auto-commit lands.
float positionFrac(const LoopTrack& t) {
    if (t.state == TRACK_STOPPED) return 0.0f;
    if (t.lengthMs == 0) {
        if (t.state != TRACK_RECORDING || t.barLength == 0) return 0.0f;
        uint32_t target = barsToMs(t.barLength);
        if (target == 0) return 0.0f;
        float frac = (float)(millis() - t.recordStartMs) / (float)target;
        return frac > 1.0f ? 1.0f : frac;
    }
    return (float)(elapsedRunMs(t) % t.lengthMs) / (float)t.lengthMs;
}

uint8_t dataBytesForStatus(uint8_t status) {
    uint8_t type = status & 0xF0;
    return (type == 0xC0 || type == 0xD0) ? 1 : 2; // Program Change / Channel Aftertouch
}

// Recomputes prevPos/playCursor from the current position -- used both
// when flipping Sync/Independent (the epoch driving epochFor() changes
// for every track at once) and when resuming a stopped track (elapsedRunMs()
// jumps back to where it was frozen), so playback picks up correctly
// instead of double-firing or skipping whatever's between the old and new
// position.
void resyncCursor(LoopTrack& t) {
    uint32_t pos = elapsedRunMs(t) % t.lengthMs;
    t.prevPos = pos;
    int idx = 0;
    while (idx < t.eventCount && t.events[idx].offsetMs <= pos) idx++;
    t.playCursor = idx;
}

// Un-freezes a stopped track by restarting it from the beginning of its
// loop (position 0), not resuming from wherever it was frozen -- see
// finishResumeFromPause() for the resume-in-place counterpart Pause uses
// instead. Setting stopAccumMs to exactly `now - epochFor(t)` makes
// elapsedRunMs() (and therefore position) come out to 0 right at this
// instant -- this works the same way whether epochFor() is currently the
// shared Sync epoch or this track's own Independent one, since
// stopAccumMs is per-track either way. Shared by NAV and EDIT-held ("All
// Start") on a stopped track -- both should restart it the same way.
void resumeFromStop(LoopTrack& t) {
    uint32_t now = millis();
    // The track itself always restarts from position 0 here, never resumes
    // in place -- if the master clock was genuinely idle (nothing else
    // already running/counting-in), restart it from the downbeat too, same
    // "fresh start" rule restartMasterClockFromStart() documents. A track
    // being Stopped (not Paused) is deliberately NOT the resume-in-place
    // exception to that rule -- only a resumed Pause is (see
    // finishResumeFromPause(), which this is not).
    if (masterClockNextDueMs == 0) restartMasterClockFromStart(now);
    t.stopAccumMs = now - epochFor(t);
    t.state = TRACK_PLAYING;
    t.playCursor = 0;
    t.prevPos = 0;
    // In case a Sync-mode quantized restart was queued (see
    // togglePauseOnSelected()) and this call bypassed it instead (e.g.
    // NAV/"All Start" resuming immediately while one was still pending) --
    // otherwise the stale flag would sit there and fire unexpectedly the
    // next time this track becomes Stopped again.
    t.resumePending = false;
}

// Advances every track's playback cursor and fires any events whose
// scheduled position has been reached since the last call. Runs every
// tick regardless of state -- RECORDING tracks with existing content
// (overdubs) play back the same way PLAYING ones do, which is what lets
// you hear the loop while you're layering onto it; MUTED tracks still
// advance (silently) so unmuting doesn't have to "catch up". EMPTY,
// STOPPED, PAUSED, and ARMED (whether fresh or waiting to overdub) tracks
// are skipped entirely -- see elapsedRunMs().
void updatePlayback() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.state == TRACK_EMPTY || t.state == TRACK_STOPPED || t.state == TRACK_PAUSED ||
            t.state == TRACK_ARMED || t.lengthMs == 0) continue;

        uint32_t pos = elapsedRunMs(t) % t.lengthMs;
        if (pos < t.prevPos) t.playCursor = 0; // wrapped
        t.prevPos = pos;

        while (t.playCursor < t.eventCount && t.events[t.playCursor].offsetMs <= pos) {
            const LoopEvent& e = t.events[t.playCursor];
            if (t.state != TRACK_MUTED) {
                // channelOut == CHANNEL_AS_RECORDED sends the event exactly
                // as captured (its status byte's own channel nibble,
                // whatever that was -- see recordEvent()); otherwise every
                // event is forced onto one fixed output channel regardless
                // of what it was recorded on, so e.g. a single input device
                // can drive several pieces of outboard gear on different
                // channels without ever touching that device's own channel.
                uint8_t outStatus = (t.channelOut == CHANNEL_AS_RECORDED)
                    ? e.status : (uint8_t)((e.status & 0xF0) | t.channelOut);
                MidiOutput::sendRaw(outStatus, e.data1, e.data2, dataBytesForStatus(e.status));
                // Separate from recordEvent()'s lastRecordActivityMs: a
                // track mid-overdub is simultaneously capturing new events
                // AND playing back its existing content, so these need
                // their own indicator rather than sharing one. Muted
                // tracks don't flash: nothing actually went out.
                t.lastPlaybackActivityMs = millis();
            }
            t.playCursor++;
        }
    }
}

// Silences whatever channel(s) this track has actually been sounding
// notes on -- shared by every state transition that needs to avoid a
// stuck note (Mute, Stop, Pause, MIDI Stop, Erase). A remapped output
// channel (channelOut != CHANNEL_AS_RECORDED) only ever sends on that one
// channel, so silencing just it is enough; in AS_RECORDED mode played-back
// events keep whatever channel they were captured on, which can vary
// per-event under OMNI input, so there's no single channel guaranteed to
// catch every stuck note -- broadcast All Notes Off across all 16 real
// channels instead. Deliberately just CC 123 (not also All Sound Off, the
// way MidiOutput::allNotesOffAllChannels() does for a global panic) --
// same narrow scope the single-channel call this replaces already had.
void allNotesOffForTrack(const LoopTrack& t) {
    if (t.channelOut != CHANNEL_AS_RECORDED) {
        MidiOutput::sendControlChange(t.channelOut, 123, 0);
    } else {
        for (uint8_t ch = 0; ch < 16; ch++) MidiOutput::sendControlChange(ch, 123, 0);
    }
}

// True while at least one track is actively capturing (a fresh recording
// pass or an overdub -- both are TRACK_RECORDING, see LoopTrack's own
// comment on why there's no separate overdub state). TRACK_ARMED doesn't
// count: arming alone hasn't started capturing anything yet.
bool anyTrackRecording() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (tracks[i].state == TRACK_RECORDING) return true;
    }
    return false;
}

// True while at least one track is Muted -- lets the metronome stand in
// as a background reference click specifically when the track that would
// otherwise provide one has been silenced. Deliberately not "any track
// Playing": a track that's actually audible already provides its own
// reference, so there's no need to also click over it.
bool anyTrackMuted() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (tracks[i].state == TRACK_MUTED) return true;
    }
    return false;
}

// True while at least one track is genuinely running -- Recording,
// Playing, or Muted (Muted still advances its position/plays back
// internally, just silently -- same three states updatePlayback() itself
// treats as "actively advancing", as opposed to EMPTY/STOPPED/PAUSED/
// ARMED, which it skips). Used to hold the metronome's beat clock paused
// until something actually starts (see updateMasterClock()) -- deliberately
// excludes ARMED: a track waiting for its first note hasn't started
// playing or recording yet, so it shouldn't release the clock on its own.
bool anyTrackAdvancing() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        TrackState s = tracks[i].state;
        if (s == TRACK_RECORDING || s == TRACK_PLAYING || s == TRACK_MUTED) return true;
    }
    return false;
}

// Advances the master clock (this session's shared beat/bar position --
// see its own declaration comment above) and, when a beat comes due,
// fires an audible click -- but only while Metro is actually on
// (metronomeOn) and something's actually recording, a count-in is leading
// into one (see maybeStartCountIn()), a track is Muted (standing in as a
// reference click for whatever got silenced), or a click-only pre-roll is
// running (see startMetronomeClickOnly()). The clock itself keeps
// advancing regardless of metronomeOn, though, silently -- it's what
// drives the header's beat-indicator squares (see masterClockCurrentBeat()/
// Ui::updateLooperBeatIndicator()), which stay a useful "where are we in
// the bar" reference for a running loop even with the audible click
// switched off; the BPM row's own beat-flash (updateBpmRowBeatFlash()) is
// the separate on-screen indicator for the click itself, and stays
// metronomeOn-gated. Talks to Synth directly rather than through
// MidiOutput::sendNoteOn() -- unlike track playback, this must never reach
// HW/USB MIDI out, only the onboard synth's own audio-out. No explicit
// note-off needed: percussion voices are one-shot and decay on their own
// (see synth.cpp's startPercussionVoice()).
//
// The clock only actually runs while something is genuinely happening --
// a count-in, at least one track Recording/Playing/Muted (see
// anyTrackAdvancing()), or a click-only pre-roll -- rather than
// continuously: otherwise it'd sit there advancing (and the header's
// beat-indicator squares moving) with nothing playing, recording, or even
// pre-rolling, which reads as "running by default" rather than "waiting
// to be used". While something IS running,
// though, the clock deliberately keeps advancing every beat regardless of
// finer-grained changes in *what* exactly is running, whether it's
// audible, or whether Metro itself gets toggled mid-session (e.g. muting
// one of several playing tracks, or flipping Metro on/off) -- rather than
// being reset on every such change, since that would resync to "whichever
// beat we're in right now" instead of landing on the next beat a listener
// (or the header display) would actually expect.
//
// A restart from nothing running (masterClockNextDueMs == 0) is the one
// case that DOES resync -- exactly once, right here, never on every call
// (recomputing an absolute beat count from the *entire* elapsed session
// time on every call broke down as soon as beatMs changed: manualBpm/
// timeSigDen adjustment, a tiny tempo tweak, multiplied across a long
// elapsed time, could shift that computed integer by many beats at once,
// firing a spurious click on almost every adjustment step -- advancing by
// exactly one beatMs interval per click instead means a tempo change only
// ever affects the *next* interval, never retroactively recounts history).
// This generic resync -- picking up wherever elapsed real time since
// sessionEpochMs says the clock should be -- is deliberately what a
// resumed Pause relies on for resuming in place (see
// finishResumeFromPause()'s own comment). Every *other* restart path
// (arming a track and triggering it, a click-only pre-roll, resuming a
// Stopped track) instead calls restartMasterClockFromStart() first, which
// forces masterClockBeatInBar/masterClockNextDueMs to the downbeat before
// this generic resync would otherwise run -- since by the time this
// function sees masterClockNextDueMs == 0 again, it can no longer tell a
// "start fresh at bar 1" restart apart from a "resume in place" one on its
// own.
void updateMasterClock() {
    // A click-only pre-roll (see startMetronomeClickOnly()) only makes
    // sense while Metro itself is on -- cancel it the same way
    // updateCountIn() cancels an in-progress count-in when Metro turns off
    // mid-count-in, rather than leaving the clock silently advancing with
    // no audible click and nothing armed/recording to justify it.
    if (metronomeClickOnlyActive && !metronomeOn) metronomeClickOnlyActive = false;

    if (!(countInActive || anyTrackAdvancing() || metronomeClickOnlyActive)) {
        // Not just "not running" -- reset all the way back to the start,
        // every tick nothing's advancing, so the master clock never sits
        // idle holding some stale mid-bar position (e.g. left over from a
        // track that was Paused and then erased instead of resumed). A
        // fresh start's own restartMasterClockFromStart() call would
        // override this anyway, but resetting proactively here means
        // *any* idle stretch, from *any* cause, always leaves the clock
        // parked at the beginning -- never relies on every possible path
        // back to idle remembering to clean up after itself.
        masterClockNextDueMs = 0;
        masterClockBeatInBar = 0;
        return;
    }

    uint32_t now = millis();
    if (!sessionEpochSet) { sessionEpochMs = now; sessionEpochSet = true; }

    float beatMs = 60000.0f / manualBpm * (4.0f / timeSigDen);
    if (beatMs <= 0.0f) return;

    if (masterClockNextDueMs == 0) {
        uint32_t elapsed = now - sessionEpochMs;
        int beatIndex = (int)(elapsed / beatMs);
        masterClockBeatInBar = beatIndex % timeSigNum;
        masterClockNextDueMs = now; // treat the beat we're already in as due right away
    }
    if ((int32_t)(now - masterClockNextDueMs) < 0) return; // not due yet

    if (metronomeOn && (countInActive || anyTrackRecording() || anyTrackMuted() || metronomeClickOnlyActive)) {
        bool downbeat = (masterClockBeatInBar == 0);
        uint8_t baseVelocity = downbeat ? METRONOME_VELOCITY_DOWNBEAT : METRONOME_VELOCITY;
        uint8_t velocity = (uint8_t)(((int)baseVelocity * metronomeVolumePercent) / 100);
        Synth::noteOn(METRONOME_CHANNEL, METRONOME_NOTE, velocity);
    }

    masterClockBeatInBar = (masterClockBeatInBar + 1) % timeSigNum;
    masterClockNextDueMs += (uint32_t)beatMs;
}

// Derives which beat is current (0 = downbeat) -- despite the name, this
// tracks the loop's beat *position*, not specifically the audible click:
// it's driven by the same silently-still-advancing clock regardless of
// metronomeOn (see updateMasterClock()'s own comment), so it stays valid for
// the header's beat-indicator squares (updateMetronomeBeatIndicator()/
// Ui::updateLooperBeatIndicator()) whenever a track is actually running,
// even with the audible click switched off. Returns -1 (no beat lit -- the
// row still shows, just with nothing highlighted) while the clock itself
// isn't actually running (masterClockNextDueMs == 0, see updateMasterClock()'s
// own "held until something starts" gate) -- otherwise, right after
// something starts running but before its first beat, this would show
// whatever stale beat masterClockBeatInBar was last left at as if it were
// live. When the clock IS running: masterClockBeatInBar always holds the
// *next* due beat by the time updateMasterClock() returns (see its own big
// comment above) -- never "the beat that just fired" -- so the beat
// actually current right now is one step behind it, wrapping backward
// through 0.
int masterClockCurrentBeat() {
    if (masterClockNextDueMs == 0) return -1; // clock held, waiting for something to start
    return (masterClockBeatInBar - 1 + timeSigNum) % timeSigNum;
}

// Called from PLAY on the BPM row while nothing's already advancing (see
// its handler in updateMainScreen()) -- if the metronome and count-in are
// both enabled and a count-in isn't already running, starts one. No-op
// otherwise (see startMetronomeClickOnly() for the Metro-on-but-
// Count-In-off alternative that handler falls back to). Safe to call
// while a count-in is already active -- e.g. PLAY pressed again partway
// through one -- since it just no-ops and leaves the existing count-in
// running.
void maybeStartCountIn() {
    if (!metronomeOn || !countInEnabled || countInActive) return;

    uint32_t now = millis();
    if (!sessionEpochSet) { sessionEpochMs = now; sessionEpochSet = true; }

    countInActive = true;
    // Snap backward to the current bar's start, same "decided live"
    // convention startArmedTracksTogether() uses for bar-quantized tracks
    // -- the count-in always covers one real, grid-aligned bar rather
    // than an arbitrary mid-bar slice, even if the metronome's already
    // ticking from something else playing/recording.
    countInStartMs = snapToBarBoundary(now, sessionEpochMs, barsToMs(1));
    countInBeatShown = 0;

    // Force the metronome's own click clock to restart exactly here, on
    // this same downbeat, instead of leaving it wherever its ongoing
    // background phase happened to be (see updateMasterClock()'s comment on
    // why that clock normally *doesn't* reset on every audibility change
    // -- a count-in is the one deliberate exception, since the whole
    // point of pressing it is a clean, predictable "4, 3, 2, 1" where the
    // beats you hear actually match the beats the overlay is counting
    // down, not whatever click happened to already be mid-cycle).
    masterClockNextDueMs = countInStartMs;
    masterClockBeatInBar = 0;

    needsRedraw = true; // switches the main-screen redraw into count-in mode -- see update()
}

// Called from PLAY on the BPM row (see its handler in updateMainScreen())
// when maybeStartCountIn() wouldn't do anything -- Metro's on but Count In
// isn't, so there's no overlay to lead into a start; this just starts the
// click running immediately instead; lets the user get a feel for the
// tempo, or simply get the session's shared clock/epoch established,
// before arming or playing anything. Its only call site's own
// anyTrackAdvancing()/countInEnabled guards mean this only ever runs while
// the master clock was genuinely idle, so restartMasterClockFromStart()
// unconditionally applies -- no need to check masterClockNextDueMs == 0
// first the way its other two call sites do. metronomeClickOnlyActive is
// what actually keeps the clock running (see updateMasterClock()) until
// real recording/playing takes over that job (see
// startArmedTracksTogether()).
void startMetronomeClickOnly() {
    uint32_t now = millis();
    if (!sessionEpochSet) { sessionEpochMs = now; sessionEpochSet = true; }
    restartMasterClockFromStart(now);
    metronomeClickOnlyActive = true;
}

// Advances the count-in and, once it's run for countInBars bars, starts
// every currently-armed track together (same call the note-triggered
// path in handleIncomingMidi() uses) and hands the screen back to the
// normal track view. Runs every tick regardless of which screen is up,
// same reasoning as updatePlayback().
void updateCountIn() {
    if (!countInActive) return;
    if (!metronomeOn) { // turned off mid-count-in -- cancel, tracks go back to waiting for a note
        countInActive = false;
        needsRedraw = true;
        return;
    }

    uint32_t now = millis();
    float beatMs = 60000.0f / manualBpm * (4.0f / timeSigDen);
    if (beatMs <= 0.0f) { countInActive = false; needsRedraw = true; return; }

    int totalBeats = countInBars * timeSigNum; // matches barsToMs()'s beat definition
    uint32_t totalMs = (uint32_t)(beatMs * totalBeats);
    uint32_t elapsed = now - countInStartMs;

    if (elapsed >= totalMs) {
        countInActive = false;
        startArmedTracksTogether();
        needsRedraw = true;
        return;
    }

    int beatIndex = (int)(elapsed / beatMs); // 0..totalBeats-1
    if (beatIndex != countInBeatShown) {
        countInBeatShown = beatIndex;
        // Counts down (e.g. 4 3 2 1), not up -- beatIndex 0 (the first
        // beat) shows the full count, the last beat shows 1.
        Ui::updateLooperCountInBeat(totalBeats - countInBeatShown);
    }
}

// Cheap per-tick check driving the header's beat-indicator row (see
// Ui::updateLooperBeatIndicator()) -- same "compare derived state against
// what was last actually drawn, only touch the display on an actual change"
// pattern as updateCountIn()'s beatIndex/countInBeatShown check just above,
// just with local statics instead of a shared namespace variable since
// nothing else needs to read this indicator's last-drawn state. Runs
// unconditionally every SCREEN_MAIN tick (see update()) rather than on the
// 100ms updateLooperPositions() throttle -- the comparison itself is cheap,
// and the row only actually needs to change roughly once per beat.
void updateMetronomeBeatIndicator() {
    static bool shownVisible = false;
    static int shownBeat = -1;
    static int shownNum = -1;

    // Visibility is its own toggle (see showBeatIndicator), independent of
    // metronomeOn; the highlighted square tracks a running track's beat
    // *position*, also independent of metronomeOn (masterClockCurrentBeat()
    // advances off the same silent clock regardless -- see
    // updateMasterClock()'s own comment), so this advances for a running
    // loop even with the audible click switched off -- there's a separate
    // on-screen indicator for the click itself (the BPM row's own
    // beat-flash, see updateBpmRowBeatFlash()). Squares still show static
    // (nothing lit) while nothing's actually running, same as before.
    bool visible = showBeatIndicator;
    int beat = visible ? masterClockCurrentBeat() : -1;

    if (visible == shownVisible && beat == shownBeat && timeSigNum == shownNum) return;
    shownVisible = visible;
    shownBeat = beat;
    shownNum = timeSigNum;

    Ui::updateLooperBeatIndicator(visible, beat, timeSigNum);
}

// Flashes the BPM row's outline briefly on each metronome beat -- a
// bigger, more peripheral-vision-friendly companion to the small header
// beat squares above, meant to be noticed without staring at the header
// but not distracting if you already are. Independent of showBeatIndicator
// (the header squares' own on/off toggle): this only cares whether the
// metronome is actually sounding, not whether that separate row is shown.
// The downbeat (beat 0) flashes orange instead of yellow, same idea as the
// metronome's own louder downbeat click, so bar boundaries stand out from
// the beats in between at a glance.
// Detects "a new beat just fired" the same way updateMetronomeBeatIndicator()
// does (comparing masterClockCurrentBeat() against what it was last tick) --
// lastBeat starts at -2 rather than -1 so the clock's initial "-1 = held,
// nothing sounding yet" reading is never itself mistaken for a beat.
void updateBpmRowBeatFlash() {
    static int lastBeat = -2;
    uint32_t now = millis();

    int beat = metronomeOn ? masterClockCurrentBeat() : -1;
    if (beat != lastBeat) {
        lastBeat = beat;
        if (beat >= 0) { // an actual beat fired, not the clock going idle/held
            bpmRowFlashUntilMs = now + BPM_ROW_BEAT_FLASH_MS;
            Ui::updateLooperBpmRowBeatFlash(true, beat == 0, onBpmRow);
        }
    }

    if (bpmRowFlashUntilMs != 0 && now >= bpmRowFlashUntilMs) {
        bpmRowFlashUntilMs = 0;
        Ui::updateLooperBpmRowBeatFlash(false, false, onBpmRow);
    }
}

// For a bar-quantized track (barLength > 0) on its open-ended first
// recording pass, auto-commits it once the target duration has elapsed --
// no manual PLAY press needed, which is the whole point of quantizing: the
// loop lands on an exact bar boundary rather than whatever length the
// user happened to stop it at. Freeform tracks (barLength == 0) and
// tracks past their first pass (lengthMs != 0, so overdub applies
// instead) are untouched. Sets lengthMs to the exact computed target
// rather than measured elapsed time, so it's precisely on the BPM grid
// even though this is only checked once per tick (a few ms of drift would
// otherwise creep in from tick granularity).
void updateBarQuantizedRecording() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.state != TRACK_RECORDING || t.lengthMs != 0 || t.barLength == 0) continue;

        uint32_t target = barsToMs(t.barLength);
        if (millis() - t.recordStartMs >= target) {
            t.lengthMs = target;
            t.epochMs = t.recordStartMs;
            // Same re-zero as the manual-commit path in
            // toggleRecordOnSelected()'s TRACK_RECORDING case -- see its
            // comment for why this can't be left at its stale prior value.
            t.stopAccumMs = millis() - epochFor(t);
            t.playCursor = 0;
            t.prevPos = 0;
            t.state = TRACK_PLAYING;
            redrawTrackRow(i); // same as startArmedTracksTogether() -- this can happen with no button press
        }
    }
}

// Inserts a captured event in time-sorted order. During a fresh (still
// open-ended) recording pass this always degenerates to a plain append,
// since offsets arrive already increasing; during an overdub, the new
// event lands mid-buffer at wherever its position-in-cycle falls, so an
// actual insertion is needed to keep the array sorted for updatePlayback().
void recordEvent(LoopTrack& t, uint8_t status, uint8_t data1, uint8_t data2) {
    t.lastRecordActivityMs = millis(); // flash the record indicator regardless of whether storage succeeds below
    if (t.eventCount >= MAX_EVENTS_PER_TRACK) {
        t.bufferFull = true; // shown on-screen (see buildTrackViews()) rather than silently dropping unnoticed
        return;
    }

    uint32_t offset = (t.lengthMs == 0) ? (millis() - t.recordStartMs) : (elapsedRunMs(t) % t.lengthMs);

    int idx = t.eventCount;
    while (idx > 0 && t.events[idx - 1].offsetMs > offset) idx--;
    for (int j = t.eventCount; j > idx; j--) t.events[j] = t.events[j - 1];
    t.events[idx] = {offset, status, data1, data2};
    t.eventCount++;

    // Inserting at/before the playback cursor shifts every event from
    // idx onward one slot to the right -- without this, the cursor would
    // keep its old numeric index, which after the shift points at the
    // wrong event (the one that used to be "next" is now one slot later),
    // causing it to double-fire on this cycle while the just-inserted
    // event gets skipped until the next one.
    if (idx <= t.playCursor) t.playCursor++;
}

// Starts every currently-armed track's recording clock at once, right
// now -- called the moment any one of them actually receives a matching
// event (see handleIncomingMidi()). This is what makes "arm several
// tracks, then just start playing" record them in sync: they don't each
// start on their own first note, they all start together on whichever
// one comes first.
void startArmedTracksTogether() {
    uint32_t now = millis();
    if (!sessionEpochSet) { sessionEpochMs = now; sessionEpochSet = true; }
    // A fresh start while the master clock was genuinely idle always
    // begins right on the downbeat (see restartMasterClockFromStart()) --
    // checked *before* anything below can change what's advancing, so it
    // only ever fires for a truly fresh start: not mid-count-in or while
    // joining tracks already playing (masterClockNextDueMs is already
    // nonzero by the time this runs, in both cases).
    if (masterClockNextDueMs == 0) restartMasterClockFromStart(now);
    // Whatever's about to start recording/playing takes over keeping the
    // master clock running (anyTrackAdvancing() covers it from
    // here on) -- a pre-roll click-only session (see
    // startMetronomeClickOnly()) has served its purpose the moment real
    // recording begins, regardless of which of the three call paths
    // (incoming note, PLAY, count-in finishing) got here.
    metronomeClickOnlyActive = false;
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.state == TRACK_ARMED) {
            if (t.lengthMs > 0) {
                // Armed from Stopped for an overdub (see
                // toggleRecordOnSelected()'s TRACK_STOPPED case), not a
                // fresh first pass -- un-freeze it the same way a Pause
                // resume does (extend stopAccumMs by exactly how long it
                // was frozen, see finishResumeFromPause()) so capturing
                // picks up from wherever it was stopped, rather than
                // silently jumping forward as if it had kept playing the
                // whole time it was actually frozen.
                t.stopAccumMs += now - t.stopStartMs;
                resyncCursor(t);
            } else if (t.barLength > 0) {
                // Bar-quantized tracks start counting from the nearest bar
                // line instead of the raw trigger moment -- this becomes
                // both the reference event offsets are captured against
                // (recordEvent()) and, once committed, epochMs, so the
                // whole take is bar-grid-aligned with no separate
                // after-the-fact adjustment needed.
                t.recordStartMs = snapToBarBoundary(now, sessionEpochMs, barsToMs(1));
            } else if (metronomeOn) {
                // Freeform (barLength == 0) with Metro actually running a
                // click the user could realistically have been playing
                // along to (e.g. a pre-roll from startMetronomeClickOnly())
                // -- snap to the nearest *beat* instead of the raw trigger
                // moment, same floor-not-true-nearest reasoning
                // snapToBarBoundary()'s own comment gives (the next beat
                // hasn't happened yet at the moment this fires, so there's
                // nothing to round forward to). snapToBarBoundary() is
                // grid-size-agnostic despite its name -- one beat's worth
                // of ms is just a finer grid than one bar's.
                float beatMs = 60000.0f / manualBpm * (4.0f / timeSigDen);
                t.recordStartMs = snapToBarBoundary(now, sessionEpochMs, (uint32_t)beatMs);
            } else {
                // No beat reference to snap to (Metro off) -- keep the raw
                // trigger moment, the "unquantized" option.
                t.recordStartMs = now;
            }
            t.state = TRACK_RECORDING;
            // This runs from the MIDI input callback or updateCountIn(),
            // not a button handler, so nothing else would otherwise tell
            // the main screen its label text needs to change.
            redrawTrackRow(i);
        }
    }
}

// Registered with MidiOutput::setInputHandler() in enter(). A track that's
// TRACK_ARMED is waiting for its first note rather than already recording
// (see toggleRecordOnSelected()); an incoming event matching an armed
// track's channel is the "go" signal -- see startArmedTracksTogether().
// Once (now) recording, feeds any track on the matching channel (or set
// to OMNI_CHANNEL, which matches anything). Deliberately does NOT echo
// the event back out live: MIDI out only ever carries recorded loop
// content during playback (see updatePlayback()), never a direct mirror
// of the input, since with some USB/DAW routings that mirror comes right
// back around as input again -- an unbounded feedback loop, not just an
// annoyance.
void handleIncomingMidi(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
    uint8_t channel = status & 0x0F;
    (void)len;

    // When metronome+count-in are both active, arming a track already
    // triggers a timed count-in instead (see maybeStartCountIn(), called
    // from toggleRecordOnSelected()) -- skip the note-triggered path
    // entirely then, so an incoming note can't ALSO independently start
    // things early, mid-count-in.
    if (!(metronomeOn && countInEnabled)) {
        bool matchesArmed = false;
        for (int i = 0; i < NUM_TRACKS; i++) {
            const LoopTrack& t = tracks[i];
            if (t.state == TRACK_ARMED && (t.channelIn == channel || t.channelIn == OMNI_CHANNEL)) {
                matchesArmed = true;
                break;
            }
        }
        if (matchesArmed) startArmedTracksTogether();
    }

    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.state == TRACK_RECORDING && (t.channelIn == channel || t.channelIn == OMNI_CHANNEL)) {
            recordEvent(t, status, data1, data2);
        }
    }
}

// -- track actions ------------------------------------------------------

void toggleRecordOnSelected() {
    LoopTrack& t = tracks[selectedTrack];
    switch (t.state) {
        case TRACK_EMPTY:
            // Arm rather than start recording immediately -- the actual
            // clock/capture only begins once a matching note arrives (see
            // handleIncomingMidi()/startArmedTracksTogether()), so an
            // empty gap before you actually start playing doesn't get
            // baked into the loop as silence. Arming alone never starts a
            // count-in even with the metronome's count-in enabled --
            // that's a deliberate separate action, PLAY on the BPM/Metro
            // row (see maybeStartCountIn()'s call site in
            // updateMainScreen()), so arming several tracks in any order
            // first and then starting them together stays possible.
            t.lengthMs = 0;
            t.eventCount = 0;
            t.state = TRACK_ARMED;
            break;

        case TRACK_ARMED:
            // Cancel arming -- EDIT toggles it off same as it toggled it
            // on. Back to EMPTY if this was arming a fresh track, or back
            // to STOPPED (content untouched, still frozen) if this was
            // arming an existing stopped track for overdub instead (see
            // the TRACK_STOPPED case below) -- lengthMs > 0 is what tells
            // the two apart, same convention recordEvent()/positionFrac()
            // already use for "still on the open first pass" vs. not.
            t.state = (t.lengthMs > 0) ? TRACK_STOPPED : TRACK_EMPTY;
            break;

        case TRACK_RECORDING:
            if (t.lengthMs == 0) {
                // Committing the open-ended first pass -- this defines
                // the track's own length and position-zero.
                uint32_t elapsed = millis() - t.recordStartMs;
                t.lengthMs = elapsed > 0 ? elapsed : 1;
                t.epochMs = t.recordStartMs;
                // Re-zero stopAccumMs against *this* commit instant --
                // can't be left at whatever it was (0 by default, or a
                // stale value from an earlier stop/pause cycle on this
                // same track): without this, elapsedRunMs() keeps
                // measuring position from sessionEpochMs (Sync mode) or
                // an unrelated old stopStartMs, which very rarely lines up
                // with where this take's events were actually captured
                // (offsets 0..lengthMs, relative to recordStartMs) -- the
                // take would start audibly partway through instead of at
                // its own beginning. Same "make elapsedRunMs() come out to
                // 0 right now" trick resumeFromStop() uses.
                t.stopAccumMs = millis() - epochFor(t);
            }
            t.playCursor = 0;
            t.prevPos = 0;
            t.state = TRACK_PLAYING; // recording (fresh or overdub) always leaves you hearing it
            break;

        case TRACK_PLAYING:
        case TRACK_MUTED:
            // Start an overdub pass -- length/epoch stay put, new events
            // land at their current position-in-cycle (see recordEvent()).
            t.state = TRACK_RECORDING;
            break;

        case TRACK_STOPPED:
            // Arms it for overdub rather than resuming right away -- stays
            // frozen/silent (see elapsedRunMs()) until the first matching
            // note actually starts capturing (see handleIncomingMidi()/
            // startArmedTracksTogether()), the same "arm now, start for
            // real on the first note" pattern a fresh EMPTY track uses,
            // so EDIT never itself decides whether the track ends up
            // playing -- only the incoming note does. Existing content
            // (lengthMs/eventCount) is left alone, unlike EMPTY's arm
            // case, which explicitly resets both since it has nothing yet.
            t.state = TRACK_ARMED;
            break;

        case TRACK_PAUSED:
            break; // no-op -- resume a Pause via PLAY instead (see togglePauseOnSelected())
    }
}

void toggleMuteOnSelected() {
    LoopTrack& t = tracks[selectedTrack];
    if (t.state == TRACK_PLAYING) {
        allNotesOffForTrack(t); // avoid a stuck note
        t.state = TRACK_MUTED;
    } else if (t.state == TRACK_MUTED) {
        t.state = TRACK_PLAYING;
    }
    // EMPTY/RECORDING/STOPPED/PAUSED: no-op, nothing sounding (or already silent) to mute.
}

// Commits an in-progress recording (same as a normal PLAY-to-stop, so the
// take isn't lost) and then freezes the track. Shared by toggleStopOnSelected()
// (NAV on the selected track, which should stop a recording too, not just
// playback) and stopAllActiveTracks() (confirmed exit while loops are
// running -- see the comment on that function). Also reaches a Paused
// track (NAV should be able to fully stop something merely paused, same
// as it can a playing one) -- resuming it afterward always goes through
// resumeFromStop()'s reset-to-0 behavior from then on, not a Pause-style
// resume-in-place, since it's no longer just paused.
void commitAndStop(LoopTrack& t) {
    if (t.state == TRACK_ARMED) {
        // Never actually started -- nothing to commit, just cancel the
        // arming. Same EMPTY-vs-STOPPED distinction as toggleRecordOnSelected()'s
        // own TRACK_ARMED case: an overdub-arm (lengthMs > 0) has real
        // content to preserve, a fresh arm (lengthMs == 0) doesn't.
        t.state = (t.lengthMs > 0) ? TRACK_STOPPED : TRACK_EMPTY;
        return;
    }
    if (t.state == TRACK_RECORDING) {
        if (t.lengthMs == 0) {
            uint32_t elapsed = millis() - t.recordStartMs;
            t.lengthMs = elapsed > 0 ? elapsed : 1;
            t.epochMs = t.recordStartMs;
        }
        t.state = TRACK_PLAYING;
    }
    if (t.state == TRACK_PLAYING || t.state == TRACK_MUTED) {
        allNotesOffForTrack(t); // avoid a stuck note
        t.stopStartMs = millis();
        t.state = TRACK_STOPPED;
    } else if (t.state == TRACK_PAUSED) {
        // Already frozen (same mechanism as Stop, see elapsedRunMs()) --
        // just relabel it and drop any pending Sync-mode resume, no new
        // freeze bookkeeping needed.
        t.resumePending = false;
        t.state = TRACK_STOPPED;
    }
    // EMPTY (or already STOPPED): no-op.
}

// Stop halts a track outright: its position freezes (see elapsedRunMs())
// and it stays silent until explicitly resumed, unlike Mute, which keeps
// the track running in phase with the rest of the group the whole time.
// Because real time passes while stopped, resuming can shift this track's
// phase relative to the others -- that's the deliberate difference from
// Mute, not a bug. Also stops an in-progress recording (see commitAndStop()),
// rather than ignoring NAV while TRACK_RECORDING.
void toggleStopOnSelected() {
    LoopTrack& t = tracks[selectedTrack];
    if (t.state == TRACK_STOPPED) resumeFromStop(t);
    else commitAndStop(t);
}

// Un-pauses t so playback continues from exactly where it was frozen --
// unlike resumeFromStop(), which restarts from position 0 -- by extending
// stopAccumMs by exactly how long this pause lasted, so elapsedRunMs()
// picks its position calculation back up seamlessly rather than jumping.
// `atMs` is the moment the pause actually ends: millis() for an
// immediate (Independent-mode) resume, or the beat boundary it was
// waiting for in Sync mode (see togglePauseOnSelected()/
// updatePendingResumes()). resyncCursor() is needed the same way it is
// after a Sync/Independent flip -- the position math just changed
// underneath, so playCursor has to be recomputed rather than continuing
// from wherever it was mid-freeze.
void finishResumeFromPause(LoopTrack& t, uint32_t atMs) {
    t.stopAccumMs += atMs - t.stopStartMs;
    t.state = TRACK_PLAYING;
    t.resumePending = false;
    resyncCursor(t);
}

// The next upcoming beat boundary (the smallest sessionEpochMs-grid point
// strictly after `now`) -- used to quantize a Sync-mode Pause resume onto
// the beat instead of resuming at whatever arbitrary instant the button
// happened to be pressed (see togglePauseOnSelected()). MUST be called
// once and the result cached (see LoopTrack::pendingResumeAtMs) rather
// than re-derived from an ever-advancing `now` on every poll: since this
// always returns a point strictly after whatever `now` it's given, doing
// that would make the target perpetually stay just out of reach --
// exactly the bug that first shipped here, where a Sync-mode Pause could
// never actually resume.
uint32_t nextBeatBoundaryAfter(uint32_t now) {
    float beatMs = 60000.0f / manualBpm * (4.0f / timeSigDen);
    if (beatMs <= 0.0f) return now;
    uint32_t elapsed = now - sessionEpochMs;
    uint32_t beatIndex = (uint32_t)(elapsed / beatMs) + 1;
    return sessionEpochMs + (uint32_t)((float)beatIndex * beatMs);
}

// Same idea as nextBeatBoundaryAfter() above (see its own comment on why
// the result must be cached, not recomputed from a moving `now`), but for
// the next 1-bar grid line instead of the next beat -- used to quantize a
// Sync-mode restart of a bar-quantized Stopped track onto the next bar
// (see togglePauseOnSelected()), same "decided live" bar grid
// startArmedTracksTogether()/snapToBarBoundary() already anchor
// bar-quantized recording to.
uint32_t nextBarBoundaryAfter(uint32_t now) {
    uint32_t barMs = barsToMs(1);
    if (barMs == 0) return now;
    uint32_t elapsed = now - sessionEpochMs;
    uint32_t barIndex = (elapsed / barMs) + 1;
    return sessionEpochMs + barIndex * barMs;
}

// PLAY: pauses a running track in place, resumes a paused one, or restarts
// a stopped one from the top -- see this file's header/updateMainScreen()
// for why this replaced the old record-toggle here (now on EDIT, see
// toggleRecordOnSelected()). A Sync-mode Pause resume doesn't happen
// immediately: it's deferred to the next beat boundary (see
// updatePendingResumes()) so it lands predictably in time with everything
// else instead of cutting back in at a musically arbitrary instant;
// Independent mode has no shared grid to line up with, so it just resumes
// right away. Pressing PLAY again while a resume is still pending cancels
// it, back to plainly paused, rather than queuing a second one.
//
// Restarting a Stopped track works the same way, just quantized to the
// next *bar* instead of the next beat, and only when it's actually
// meaningful to line up: a bar-quantized track (barLength > 0) with Sync
// on. A Freeform track (no bar identity to line up against) or Independent
// mode (no shared grid to wait for) restarts immediately instead, via the
// same resumeFromStop() NAV/EDIT-held ("All Start") already use.
void togglePauseOnSelected() {
    LoopTrack& t = tracks[selectedTrack];
    if (t.state == TRACK_PLAYING || t.state == TRACK_MUTED) {
        allNotesOffForTrack(t); // avoid a stuck note, same as Stop/Mute
        t.stopStartMs = millis();
        t.state = TRACK_PAUSED;
    } else if (t.state == TRACK_PAUSED) {
        if (t.resumePending) {
            t.resumePending = false;
        } else if (syncMode) {
            t.resumePending = true;
            t.pendingResumeAtMs = nextBeatBoundaryAfter(millis()); // fixed target -- see this function's comment
        } else {
            finishResumeFromPause(t, millis());
        }
    } else if (t.state == TRACK_STOPPED) {
        if (t.resumePending) {
            t.resumePending = false; // pressed again before the bar boundary arrived -- cancel, stay stopped
        } else if (syncMode && t.barLength > 0) {
            t.resumePending = true;
            t.pendingResumeAtMs = nextBarBoundaryAfter(millis()); // fixed target -- see nextBarBoundaryAfter()'s comment
        } else {
            resumeFromStop(t);
        }
    }
    // EMPTY/ARMED/RECORDING: no-op -- Pause/restart only applies to a
    // track that's actually sounding or already stopped/paused/pending.
}

// Finalizes any pending Sync-mode restarts once their target boundary
// (LoopTrack::pendingResumeAtMs, fixed when it was requested -- see
// togglePauseOnSelected()) actually arrives -- a Paused track resumes in
// place at the next beat, a Stopped one restarts from the top at the next
// bar. Runs every tick regardless of screen, same reasoning as
// updatePlayback()/updateMasterClock().
void updatePendingResumes() {
    uint32_t now = millis();
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (!t.resumePending) continue;
        if ((int32_t)(now - t.pendingResumeAtMs) < 0) continue;

        if (t.state == TRACK_PAUSED) {
            finishResumeFromPause(t, t.pendingResumeAtMs);
            redrawTrackRow(i);
        } else if (t.state == TRACK_STOPPED) {
            resumeFromStop(t); // clears resumePending itself
            redrawTrackRow(i);
        }
    }
}

// True if any track is actively running (playing, muted-but-running, or
// recording) -- used to decide whether leaving the looper needs a "are
// you sure" prompt first. Stopped/empty tracks don't warrant one.
bool anyTrackActive() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        TrackState s = tracks[i].state;
        if (s == TRACK_PLAYING || s == TRACK_MUTED || s == TRACK_RECORDING || s == TRACK_ARMED) return true;
    }
    return false;
}

// Unlike anyTrackActive() above, this is about whether there's recorded
// data worth not losing, not whether anything's currently making sound --
// a STOPPED track holds a full loop's worth of events but isn't "active".
// A fresh ARMED track doesn't count: nothing's been captured yet, there's
// nothing to lose -- but ARMED can also mean "an existing stopped track
// armed for overdub" (see toggleRecordOnSelected()'s TRACK_STOPPED case),
// which very much does have content, distinguished the same way
// elsewhere by lengthMs > 0. Used to decide whether a load (which
// overwrites in place, with no undo) needs to warn before clobbering a
// track -- see updateLoadPick().
bool trackHasContent(const LoopTrack& t) {
    if (t.state == TRACK_EMPTY) return false;
    if (t.state == TRACK_ARMED) return t.lengthMs > 0;
    return true;
}

bool anyTrackHasContent() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (trackHasContent(tracks[i])) return true;
    }
    return false;
}

// "All Stop" -- called both from NAV's held panic-stop gesture (see
// handleStopInput()) and once the user confirms leaving the looper while
// loops are running (updateExitConfirm()): freezes every active track
// (committing any in-progress recording first) rather than erasing
// anything, so coming back into Looper mode later finds everything
// intact, just stopped. Also cancels a click-only pre-roll (see
// startMetronomeClickOnly()) if one's running with nothing armed/recording
// yet to actually stop -- "stop everything" should silence that too,
// rather than leaving it clicking on its own with no way back to it short
// of toggling Metro off.
void stopAllActiveTracks() {
    metronomeClickOnlyActive = false;
    for (int i = 0; i < NUM_TRACKS; i++) commitAndStop(tracks[i]);
}

// "All Start" (EDIT held, see handleRecordInput()): resumes every track
// that has content but is currently stopped -- the counterpart to All
// Stop. Empty/armed tracks (nothing to play) and tracks that are already
// playing or muted (nothing to resume) are left untouched, per "start
// any loops with content but not empty ones".
void startAllStoppedTracks() {
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.state == TRACK_STOPPED && t.lengthMs > 0) resumeFromStop(t);
    }
}

// -- MIDI clock / transport (see handleMidiRealtime()) -------------------

// Only redraws while this mode's own main screen is actually on display --
// a real-time MIDI event can arrive at any moment regardless of what's
// currently drawn (unlike button input, which only ever gets processed
// from within updateMainScreen() to begin with), so unlike the rest of
// this file's redrawTrackRow()/redrawAllTrackRows() call sites, this one
// can't assume SCREEN_MAIN is showing. Returning to it later already
// forces a full redraw (see enter()/back-out paths), so skipping here
// loses nothing.
void redrawAllTrackRowsIfVisible() {
    if (screen == SCREEN_MAIN) redrawAllTrackRows();
}

// MIDI Start (see handleMidiRealtime()): every track with content --
// except one mid-recording, which this must not clobber -- jumps to
// position 0 and plays, same "make elapsedRunMs() come out to 0 right
// now" trick resumeFromStop() uses. A muted track stays muted (Start
// shouldn't silently unmute anything); every other content-having state
// (PLAYING/PAUSED/STOPPED/overdub-armed) becomes PLAYING.
void midiTransportStart() {
    bool any = false;
    uint32_t now = millis();
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (!trackHasContent(t) || t.state == TRACK_RECORDING) continue;
        t.stopAccumMs = now - epochFor(t);
        t.playCursor = 0;
        t.prevPos = 0;
        t.resumePending = false;
        if (t.state != TRACK_MUTED) t.state = TRACK_PLAYING;
        any = true;
    }
    if (any) redrawAllTrackRowsIfVisible();
}

// MIDI Stop (see handleMidiRealtime()): freezes every currently-
// playing/muted track in place, mirroring togglePauseOnSelected()'s own
// Pause branch. Also cancels any pending quantized resume on every track
// (not just the ones frozen here) -- Stop is meant as a hard "everything
// halts now", so a track that was mid-wait for a beat/bar boundary
// shouldn't spring back to life shortly afterward.
void midiTransportStop() {
    bool any = false;
    uint32_t now = millis();
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        t.resumePending = false;
        if (t.state == TRACK_PLAYING || t.state == TRACK_MUTED) {
            allNotesOffForTrack(t); // avoid a stuck note, same as Pause/Stop
            t.stopStartMs = now;
            t.state = TRACK_PAUSED;
            any = true;
        }
    }
    if (any) redrawAllTrackRowsIfVisible();
}

// MIDI Continue (see handleMidiRealtime()): resumes every currently-
// paused track immediately, in place -- never quantized to a beat/bar
// boundary the way the PLAY button's Sync-mode resume can be, since an
// external sequencer's Continue is itself the timing signal to resume on.
// A track sitting at position 0 (frozen there since Stop, or never
// started) naturally starts from there -- finishResumeFromPause() doesn't
// move the position, just un-freezes it.
void midiTransportContinue() {
    bool any = false;
    uint32_t now = millis();
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.state == TRACK_PAUSED) {
            finishResumeFromPause(t, now);
            any = true;
        }
    }
    if (any) redrawAllTrackRowsIfVisible();
}

// MIDI Clock (24 pulses per quarter note): tracks a smoothed tempo
// estimate from inter-pulse timing regardless of Clock Source, so
// switching to Slave mid-stream doesn't start from a cold estimate.
// Actually driving manualBpm is gated on SettingsMode::clockSourceSlave()
// -- this is tempo-follow only (a continuously updated BPM number that
// the rest of this file's existing wall-clock timing math then runs
// with), not a hard per-pulse phase lock; position/timing precision is
// otherwise identical to Internal mode, just with the BPM itself tracking
// the incoming clock instead of the manually-set value.
uint32_t clockLastPulseMs = 0;
bool clockLastPulseValid = false;
float clockSmoothedIntervalMs = 0.0f;
int clockLastAppliedBpmInt = -1;

void onMidiClockTick() {
    uint32_t now = millis();
    if (clockLastPulseValid) {
        uint32_t interval = now - clockLastPulseMs;
        // Ignore implausible gaps (external gear paused/reconnected) rather
        // than letting one huge interval corrupt the running average.
        if (interval > 0 && interval < 2000) {
            const float EMA_ALPHA = 0.2f;
            clockSmoothedIntervalMs = (clockSmoothedIntervalMs <= 0.0f)
                ? (float)interval
                : clockSmoothedIntervalMs + EMA_ALPHA * ((float)interval - clockSmoothedIntervalMs);
        }
    }
    clockLastPulseMs = now;
    clockLastPulseValid = true;

    if (!SettingsMode::clockSourceSlave() || clockSmoothedIntervalMs <= 0.0f) return;

    float bpm = 60000.0f / (clockSmoothedIntervalMs * 24.0f);
    if (bpm < 20.0f) bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    manualBpm = bpm;

    // Only rescale/redraw when the displayed integer actually changes --
    // otherwise sub-BPM jitter from the EMA would churn bar-quantized
    // track lengths and the BPM row far more often than anything on
    // screen could usefully show.
    int bpmInt = (int)(bpm + 0.5f);
    if (bpmInt == clockLastAppliedBpmInt) return;
    clockLastAppliedBpmInt = bpmInt;
    rescaleBarQuantizedTracks();
    if (screen == SCREEN_MAIN) {
        Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                             false, countInEnabled, countInBars, false, (int)bpmRowFocus, onBpmRow);
    }
}

// Registered with MidiOutput::setRealtimeHandler() in enter(). Clock
// tempo-tracking always runs; Start/Stop/Continue only act while MIDI
// Transport is enabled (see SettingsMode::midiTransportEnabled()) --
// otherwise external transport messages are just thru'd (if Thru is on)
// without this app reacting to them.
void handleMidiRealtime(MidiRealtimeEvent event) {
    switch (event) {
        case MIDI_RT_CLOCK:
            onMidiClockTick();
            break;
        case MIDI_RT_START:
            if (SettingsMode::midiTransportEnabled()) midiTransportStart();
            break;
        case MIDI_RT_STOP:
            if (SettingsMode::midiTransportEnabled()) midiTransportStop();
            break;
        case MIDI_RT_CONTINUE:
            if (SettingsMode::midiTransportEnabled()) midiTransportContinue();
            break;
    }
}

// Reset fields individually rather than `t = LoopTrack()` -- each track
// holds a MAX_EVENTS_PER_TRACK-sized buffer (~16KB), so constructing a
// whole temporary LoopTrack on the stack here would be a large, avoidable
// stack spike. channelIn/channelOut and barLength are deliberately
// untouched -- all are per-track setup that callers either want preserved
// (erase-and-re-record) or overwrite themselves right after (loading saved
// content).
void resetTrack(LoopTrack& t) {
    if (t.state != TRACK_EMPTY) allNotesOffForTrack(t);
    t.state = TRACK_EMPTY;
    t.lengthMs = 0;
    t.epochMs = 0;
    t.recordStartMs = 0;
    t.stopAccumMs = 0;
    t.stopStartMs = 0;
    t.resumePending = false; // in case a Sync-mode restart was queued (see togglePauseOnSelected()) when this got erased
    t.pendingResumeAtMs = 0;
    t.lastRecordActivityMs = 0;
    t.lastPlaybackActivityMs = 0;
    t.prevPos = 0;
    t.playCursor = 0;
    t.eventCount = 0;
    t.bufferFull = false;
}

void eraseSelected() {
    resetTrack(tracks[selectedTrack]);
}

// "Erase All Tracks" (menu): unlike "Erase Track N", this ignores
// selectedTrack entirely -- it always resets all 4, not whichever one
// happens to be selected when the menu was opened.
void eraseAllTracks() {
    for (int i = 0; i < NUM_TRACKS; i++) resetTrack(tracks[i]);
}

// delta is +1/-1. Wraps through all 16 real channels plus OMNI_CHANNEL.
void adjustChannelInOnSelected(int delta) {
    LoopTrack& t = tracks[selectedTrack];
    int ch = (int)t.channelIn + delta;
    if (ch < 0) ch = OMNI_CHANNEL;
    if (ch > OMNI_CHANNEL) ch = 0;
    t.channelIn = (uint8_t)ch;
    // Already-recorded events keep whatever channel they were captured
    // with (it's baked into their stored status byte) -- this only
    // changes what future recording on this track listens for.
}

// delta is +1/-1. Wraps through all 16 real channels plus
// CHANNEL_AS_RECORDED. Unlike channelIn, this affects already-recorded
// content too -- see updatePlayback()'s remap.
void adjustChannelOutOnSelected(int delta) {
    LoopTrack& t = tracks[selectedTrack];
    int ch = (int)t.channelOut + delta;
    if (ch < 0) ch = CHANNEL_AS_RECORDED;
    if (ch > CHANNEL_AS_RECORDED) ch = 0;
    t.channelOut = (uint8_t)ch;
}

// delta is +1/-1. Cycles through BAR_PRESETS (Freeform, 1/2/4/8/16/32/64/128).
// Only affects future fresh recordings on this track (see
// updateBarQuantizedRecording()) -- doesn't touch whatever it currently holds.
void cycleBarLengthOnSelected(int delta) {
    LoopTrack& t = tracks[selectedTrack];
    int idx = 0;
    for (int i = 0; i < BAR_PRESET_COUNT; i++) {
        if (BAR_PRESETS[i] == t.barLength) { idx = i; break; }
    }
    idx += delta;
    if (idx < 0) idx = BAR_PRESET_COUNT - 1;
    if (idx >= BAR_PRESET_COUNT) idx = 0;
    t.barLength = BAR_PRESETS[idx];
}

// Recomputes every non-stopped track's playCursor/prevPos against
// whichever epoch (shared vs per-track) now applies, so flipping the
// toggle doesn't double-fire or skip events on the next tick. Stopped
// tracks are left alone -- they don't use the cursor while frozen.
void toggleSyncMode() {
    syncMode = !syncMode;
    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.lengthMs == 0 || t.state == TRACK_STOPPED) continue;
        resyncCursor(t);
    }
}

// -- SD persistence -------------------------------------------------

void writeVarLen(FsFile& file, uint32_t value, uint32_t& bytesWritten) {
    uint32_t buffer = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer <<= 8;
        buffer |= 0x80 | (value & 0x7F);
    }
    for (;;) {
        uint8_t b = (uint8_t)(buffer & 0xFF);
        file.write(&b, 1);
        bytesWritten++;
        if (buffer & 0x80) buffer >>= 8;
        else break;
    }
}

// Serializes one track's event buffer as a standalone format-0 SMF.
// Ticks use a fixed synthetic division/tempo (same convention as
// MidiRecorder) -- there's no real "performance tempo" here since
// recording isn't quantized to any grid, so this is just for playback
// tools to show sane timing. Always writes an explicit End-of-Track at
// the *full loop length* (not just the last event) so a future Load can
// recover the exact loop length even if nothing was played right at the
// boundary.
bool saveTrackToFile(const LoopTrack& t, const char* path) {
    FsFile file;
    if (!file.open(path, O_RDWR | O_CREAT | O_TRUNC)) return false;

    const uint16_t division = 480;
    const uint32_t usPerQuarter = 500000; // synthetic 120 BPM

    const uint8_t mthd[14] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6,
        0, 0, // format 0
        0, 1, // ntrks = 1
        (uint8_t)(division >> 8), (uint8_t)(division & 0xFF),
    };
    file.write(mthd, sizeof(mthd));

    file.write((const uint8_t*)"MTrk", 4);
    uint32_t mtrkLenPos = file.curPosition();
    const uint8_t zero[4] = {0, 0, 0, 0};
    file.write(zero, 4);
    uint32_t bytesWritten = 0;

    const uint8_t tempoEvt[6] = {
        0xFF, 0x51, 0x03,
        (uint8_t)(usPerQuarter >> 16), (uint8_t)(usPerQuarter >> 8), (uint8_t)usPerQuarter,
    };
    writeVarLen(file, 0, bytesWritten);
    file.write(tempoEvt, sizeof(tempoEvt));
    bytesWritten += sizeof(tempoEvt);

    uint32_t lastTick = 0;
    for (int i = 0; i < t.eventCount; i++) {
        const LoopEvent& e = t.events[i];
        uint32_t tick = (uint32_t)(((uint64_t)e.offsetMs * 1000 * division) / usPerQuarter);
        writeVarLen(file, tick - lastTick, bytesWritten);
        lastTick = tick;
        uint8_t len = dataBytesForStatus(e.status);
        const uint8_t evt[3] = {e.status, e.data1, e.data2};
        file.write(evt, 1 + len);
        bytesWritten += 1 + len;
    }

    uint32_t endTick = (uint32_t)(((uint64_t)t.lengthMs * 1000 * division) / usPerQuarter);
    if (endTick < lastTick) endTick = lastTick;
    writeVarLen(file, endTick - lastTick, bytesWritten);
    const uint8_t eot[3] = {0xFF, 0x2F, 0x00};
    file.write(eot, sizeof(eot));
    bytesWritten += sizeof(eot);

    uint32_t endPos = file.curPosition();
    const uint8_t lenBytes[4] = {
        (uint8_t)(bytesWritten >> 24), (uint8_t)(bytesWritten >> 16),
        (uint8_t)(bytesWritten >> 8), (uint8_t)bytesWritten,
    };
    file.seekSet(mtrkLenPos);
    file.write(lenBytes, 4);
    file.seekSet(endPos);

    file.sync();
    file.close();
    return true;
}

// Picks "LOOP001", "LOOP002", etc. under BUNDLES_ROOT -- same pattern as
// FilePlayerMode's REC00N auto-naming.
void generateSessionName(char* out, size_t outSize) {
    int nextNum = 1;
    FsFile dir = sd.open(BUNDLES_ROOT);
    if (dir && dir.isDir()) {
        FsFile entry;
        char name[32];
        while (entry.openNext(&dir, O_RDONLY)) {
            if (entry.isDir()) {
                entry.getName(name, sizeof(name));
                if (strncasecmp(name, "LOOP", 4) == 0) {
                    int num = atoi(name + 4);
                    if (num >= nextNum) nextNum = num + 1;
                }
            }
            entry.close();
        }
    }
    if (dir) dir.close();
    snprintf(out, outSize, "LOOP%03d", nextNum);
}

// Picks "Loop001", "Loop002", etc. directly under LOOPS_ROOT (not
// BUNDLES_ROOT) -- individual loop saves are loose files at the loops
// root, one level up from where the bundle folders live, and this scans
// past those folders (isDir() is skipped) so a same-numbered bundle never
// collides with an individual file's suggested name.
void generateLoopFileName(char* out, size_t outSize) {
    int nextNum = 1;
    FsFile dir = sd.open(LOOPS_ROOT);
    if (dir && dir.isDir()) {
        FsFile entry;
        char name[32];
        while (entry.openNext(&dir, O_RDONLY)) {
            if (!entry.isDir()) {
                entry.getName(name, sizeof(name));
                if (strncasecmp(name, "Loop", 4) == 0) {
                    int num = atoi(name + 4);
                    if (num >= nextNum) nextNum = num + 1;
                }
            }
            entry.close();
        }
    }
    if (dir) dir.close();
    snprintf(out, outSize, "Loop%03d", nextNum);
}

// Saves just the selected track as a standalone .mid directly under
// LOOPS_ROOT -- no session.txt, no channelIn/channelOut/barLength
// metadata (same "no metadata" contract importFileIntoTrack() already
// uses to load one of these back in, see its own comment). `path` is
// opened O_TRUNC, so this doubles as the overwrite path -- no separate
// remove needed when the name already exists (see
// updateSaveLoopOverwrite()).
bool saveSelectedLoop(const char* name) {
    if (!sd.exists(LOOPS_ROOT)) sd.mkdir(LOOPS_ROOT);
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.mid", LOOPS_ROOT, name);
    return saveTrackToFile(tracks[selectedTrack], path);
}

void saveSession() {
    if (!sd.exists(LOOPS_ROOT)) sd.mkdir(LOOPS_ROOT);
    if (!sd.exists(BUNDLES_ROOT)) sd.mkdir(BUNDLES_ROOT);

    char sessionName[16];
    generateSessionName(sessionName, sizeof(sessionName));
    char sessionPath[64];
    snprintf(sessionPath, sizeof(sessionPath), "%s/%s", BUNDLES_ROOT, sessionName);

    if (!sd.mkdir(sessionPath)) {
        showFlashMessage("Save failed", "could not create folder", 1500);
        return;
    }

    char metaPath[96];
    snprintf(metaPath, sizeof(metaPath), "%s/session.txt", sessionPath);
    FsFile meta;
    bool metaOk = meta.open(metaPath, O_RDWR | O_CREAT | O_TRUNC);
    if (metaOk) {
        char line[64];
        int n = snprintf(line, sizeof(line), "sync=%d\nbpm=%d\ntimeSigNum=%d\ntimeSigDen=%d\n",
                          syncMode ? 1 : 0, (int)manualBpm, timeSigNum, timeSigDen);
        meta.write((const uint8_t*)line, n);
    }

    bool anySaved = false;
    for (int i = 0; i < NUM_TRACKS; i++) {
        const LoopTrack& t = tracks[i];
        if (t.state == TRACK_EMPTY || t.lengthMs == 0) continue;

        char trackPath[112];
        snprintf(trackPath, sizeof(trackPath), "%s/track%d.mid", sessionPath, i + 1);
        if (saveTrackToFile(t, trackPath)) anySaved = true;

        if (metaOk) {
            char inStr[8];
            if (t.channelIn == OMNI_CHANNEL) snprintf(inStr, sizeof(inStr), "OMNI");
            else snprintf(inStr, sizeof(inStr), "%d", t.channelIn + 1);
            char outStr[8];
            if (t.channelOut == CHANNEL_AS_RECORDED) snprintf(outStr, sizeof(outStr), "REC");
            else snprintf(outStr, sizeof(outStr), "%d", t.channelOut + 1);
            // barLength isn't recoverable from the .mid file itself (a
            // freeform take and a bar-quantized one can produce identical
            // event/length data) -- lengthMs is written too, but only for
            // reference: loadTrackFromFile() recovers the exact value from
            // the file's own End-of-Track event instead.
            char line[128];
            int n = snprintf(line, sizeof(line),
                              "track%d.channelIn=%s\ntrack%d.channelOut=%s\ntrack%d.barLength=%d\ntrack%d.lengthMs=%lu\n",
                              i + 1, inStr, i + 1, outStr, i + 1, t.barLength, i + 1, (unsigned long)t.lengthMs);
            meta.write((const uint8_t*)line, n);
        }
    }
    if (metaOk) { meta.sync(); meta.close(); }

    char msg[32];
    snprintf(msg, sizeof(msg), "Saved as %s", sessionName);
    showFlashMessage(msg, anySaved ? nullptr : "(no tracks had content)", 1200);
}

// Scans BUNDLES_ROOT for saved session folders into savedSessionNames[],
// sorted alphabetically (a plain insertion sort -- MAX_SAVED_SESSIONS is
// small enough that O(n^2) doesn't matter). Used by SCREEN_DELETE_BROWSE.
void loadSavedSessions() {
    savedSessionCount = 0;
    FsFile dir = sd.open(BUNDLES_ROOT);
    if (dir && dir.isDir()) {
        FsFile entry;
        while (savedSessionCount < MAX_SAVED_SESSIONS && entry.openNext(&dir, O_RDONLY)) {
            if (entry.isDir()) {
                entry.getName(savedSessionNames[savedSessionCount], sizeof(savedSessionNames[0]));
                savedSessionCount++;
            }
            entry.close();
        }
    }
    if (dir) dir.close();

    for (int i = 1; i < savedSessionCount; i++) {
        char key[16];
        strncpy(key, savedSessionNames[i], sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        int j = i - 1;
        while (j >= 0 && strcasecmp(savedSessionNames[j], key) > 0) {
            strncpy(savedSessionNames[j + 1], savedSessionNames[j], sizeof(savedSessionNames[0]));
            j--;
        }
        strncpy(savedSessionNames[j + 1], key, sizeof(savedSessionNames[0]));
    }
}

// Removes every file inside the session folder, then the (now-empty)
// folder itself -- SD FAT's rmdir() requires an empty directory, and
// saveSession() only writes a .mid for tracks that actually had content
// (never a fixed set of 4 files), so this removes whatever's actually
// there rather than guessing which of track1-4.mid/session.txt exist.
bool deleteSavedSession(const char* name) {
    char sessionPath[64];
    snprintf(sessionPath, sizeof(sessionPath), "%s/%s", BUNDLES_ROOT, name);

    FsFile dir = sd.open(sessionPath);
    if (dir && dir.isDir()) {
        FsFile entry;
        char entryName[32];
        while (entry.openNext(&dir, O_RDONLY)) {
            entry.getName(entryName, sizeof(entryName));
            entry.close();
            char filePath[112];
            snprintf(filePath, sizeof(filePath), "%s/%s", sessionPath, entryName);
            sd.remove(filePath);
        }
    }
    if (dir) dir.close();
    return sd.rmdir(sessionPath);
}

// -- SD loading -----------------------------------------------------------

bool readChunkHeaderLocal(FsFile& f, char id[5], uint32_t& length) {
    uint8_t buf[8];
    if (f.read(buf, 8) != 8) return false;
    id[0] = buf[0]; id[1] = buf[1]; id[2] = buf[2]; id[3] = buf[3]; id[4] = '\0';
    length = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
             ((uint32_t)buf[6] << 8) | buf[7];
    return true;
}

bool readVarLen(FsFile& f, uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; i++) {
        int b = f.read();
        if (b < 0) return false;
        value = (value << 7) | (uint32_t)(b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true; // malformed (>4 continuation bytes) -- accept what we have
}

// Reverses saveTrackToFile(): reads a format-0 single-track SMF this same
// firmware wrote and rebuilds t.events[]/eventCount/lengthMs from it.
// Not a general SMF loader -- saveTrackToFile() always writes an explicit
// status byte per event (no running status) and a fixed synthetic
// division/tempo, so this only needs to reverse exactly that, not handle
// arbitrary files. lengthMs comes from the End-of-Track event's own tick,
// not the last note event -- see saveTrackToFile()'s comment on why that
// was written that way. Leaves t.channelIn/channelOut/barLength untouched
// -- none is recoverable from the file itself, callers fill them in from
// the session's metadata (see readSessionMeta()) afterward.
bool loadTrackFromFile(LoopTrack& t, const char* path) {
    FsFile file;
    if (!file.open(path, O_RDONLY)) return false;

    char id[5];
    uint32_t chunkLen;
    if (!readChunkHeaderLocal(file, id, chunkLen) || strcmp(id, "MThd") != 0 || chunkLen < 6) {
        file.close();
        return false;
    }
    uint8_t hdr[6];
    if (file.read(hdr, 6) != 6) { file.close(); return false; }
    uint16_t division = ((uint16_t)hdr[4] << 8) | hdr[5];
    if (division == 0) division = 480;
    if (chunkLen > 6) file.seekCur(chunkLen - 6);

    if (!readChunkHeaderLocal(file, id, chunkLen) || strcmp(id, "MTrk") != 0) {
        file.close();
        return false;
    }
    uint32_t chunkEnd = file.curPosition() + chunkLen;

    const uint32_t usPerQuarter = 500000; // must match saveTrackToFile()'s synthetic tempo

    t.eventCount = 0;
    t.bufferFull = false;
    t.lengthMs = 0;
    uint32_t absTick = 0;
    uint8_t runningStatus = 0;
    bool ok = true;

    while (file.curPosition() < chunkEnd) {
        uint32_t delta;
        if (!readVarLen(file, delta)) { ok = false; break; }
        absTick += delta;

        int b0i = file.read();
        if (b0i < 0) { ok = false; break; }
        uint8_t b0 = (uint8_t)b0i;
        uint8_t status, firstData = 0;
        bool haveFirst = false;
        if (b0 & 0x80) {
            status = b0;
            if (status < 0xF0) runningStatus = status;
        } else {
            status = runningStatus;
            firstData = b0;
            haveFirst = true;
            if (status == 0) { ok = false; break; }
        }

        if (status == 0xFF) {
            int metaTypeI = file.read();
            if (metaTypeI < 0) { ok = false; break; }
            uint8_t metaType = (uint8_t)metaTypeI;
            uint32_t metaLen;
            if (!readVarLen(file, metaLen)) { ok = false; break; }
            if (metaType == 0x2F) { // End of Track -- its tick IS the loop length
                t.lengthMs = (uint32_t)(((uint64_t)absTick * usPerQuarter) / ((uint64_t)division * 1000));
                break;
            }
            if (metaLen > 0) file.seekCur(metaLen); // e.g. the leading tempo meta -- already known
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t sysExLen;
            if (!readVarLen(file, sysExLen)) { ok = false; break; }
            if (sysExLen > 0) file.seekCur(sysExLen);
        } else if (status >= 0x80 && status < 0xF0) {
            uint8_t need = dataBytesForStatus(status);
            uint8_t d1, d2 = 0;
            if (haveFirst) {
                d1 = firstData;
            } else {
                int r = file.read();
                if (r < 0) { ok = false; break; }
                d1 = (uint8_t)r;
            }
            if (need >= 2) {
                int r = file.read();
                if (r < 0) { ok = false; break; }
                d2 = (uint8_t)r;
            }
            if (t.eventCount < MAX_EVENTS_PER_TRACK) {
                LoopEvent& e = t.events[t.eventCount++];
                e.offsetMs = (uint32_t)(((uint64_t)absTick * usPerQuarter) / ((uint64_t)division * 1000));
                e.status = status;
                e.data1 = d1;
                e.data2 = d2;
            } else {
                t.bufferFull = true;
            }
        } else {
            ok = false; // unexpected system message -- not something this writer ever emits
            break;
        }
    }

    file.close();
    return ok && t.lengthMs > 0;
}

// Common finish-up for any freshly loaded/imported track (whole-session
// load, single saved-track load, or an arbitrary file import): parks it
// TRACK_STOPPED with its own position-zero pinned to right now, so it
// starts at the top of its loop the first time it's played rather than
// wherever elapsedRunMs() would otherwise land it -- same trick
// resumeFromStop() uses, just landing in STOPPED instead of PLAYING since
// loaded content shouldn't start making sound on its own.
void parkLoadedTrack(LoopTrack& t) {
    uint32_t now = millis();
    // Sync mode's shared epoch may never have been established yet (only
    // startArmedTracksTogether() normally sets it) if nothing was ever
    // recorded before this load -- establish it now, same as that does.
    if (syncMode && !sessionEpochSet) { sessionEpochMs = now; sessionEpochSet = true; }
    t.epochMs = now;
    t.playCursor = 0;
    t.prevPos = 0;
    t.state = TRACK_STOPPED;
    t.stopStartMs = now;
    t.stopAccumMs = now - epochFor(t);
}

// -- SD loading: arbitrary MIDI files (not just this firmware's own) -----
//
// loadTrackFromFile() above only needs to reverse the exact fixed-tempo,
// single-track, no-running-status format saveTrackToFile() writes. A file
// dropped in from the file browser could be anything a DAW exports:
// format 0 or 1, multiple tracks, running status, real tempo changes
// mid-file. Rather than duplicate MidiPlayer's already-correct multi-
// track-merge/tempo-map logic by reaching into its private internals,
// this reimplements the same approach (see midi_file.cpp's primeTrack()/
// findNextTrack()/update()) as a batch pass instead of a real-time one:
// merge every track's events into one flat, time-sorted buffer instead of
// sending them out live.

struct ImportTrackReader {
    FsFile file;
    uint32_t chunkEnd = 0;
    uint32_t absoluteTick = 0;
    uint8_t runningStatus = 0;
    bool ended = false;

    // Next staged (not-yet-consumed) event, same three-way shape as
    // MidiPlayer::TrackReader -- a real channel-voice event, a tempo
    // change, or (unlike MidiPlayer, which just ends the track silently)
    // this track's own End-of-Track, staged as a schedulable pseudo-event
    // so its tick still advances the shared elapsed-time accumulator in
    // the merge loop below -- otherwise a track that ends with trailing
    // silence would have that silence silently dropped from the loop
    // length, same reasoning as saveTrackToFile()'s comment on why EOT is
    // always written at the full length rather than the last event.
    uint32_t pendingTick = 0;
    bool pendingIsTempo = false;
    bool pendingIsEnd = false;
    uint32_t pendingTempoUsPerQuarter = 500000;
    uint8_t pendingStatus = 0, pendingData1 = 0, pendingData2 = 0;
};

bool primeImportTrack(ImportTrackReader& t) {
    while (true) {
        if (!t.file || t.file.curPosition() >= t.chunkEnd) { t.ended = true; return false; }

        uint32_t delta;
        if (!readVarLen(t.file, delta)) { t.ended = true; return false; }
        t.absoluteTick += delta;

        int b0i = t.file.read();
        if (b0i < 0) { t.ended = true; return false; }
        uint8_t b0 = (uint8_t)b0i;
        uint8_t status, firstData = 0;
        bool haveFirst = false;
        if (b0 & 0x80) {
            status = b0;
            if (status < 0xF0) t.runningStatus = status;
        } else {
            status = t.runningStatus;
            firstData = b0;
            haveFirst = true;
            if (status == 0) { t.ended = true; return false; }
        }

        if (status == 0xFF) {
            int metaTypeI = t.file.read();
            if (metaTypeI < 0) { t.ended = true; return false; }
            uint8_t metaType = (uint8_t)metaTypeI;
            uint32_t metaLen;
            if (!readVarLen(t.file, metaLen)) { t.ended = true; return false; }

            if (metaType == 0x51 && metaLen == 3) { // Set Tempo
                uint8_t d[3];
                if (t.file.read(d, 3) != 3) { t.ended = true; return false; }
                t.pendingTick = t.absoluteTick;
                t.pendingIsTempo = true;
                t.pendingIsEnd = false;
                t.pendingTempoUsPerQuarter = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
                return true;
            } else if (metaType == 0x2F) { // End of Track
                t.pendingTick = t.absoluteTick;
                t.pendingIsTempo = false;
                t.pendingIsEnd = true;
                return true;
            } else {
                if (metaLen > 0) t.file.seekCur(metaLen);
                continue;
            }
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t sysExLen;
            if (!readVarLen(t.file, sysExLen)) { t.ended = true; return false; }
            if (sysExLen > 0) t.file.seekCur(sysExLen);
            continue;
        } else if (status >= 0x80 && status < 0xF0) {
            uint8_t need = dataBytesForStatus(status);
            uint8_t d1, d2 = 0;
            if (haveFirst) {
                d1 = firstData;
            } else {
                int r = t.file.read();
                if (r < 0) { t.ended = true; return false; }
                d1 = (uint8_t)r;
            }
            if (need >= 2) {
                int r = t.file.read();
                if (r < 0) { t.ended = true; return false; }
                d2 = (uint8_t)r;
            }
            t.pendingTick = t.absoluteTick;
            t.pendingIsTempo = false;
            t.pendingIsEnd = false;
            t.pendingStatus = status;
            t.pendingData1 = d1;
            t.pendingData2 = d2;
            return true;
        } else {
            t.ended = true; // unexpected system message -- not something a real SMF should contain
            return false;
        }
    }
}

// Parses an arbitrary Standard MIDI File at `path` (format 0 or 1; format
// 2 and SMPTE division are rejected, same limitations MidiPlayer states)
// and rebuilds t.events[]/eventCount/lengthMs from the merged, time-
// sorted union of every track's channel-voice events. Leaves
// channel/barLength/state untouched, same contract as loadTrackFromFile()
// -- see importFileIntoTrack() for the wrapper that fills those in.
bool importMidiEvents(LoopTrack& t, const char* path) {
    FsFile hdr;
    if (!hdr.open(path, O_RDONLY)) return false;

    char id[5];
    uint32_t len;
    if (!readChunkHeaderLocal(hdr, id, len) || strcmp(id, "MThd") != 0 || len < 6) {
        hdr.close();
        return false;
    }
    uint8_t hb[6];
    if (hdr.read(hb, 6) != 6) { hdr.close(); return false; }
    uint16_t format = ((uint16_t)hb[0] << 8) | hb[1];
    uint16_t ntrks = ((uint16_t)hb[2] << 8) | hb[3];
    uint16_t division = ((uint16_t)hb[4] << 8) | hb[5];
    if (len > 6) hdr.seekCur(len - 6);

    if ((division & 0x8000) || division == 0 || format == 2 || ntrks == 0) {
        hdr.close();
        return false;
    }
    // A smaller cap than MidiPlayer's own MIDI_MAX_TRACKS (64) -- real-
    // world files needing that many simultaneous track readers are rare,
    // and each reader holds a whole open FsFile, so this is a meaningful
    // static RAM cost (unlike MidiPlayer's, this array is only alive
    // during import, but "static" avoids a one-off stack spike for it).
    const int IMPORT_MAX_TRACKS = 16;
    if (ntrks > IMPORT_MAX_TRACKS) ntrks = IMPORT_MAX_TRACKS; // best-effort: import what we can open

    static ImportTrackReader readers[IMPORT_MAX_TRACKS];
    int numReaders = 0;

    for (uint16_t i = 0; i < ntrks; i++) {
        char tid[5];
        uint32_t tlen;
        if (!readChunkHeaderLocal(hdr, tid, tlen)) break;
        uint32_t trackStart = hdr.curPosition();

        if (strcmp(tid, "MTrk") != 0) {
            hdr.seekCur(tlen); // unknown chunk type, skip it
            continue;
        }

        ImportTrackReader& r = readers[numReaders];
        r = ImportTrackReader(); // reset in case this slot was used by a previous import
        if (!r.file.open(path, O_RDONLY)) { hdr.seekCur(tlen); continue; }
        r.file.seekSet(trackStart);
        r.chunkEnd = trackStart + tlen;
        hdr.seekSet(trackStart + tlen); // advance master cursor past this track

        if (primeImportTrack(r)) {
            numReaders++;
        } else {
            r.file.close(); // empty track (e.g. just an immediate EOT with nothing staged) -- doesn't happen given pendingIsEnd above, but stay defensive
        }
    }
    hdr.close();

    if (numReaders == 0) return false;

    resetTrack(t);

    uint32_t usPerQuarter = 500000; // 120 BPM until a tempo meta says otherwise, same as MidiPlayer
    uint32_t originTick = 0;
    uint64_t elapsedUs = 0;

    for (;;) {
        int best = -1;
        uint32_t bestTick = 0;
        for (int i = 0; i < numReaders; i++) {
            if (readers[i].ended) continue;
            if (best < 0 || readers[i].pendingTick < bestTick) {
                best = i;
                bestTick = readers[i].pendingTick;
            }
        }
        if (best < 0) break; // every track exhausted

        ImportTrackReader& r = readers[best];
        uint32_t deltaTicks = r.pendingTick - originTick;
        elapsedUs += ((uint64_t)deltaTicks * usPerQuarter) / division;
        originTick = r.pendingTick;

        if (r.pendingIsEnd) {
            r.ended = true;
        } else if (r.pendingIsTempo) {
            usPerQuarter = r.pendingTempoUsPerQuarter;
            primeImportTrack(r);
        } else {
            if (t.eventCount < MAX_EVENTS_PER_TRACK) {
                LoopEvent& e = t.events[t.eventCount++];
                e.offsetMs = (uint32_t)(elapsedUs / 1000);
                e.status = r.pendingStatus;
                e.data1 = r.pendingData1;
                e.data2 = r.pendingData2;
            } else {
                t.bufferFull = true;
            }
            primeImportTrack(r);
        }
    }

    for (int i = 0; i < numReaders; i++) {
        if (readers[i].file) readers[i].file.close();
    }

    t.lengthMs = (uint32_t)(elapsedUs / 1000);
    if (t.lengthMs == 0) t.lengthMs = 1; // guard against a degenerate all-zero-length file
    return true;
}

// Imports an arbitrary Standard MIDI File into t -- unlike
// loadTrackIntoSlot() (which loads this firmware's own saved sessions and
// gets channelIn/channelOut/barLength from their metadata), there's no
// metadata for an arbitrary file, so this resets to the same sensible
// defaults a brand new track in this slot would have (OMNI in, this slot's
// own channel out, Freeform); the user can dial any of them in afterward
// same as any other track. `trackIndex` (0-3) is only needed for that
// channelOut default -- see enter()'s comment on why it can't just be a
// LoopTrack member initializer.
bool importFileIntoTrack(LoopTrack& t, const char* path, int trackIndex) {
    if (!importMidiEvents(t, path)) return false;
    t.channelIn = OMNI_CHANNEL;
    t.channelOut = (uint8_t)trackIndex;
    t.barLength = 0;
    parkLoadedTrack(t);
    return true;
}

// Per-track fields saveSession()'s session.txt carries but a .mid file
// can't (see loadTrackFromFile()'s comment) -- channelIn, channelOut, and
// barLength. Defaults match a fresh LoopTrack's, so a key missing from an
// older or hand-edited session.txt just falls back sanely rather than
// failing.
struct SessionMeta {
    bool sync = true;
    float bpm = 120.0f;
    int timeSigNum = 4;
    int timeSigDen = 4;
    uint8_t channelIn[NUM_TRACKS] = {OMNI_CHANNEL, OMNI_CHANNEL, OMNI_CHANNEL, OMNI_CHANNEL};
    // Matches a fresh looper's own out-channel default (see enter()):
    // track N -> channel N.
    uint8_t channelOut[NUM_TRACKS] = {0, 1, 2, 3};
    int barLength[NUM_TRACKS] = {0, 0, 0, 0};
};

void parseSessionMetaLine(const char* line, SessionMeta& meta) {
    if (strncmp(line, "sync=", 5) == 0) {
        meta.sync = atoi(line + 5) != 0;
        return;
    }
    if (strncmp(line, "bpm=", 4) == 0) {
        meta.bpm = (float)atoi(line + 4);
        return;
    }
    if (strncmp(line, "timeSigNum=", 11) == 0) {
        meta.timeSigNum = atoi(line + 11);
        return;
    }
    if (strncmp(line, "timeSigDen=", 11) == 0) {
        meta.timeSigDen = atoi(line + 11);
        return;
    }
    for (int i = 0; i < NUM_TRACKS; i++) {
        char prefix[20];
        size_t plen;

        snprintf(prefix, sizeof(prefix), "track%d.channelIn=", i + 1);
        plen = strlen(prefix);
        if (strncmp(line, prefix, plen) == 0) {
            const char* v = line + plen;
            meta.channelIn[i] = (strcasecmp(v, "OMNI") == 0) ? OMNI_CHANNEL : (uint8_t)(atoi(v) - 1);
            return;
        }

        snprintf(prefix, sizeof(prefix), "track%d.channelOut=", i + 1);
        plen = strlen(prefix);
        if (strncmp(line, prefix, plen) == 0) {
            const char* v = line + plen;
            meta.channelOut[i] = (strcasecmp(v, "REC") == 0) ? CHANNEL_AS_RECORDED : (uint8_t)(atoi(v) - 1);
            return;
        }

        snprintf(prefix, sizeof(prefix), "track%d.barLength=", i + 1);
        plen = strlen(prefix);
        if (strncmp(line, prefix, plen) == 0) {
            meta.barLength[i] = atoi(line + plen);
            return;
        }
    }
    // trackN.lengthMs and anything else: ignored -- lengthMs is recovered
    // from each .mid file's own End-of-Track event instead.
}

void readSessionMeta(const char* sessionPath, SessionMeta& meta) {
    char metaPath[96];
    snprintf(metaPath, sizeof(metaPath), "%s/session.txt", sessionPath);
    FsFile file;
    if (!file.open(metaPath, O_RDONLY)) return;

    char line[64];
    int lineLen = 0;
    int c;
    while ((c = file.read()) >= 0) {
        if (c == '\n') {
            line[lineLen] = '\0';
            parseSessionMetaLine(line, meta);
            lineLen = 0;
        } else if (c != '\r') {
            if (lineLen < (int)sizeof(line) - 1) line[lineLen++] = (char)c;
        }
    }
    if (lineLen > 0) {
        line[lineLen] = '\0';
        parseSessionMetaLine(line, meta);
    }
    file.close();
}

// Wipes any existing content from t (see resetTrack()), loads new
// events/length from path, and applies channelIn/channelOut/barLength from
// the saved session's metadata. Parked TRACK_STOPPED with its own
// position-zero pinned to right now, so it starts at the top of its loop
// the first time it's played rather than wherever elapsedRunMs() would
// otherwise land it -- same trick resumeFromStop() uses, just landing in
// STOPPED instead of PLAYING since loaded content shouldn't start making
// sound on its own.
bool loadTrackIntoSlot(LoopTrack& t, const char* path, uint8_t channelIn, uint8_t channelOut, int barLength) {
    resetTrack(t);
    if (!loadTrackFromFile(t, path)) return false;
    t.channelIn = channelIn;
    t.channelOut = channelOut;
    t.barLength = barLength;
    parkLoadedTrack(t);
    return true;
}

// Populates loadPickLabels/loadPickTrackNums/loadPickLabelCount for
// SCREEN_LOAD_PICK: "Load All" plus one entry per track file the chosen
// session actually has (a track saved empty never got a .mid -- see
// saveSession()).
void buildLoadPickList(const char* sessionName) {
    char sessionPath[64];
    snprintf(sessionPath, sizeof(sessionPath), "%s/%s", BUNDLES_ROOT, sessionName);

    loadPickLabels[0] = "Load All";
    int found = 0;
    for (int i = 0; i < NUM_TRACKS; i++) {
        char trackPath[112];
        snprintf(trackPath, sizeof(trackPath), "%s/track%d.mid", sessionPath, i + 1);
        if (sd.exists(trackPath)) {
            loadPickTrackNums[found] = i + 1;
            snprintf(loadPickTrackLabels[found], sizeof(loadPickTrackLabels[0]), "Track %d", i + 1);
            loadPickLabels[1 + found] = loadPickTrackLabels[found];
            found++;
        }
    }
    loadPickLabelCount = 1 + found;
}

// Loads one saved track (savedTrackNum, 1-4) from sessionName into
// whichever live track was selected when the load menu was opened --
// same "acts on the selected track" convention as Erase Track N.
void loadSingleTrackIntoSelected(const char* sessionName, int savedTrackNum) {
    char sessionPath[64];
    snprintf(sessionPath, sizeof(sessionPath), "%s/%s", BUNDLES_ROOT, sessionName);
    SessionMeta meta;
    readSessionMeta(sessionPath, meta);

    char trackPath[112];
    snprintf(trackPath, sizeof(trackPath), "%s/track%d.mid", sessionPath, savedTrackNum);

    int idx = savedTrackNum - 1;
    bool ok = loadTrackIntoSlot(tracks[selectedTrack], trackPath, meta.channelIn[idx], meta.channelOut[idx], meta.barLength[idx]);
    // The saved track may have been recorded at a different BPM than the
    // live session is currently running at -- bring a bar-quantized
    // track's actual length back in line with its declared bar count at
    // *this* session's current tempo (a no-op if the BPMs already match).
    rescaleBarQuantizedTracks();

    char msg[32];
    if (ok) snprintf(msg, sizeof(msg), "Loaded Track %d", selectedTrack + 1);
    else snprintf(msg, sizeof(msg), "Load failed");
    showFlashMessage(msg, nullptr, 1000);
}

// Loads every track sessionName has content for into the corresponding
// live track (1:1, ignoring whatever was selected), and restores sync
// mode + BPM + time signature -- "the entire session at once". A live
// track with no corresponding saved file is cleared rather than left
// as-is, so the result is exactly what was saved, not a merge with
// whatever was live.
void loadEntireSession(const char* sessionName) {
    char sessionPath[64];
    snprintf(sessionPath, sizeof(sessionPath), "%s/%s", BUNDLES_ROOT, sessionName);
    SessionMeta meta;
    readSessionMeta(sessionPath, meta);

    syncMode = meta.sync;
    manualBpm = meta.bpm;
    timeSigNum = meta.timeSigNum;
    timeSigDen = meta.timeSigDen;

    bool anyLoaded = false;
    for (int i = 0; i < NUM_TRACKS; i++) {
        char trackPath[112];
        snprintf(trackPath, sizeof(trackPath), "%s/track%d.mid", sessionPath, i + 1);
        if (sd.exists(trackPath)) {
            if (loadTrackIntoSlot(tracks[i], trackPath, meta.channelIn[i], meta.channelOut[i], meta.barLength[i])) anyLoaded = true;
        } else {
            resetTrack(tracks[i]);
        }
    }
    rescaleBarQuantizedTracks();

    char msg[32];
    snprintf(msg, sizeof(msg), "Loaded %s", sessionName);
    showFlashMessage(msg, anyLoaded ? nullptr : "(no tracks had content)", 1200);
}

// -- input --------------------------------------------------------------

// UP/DOWN move a single circular cursor across 5 positions: the 4 tracks,
// then the BPM row, then back to track 1 -- e.g. DOWN from track 4 lands
// on BPM, DOWN again wraps to track 1; UP from track 1 wraps back to BPM.
// ALT held repurposes UP/DOWN for out-channel-adjust instead
// (handleChannelOutHold()); EDIT held has no UP/DOWN meaning on a track
// row for now (see handleBarLengthAdjust()'s comment), but still adjusts
// BPM value/Time Sig on the BPM row (handleBpmValueAdjust()/
// handleTimeSigAdjust()) -- both have to be excluded here the same way,
// otherwise holding either while pressing UP/DOWN would also move the row
// cursor out from under whatever it was trying to adjust (or, for EDIT on
// a track row, just move the cursor when it should stay put).
void handleRowCursor() {
    if (Input::isDown(BTN_ALT) || Input::isDown(BTN_EDIT)) return;

    int prevTrack = selectedTrack;
    bool prevOnBpm = onBpmRow;

    if (Input::justPressed(BTN_UP)) {
        if (onBpmRow) { onBpmRow = false; selectedTrack = NUM_TRACKS - 1; }
        else if (selectedTrack > 0) selectedTrack--;
        else onBpmRow = true;
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (onBpmRow) { onBpmRow = false; selectedTrack = 0; }
        else if (selectedTrack < NUM_TRACKS - 1) selectedTrack++;
        else onBpmRow = true;
    }

    if (selectedTrack != prevTrack || onBpmRow != prevOnBpm) {
        // Always land back on the BPM value, not wherever focus happened
        // to be left last time -- see handleBpmRowFocus().
        bpmRowFocus = BPM_FOCUS_VALUE;
        Ui::LoopTrackView views[4];
        buildTrackViews(views);
        Ui::updateLooperSelection(views, prevTrack, prevOnBpm, selectedTrack, onBpmRow, syncMode, manualBpm,
                                   timeSigNum, timeSigDen, metronomeOn, metronomeVolumePercent, false,
                                   countInEnabled, countInBars, false, (int)bpmRowFocus);
    }
}

// RIGHT/LEFT (without ALT or EDIT held) on the BPM row step focus
// forward/back through its five fields (BPM value -> Time Sig -> Metro ->
// Count In -> Sync) instead of doing nothing -- ALT+UP/DOWN then acts on
// Metro/Count In/Sync, EDIT+UP/DOWN/LEFT/RIGHT on BPM value/Time Sig (see
// handleBpmAdjust()/handleBpmValueAdjust()). EDIT has to be excluded here
// the same way ALT already is -- otherwise holding EDIT to adjust BPM
// value/Time Sig would also step focus out from under it on every
// LEFT/RIGHT press. No-op off the BPM row -- a track row gives bare
// RIGHT/LEFT their own meanings instead (Mute and back, see
// handleMuteInput()/updateMainScreen()).
void handleBpmRowFocus() {
    if (!onBpmRow || Input::isDown(BTN_ALT) || Input::isDown(BTN_EDIT)) return;

    BpmRowFocus prev = bpmRowFocus;
    if (Input::justPressed(BTN_RIGHT) && bpmRowFocus < BPM_FOCUS_SYNC) {
        bpmRowFocus = (BpmRowFocus)(bpmRowFocus + 1);
    } else if (Input::justPressed(BTN_LEFT) && bpmRowFocus > BPM_FOCUS_VALUE) {
        bpmRowFocus = (BpmRowFocus)(bpmRowFocus - 1);
    }
    if (bpmRowFocus != prev) {
        Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
    }
}

// A held EDIT press (>= HOLD_THRESHOLD_MS at release) is the "All Start"
// version of what a tap does: EDIT tap arms/records the selected track
// (see toggleRecordOnSelected()), EDIT held resumes every stopped track
// with content (unchanged from before this button's tap meaning moved
// here from PLAY). Deciding on release (rather than acting immediately
// on press) delays the single-track action slightly, which is fine here
// -- arming isn't time-critical, it just waits for a note to truly start
// recording (see toggleRecordOnSelected()'s own comment). Both are
// no-ops while the BPM row is selected -- there's no track to act on.
// (NAV's own hold gesture, just below, works differently -- see
// handleStopInput()'s comment.)
const uint32_t HOLD_THRESHOLD_MS = 500;

void handleRecordInput() {
    static uint32_t pressStartMs = 0;
    if (Input::justPressed(BTN_EDIT)) {
        pressStartMs = millis();
        editUsedForBarLength = false;
    }
    if (Input::justReleased(BTN_EDIT)) {
        // Also skip both the tap and the held ("All Start") action below
        // if this press was actually spent cycling bar length instead
        // (handleBarLengthAdjust()) -- otherwise a quick EDIT+RIGHT tap to
        // bump the bar-length preset would also arm/record the track, and
        // releasing EDIT after cycling through a few presets over a
        // longer hold would also fire "All Start", since this handler's
        // own hold-length check has no way to tell the two apart.
        if (onBpmRow || editUsedForBarLength) return;
        if (millis() - pressStartMs >= HOLD_THRESHOLD_MS) {
            startAllStoppedTracks();
            redrawAllTrackRows();
        } else {
            toggleRecordOnSelected();
            redrawTrackRow(selectedTrack);
        }
    }
}

// RIGHT: mutes/unmutes the selected track (moved here from EDIT's old tap
// meaning). Tap-only, fires immediately on press -- unlike EDIT/NAV above,
// nothing else is attached to a held RIGHT press to disambiguate against.
// Guards against ALT and EDIT the same way handleChannelInHold() and
// handleBarLengthAdjust() require: bare RIGHT means Mute, ALT+RIGHT means
// in-channel adjust, EDIT+RIGHT means bar length instead, and all three
// need to stay mutually exclusive.
void handleMuteInput() {
    if (onBpmRow || Input::isDown(BTN_ALT) || Input::isDown(BTN_EDIT)) return;
    if (Input::justPressed(BTN_RIGHT)) {
        toggleMuteOnSelected();
        redrawTrackRow(selectedTrack);
    }
}

// NAV's "All Stop" fires the instant a continuous hold reaches
// STOP_ALL_HOLD_MS, rather than waiting for release like EDIT's own held
// gesture above -- "press and hold to stop everything" reads more
// immediately than "press, hold, then let go" for a panic-stop action,
// and there's no single-track ambiguity here to justify the release-time
// delay EDIT's version relies on. Also unlike EDIT's, this works from
// the BPM row too: stopping everything doesn't depend on which track is
// selected, so there's no reason to make it a no-op there. A short tap
// (decided on release, same as before) stops just the selected track from
// a track row (toggleStopOnSelected()), or cancels a click-only pre-roll
// from the BPM row if one's running (see the BPM row branch below) -- a
// no-op on the BPM row otherwise, and always a no-op if the hold above
// already fired.
const uint32_t STOP_ALL_HOLD_MS = 1000;

void handleStopInput() {
    static uint32_t pressStartMs = 0;
    static bool firedAllStop = false;
    if (Input::justPressed(BTN_NAV)) {
        pressStartMs = millis();
        firedAllStop = false;
    }
    if (Input::isDown(BTN_NAV) && !firedAllStop && millis() - pressStartMs >= STOP_ALL_HOLD_MS) {
        firedAllStop = true;
        stopAllActiveTracks();
        redrawAllTrackRows();
    }
    if (Input::justReleased(BTN_NAV)) {
        if (firedAllStop) return;
        if (onBpmRow) {
            // No track to stop from here, but a click-only pre-roll (see
            // startMetronomeClickOnly()) is still something worth a plain
            // NAV tap being able to cancel -- the metronome's audibly
            // running with nothing armed/recording/playing yet, so "stop"
            // means stop *that* instead. Clearing the flag alone is enough:
            // updateMasterClock() sees nothing left keeping its clock running
            // and zeroes it on its own very next tick, same as it already
            // does when Metro itself gets turned off mid-pre-roll.
            if (metronomeClickOnlyActive) metronomeClickOnlyActive = false;
            return;
        }
        toggleStopOnSelected();
        redrawTrackRow(selectedTrack);
    }
}

// RIGHT used to toggle Sync/Independent from a track row -- that's now a
// BPM-row field instead (BPM_FOCUS_SYNC, toggled the same on/off-style
// way Metro/Count In are, see handleBpmAdjust()), which is what freed up
// bare RIGHT on a track row for Mute (see handleMuteInput()). LEFT still
// backs out to mode select from a track row (see its own call site in
// updateMainScreen()); on the BPM row, LEFT/RIGHT step focus through BPM
// value -> Time Sig -> Metro -> Count In -> Sync (handleBpmRowFocus()).

// Called whenever manualBpm or the time signature changes: a bar-
// quantized track's length is really "N bars", not a fixed millisecond
// value, so its *effective* length should track the current tempo/meter.
// Freeform tracks (barLength == 0)
// are untouched -- they have no bar identity to rescale by. Rescales
// every recorded event's offset by the same length ratio (not just the
// wrap point), so relative timing within the loop is preserved -- this is
// a genuine playback-speed change, like retuning a tape, not a
// reinterpretation of where the loop happens to cut off.
void rescaleBarQuantizedTracks() {
    bool rescaled[NUM_TRACKS] = {false, false, false, false};
    bool anyRescaled = false;

    for (int i = 0; i < NUM_TRACKS; i++) {
        LoopTrack& t = tracks[i];
        if (t.barLength == 0 || t.lengthMs == 0) continue; // freeform, or still on the open pass

        uint32_t newLengthMs = barsToMs(t.barLength);
        if (newLengthMs == 0 || newLengthMs == t.lengthMs) continue;

        for (int e = 0; e < t.eventCount; e++) {
            t.events[e].offsetMs = (uint32_t)(((uint64_t)t.events[e].offsetMs * newLengthMs) / t.lengthMs);
        }
        t.lengthMs = newLengthMs;
        resyncCursor(t); // scaling preserves sort order, but playCursor's old index no longer matches the new position
        rescaled[i] = true;
        anyRescaled = true;
    }

    if (!anyRescaled) return;

    // Only the affected tracks' displayed length actually changed --
    // their progress bars self-correct on the next ~100ms position tick
    // (positionFrac() reads lengthMs fresh every time), so this only
    // needs to touch the length text.
    Ui::LoopTrackView views[4];
    buildTrackViews(views);
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (rescaled[i]) Ui::updateLooperLength(views, i, !onBpmRow && selectedTrack == i);
    }
}

// ALT held + UP/DOWN on the BPM row toggles whichever of Metro/Count
// In/Sync currently has focus (see handleBpmRowFocus()) -- UP turns it
// on, DOWN off, no hold-repeat needed for an on/off toggle. The BPM
// value itself (BPM_FOCUS_VALUE) uses EDIT instead, not ALT -- see
// handleBpmValueAdjust() -- so this only ever fires for those other
// three fields.
// Shared by the Metro/Count In/Sync branches below: UP sets `*field` true,
// DOWN sets it false (no hold-repeat -- these are on/off, not ranged
// values), and redraws the row when it actually changes.
void setBpmRowBoolField(bool* field) {
    if (Input::justPressed(BTN_UP) && !*field) {
        *field = true;
    } else if (Input::justPressed(BTN_DOWN) && *field) {
        *field = false;
    } else {
        return;
    }
    Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
}

// Forces the header beat-indicator row visible whenever the metronome
// turns on -- if you're turning the click on, you almost certainly want to
// see the visual beat reference too, regardless of whether it had been
// manually hidden (see showBeatIndicator / handleBpmRowEditToggle()'s
// BPM_FOCUS_VALUE branch). One-directional: turning Metro back off does
// NOT hide the indicator, since it's still useful as a static
// time-signature reference on its own. Called right after every place
// metronomeOn can flip on during normal interaction (handleBpmAdjust()'s
// ALT+UP/DOWN and handleBpmRowEditToggle()'s EDIT-tap); a no-op (and no
// redraw) if it's already on or the indicator's already showing.
void showBeatIndicatorIfMetronomeOn() {
    if (!metronomeOn || showBeatIndicator) return;
    showBeatIndicator = true;
    Ui::updateLooperBeatIndicator(true, masterClockCurrentBeat(), timeSigNum);
}

// EDIT toggles whichever of BPM value/Metro/Count In/Sync currently has
// focus on the BPM row: for Metro/Count In/Sync, a quicker, no-ALT-needed
// alternative to ALT+UP/DOWN (see handleBpmAdjust()); for BPM value, the
// header beat-indicator row's own on/off toggle (see showBeatIndicator) --
// that field has no ALT+UP/DOWN meaning of its own on the BPM row for this
// to be an alternative to, so EDIT-tap is its only toggle. No-op off the
// BPM row (EDIT means arm/record there instead, see handleRecordInput())
// or while focus is on Time Sig, which has nothing of its own to toggle.
// Metro's own on/off toggle is decided on EDIT's *release*, not press --
// unlike Sync below, EDIT-held+LEFT/RIGHT on Metro/Count In means
// something else entirely (adjust Metro's volume or Count In's bar count,
// see handleMetronomeVolumeAdjust()/handleCountInBarsAdjust()), so a
// plain immediate-on-press toggle would fire every time regardless of
// whether the user actually meant to hold EDIT for that adjustment
// instead. Same tap-vs-modifier pattern used elsewhere in this app (e.g.
// FilePlayerMode's ALT tap-vs-page-jump modifier): each adjust handler
// sets its own `*EditUsedForValue` flag whenever it actually changes
// something, which suppresses the toggle here.
bool metronomeEditUsedForVolume = false;
bool countInEditUsedForBars = false;
// Same idea, for BPM value's own EDIT-tap-toggles-showBeatIndicator vs.
// EDIT-held+UP/DOWN/LEFT/RIGHT-adjusts-manualBpm ambiguity (see
// handleBpmValueAdjust()).
bool bpmValueEditUsedForAdjust = false;

void handleBpmRowEditToggle() {
    if (!onBpmRow) return;

    if (bpmRowFocus == BPM_FOCUS_VALUE) {
        if (Input::justPressed(BTN_EDIT)) {
            bpmValueEditUsedForAdjust = false;
        } else if (Input::justReleased(BTN_EDIT)) {
            if (!bpmValueEditUsedForAdjust) {
                showBeatIndicator = !showBeatIndicator;
                // Header row, not the BPM row itself -- masterClockCurrentBeat()
                // tracks a running track's beat position regardless of
                // metronomeOn (see updateMasterClock()'s own comment), same
                // "-1 = nothing lit while the clock isn't running" convention
                // updateMetronomeBeatIndicator() uses.
                int beat = showBeatIndicator ? masterClockCurrentBeat() : -1;
                Ui::updateLooperBeatIndicator(showBeatIndicator, beat, timeSigNum);
            }
        }
        return;
    }
    if (bpmRowFocus == BPM_FOCUS_METRONOME) {
        if (Input::justPressed(BTN_EDIT)) {
            metronomeEditUsedForVolume = false;
            // Preview the current volume in place of "Metro" right away,
            // even before LEFT/RIGHT are pressed -- see
            // handleMetronomeVolumeAdjust()'s own comment.
            Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                 true, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
        } else if (Input::justReleased(BTN_EDIT)) {
            if (!metronomeEditUsedForVolume) metronomeOn = !metronomeOn;
            showBeatIndicatorIfMetronomeOn();
            Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                 false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
        }
        return;
    }
    if (bpmRowFocus == BPM_FOCUS_COUNT_IN) {
        if (Input::justPressed(BTN_EDIT)) {
            countInEditUsedForBars = false;
            // Preview the current bar count in place of "Count" right
            // away -- see handleCountInBarsAdjust()'s own comment.
            Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                 false, countInEnabled, countInBars, true, (int)bpmRowFocus, true);
        } else if (Input::justReleased(BTN_EDIT)) {
            if (!countInEditUsedForBars) countInEnabled = !countInEnabled;
            Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                 false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
        }
        return;
    }

    if (!Input::justPressed(BTN_EDIT)) return;
    if (bpmRowFocus == BPM_FOCUS_SYNC) {
        toggleSyncMode(); // also resyncs every running track's cursor against the new epoch
    } else {
        return;
    }
    Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
}

// EDIT held + LEFT/RIGHT, only while Metro has focus: adjusts
// metronomeVolumePercent (step 5, same as SettingsMode's own Metro
// Volume adjustment) instead of the plain on/off toggle EDIT alone means
// here (see handleBpmRowEditToggle()) -- holding EDIT swaps Metro's own
// label for the live percentage the whole time (drawLooperBpmRow()'s
// showMetronomeVolume), so adjusting it shows exactly what's changing.
// Hold-to-accelerate the same way BPM value's LEFT/RIGHT does.
void handleMetronomeVolumeAdjust() {
    if (!onBpmRow || bpmRowFocus != BPM_FOCUS_METRONOME || !Input::isDown(BTN_EDIT)) return;

    const uint32_t NORMAL_INTERVAL_MS = 150;
    const uint32_t FAST_INTERVAL_MS = 50;
    const uint32_t ACCEL_AFTER_MS = 1500;
    uint32_t now = millis();

    static uint32_t rightPressedAtMs = 0, leftPressedAtMs = 0;
    static uint32_t lastRightStep = 0, lastLeftStep = 0;

    if (Input::justPressed(BTN_RIGHT)) rightPressedAtMs = now;
    if (Input::justPressed(BTN_LEFT)) leftPressedAtMs = now;

    bool changed = false;
    if (Input::isDown(BTN_RIGHT)) {
        uint32_t interval = (now - rightPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval) {
            metronomeVolumePercent += 5;
            if (metronomeVolumePercent > 100) metronomeVolumePercent = 100;
            lastRightStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_LEFT)) {
        uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) {
            metronomeVolumePercent -= 5;
            if (metronomeVolumePercent < 0) metronomeVolumePercent = 0;
            lastLeftStep = now;
            changed = true;
        }
    }

    if (changed) {
        metronomeEditUsedForVolume = true;
        Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                             true, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
    }
}

// EDIT held + UP/DOWN/LEFT/RIGHT, only while Count In has focus: adjusts
// countInBars (1-8, same range SettingsMode's own Count In Bars uses)
// instead of the plain on/off toggle EDIT alone means here (see
// handleBpmRowEditToggle()) -- holding EDIT swaps Count In's own label
// for the live bar count the whole time (drawLooperBpmRow()'s
// showCountInBars), so adjusting it shows exactly what's changing.
// Tap-only (no hold-repeat): the range is small enough that hold-repeat
// would blow past the top/bottom before it's useful, same reasoning as
// Time Sig's own preset cycling.
void handleCountInBarsAdjust() {
    if (!onBpmRow || bpmRowFocus != BPM_FOCUS_COUNT_IN || !Input::isDown(BTN_EDIT)) return;

    if ((Input::justPressed(BTN_UP) || Input::justPressed(BTN_RIGHT)) && countInBars < 8) countInBars++;
    else if ((Input::justPressed(BTN_DOWN) || Input::justPressed(BTN_LEFT)) && countInBars > 1) countInBars--;
    else return;

    countInEditUsedForBars = true;
    Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                         false, countInEnabled, countInBars, true, (int)bpmRowFocus, true);
}

// BPM value and Time Sig adjust with EDIT held (handleBpmValueAdjust()/
// handleTimeSigAdjust()); Metro/Count In/Sync still adjust with ALT+UP/
// DOWN held instead, below. EDIT is free to mean this on the BPM row
// since it only means arm/record on a track row (handleRecordInput()
// gates on !onBpmRow), and a bare tap of it already means something else
// again when focus is on Metro/Count In/Sync (handleBpmRowEditToggle()).
void handleBpmAdjust() {
    if (!onBpmRow) return;

    if (bpmRowFocus == BPM_FOCUS_VALUE) {
        handleBpmValueAdjust();
        return;
    }
    if (bpmRowFocus == BPM_FOCUS_TIME_SIG) {
        handleTimeSigAdjust();
        return;
    }

    if (!Input::isDown(BTN_ALT)) return;

    if (bpmRowFocus == BPM_FOCUS_METRONOME) {
        setBpmRowBoolField(&metronomeOn);
        showBeatIndicatorIfMetronomeOn();
        return;
    }
    if (bpmRowFocus == BPM_FOCUS_COUNT_IN) {
        setBpmRowBoolField(&countInEnabled);
        return;
    }
    if (bpmRowFocus == BPM_FOCUS_SYNC) {
        // Not a plain bool field -- toggleSyncMode() also resyncs every
        // running track's cursor against the new epoch, so this goes
        // through it (only when the direction actually implies a change)
        // rather than setBpmRowBoolField()'s direct assignment.
        if (Input::justPressed(BTN_UP) && !syncMode) toggleSyncMode();
        else if (Input::justPressed(BTN_DOWN) && syncMode) toggleSyncMode();
        else return;
        Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
        return;
    }
}

// EDIT held + UP/DOWN/LEFT/RIGHT, only while focus is on the BPM value
// (BPM_FOCUS_VALUE) -- adjusts manualBpm, repeating while held (same
// pattern as tempo/volume hold-adjust in FilePlayerMode). A shared,
// global setting -- deliberately not per-track -- so it lives on its own
// row rather than in each track's channel/state line. Holding either
// button past ACCEL_AFTER_MS speeds up the repeat rate -- otherwise
// nudging the tempo by a lot means holding through many slow individual
// steps. Timed from when *that* button was first pressed, not from the
// accumulated hold-repeat history, so releasing and re-pressing always
// starts back at the slow rate.
// BPM_FOCUS_VALUE: EDIT+UP/DOWN steps by 10, EDIT+LEFT/RIGHT by 1, each
// with its own hold-to-accelerate timing.
void handleBpmValueAdjust() {
    if (!Input::isDown(BTN_EDIT)) return;
    const uint32_t NORMAL_INTERVAL_MS = 120;
    const uint32_t FAST_INTERVAL_MS = 40;
    const uint32_t ACCEL_AFTER_MS = 2000;
    uint32_t now = millis();

    static uint32_t upPressedAtMs = 0, downPressedAtMs = 0;
    static uint32_t rightPressedAtMs = 0, leftPressedAtMs = 0;
    static uint32_t lastUpStep = 0, lastDownStep = 0;
    static uint32_t lastRightStep = 0, lastLeftStep = 0;

    if (Input::justPressed(BTN_UP)) upPressedAtMs = now;
    if (Input::justPressed(BTN_DOWN)) downPressedAtMs = now;
    if (Input::justPressed(BTN_RIGHT)) rightPressedAtMs = now;
    if (Input::justPressed(BTN_LEFT)) leftPressedAtMs = now;

    bool changed = false;
    if (Input::isDown(BTN_UP)) {
        uint32_t interval = (now - upPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_UP) || now - lastUpStep >= interval) {
            manualBpm += 10.0f;
            if (manualBpm > 300.0f) manualBpm = 300.0f;
            lastUpStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_DOWN)) {
        uint32_t interval = (now - downPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_DOWN) || now - lastDownStep >= interval) {
            manualBpm -= 10.0f;
            if (manualBpm < 20.0f) manualBpm = 20.0f;
            lastDownStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_RIGHT)) {
        uint32_t interval = (now - rightPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval) {
            manualBpm += 1.0f;
            if (manualBpm > 300.0f) manualBpm = 300.0f;
            lastRightStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_LEFT)) {
        uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) {
            manualBpm -= 1.0f;
            if (manualBpm < 20.0f) manualBpm = 20.0f;
            lastLeftStep = now;
            changed = true;
        }
    }

    if (changed) {
        bpmValueEditUsedForAdjust = true; // suppresses handleBpmRowEditToggle()'s showBeatIndicator toggle on this press's release
        rescaleBarQuantizedTracks();
        Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false, (int)bpmRowFocus, true); // always on the BPM row itself here
    }
}

// EDIT held + UP/DOWN/LEFT/RIGHT, only while focus is on Time Sig --
// cycles TIME_SIG_PRESETS (UP/RIGHT forward, DOWN/LEFT back -- all four
// do the same thing in pairs, same as BPM value's UP/DOWN-by-10 and
// LEFT/RIGHT-by-1 both move it, just without a coarse/fine distinction
// here since there's no equivalent finer step for a preset list).
// Clamped at either end rather than wrapped -- same convention
// SettingsMode's Loop Length preset cycling uses, and unlike Metro/Count
// In/Sync there's no natural "other end" for a list of time signatures to
// wrap back to. Tap-only (no hold-repeat): there are only
// TIME_SIG_PRESET_COUNT entries, each a deliberate choice, not a
// continuous range like BPM.
void handleTimeSigAdjust() {
    if (!Input::isDown(BTN_EDIT)) return;

    int idx = timeSigPresetIndex();
    if ((Input::justPressed(BTN_UP) || Input::justPressed(BTN_RIGHT)) && idx < TIME_SIG_PRESET_COUNT - 1) idx++;
    else if ((Input::justPressed(BTN_DOWN) || Input::justPressed(BTN_LEFT)) && idx > 0) idx--;
    else return;
    timeSigNum = TIME_SIG_PRESETS[idx].num;
    timeSigDen = TIME_SIG_PRESETS[idx].den;
    // A bar-quantized track's length in ms depends on the time signature
    // (see barsToMs()), same as it depends on BPM -- so a time signature
    // change needs the same rescale a BPM change gets.
    rescaleBarQuantizedTracks();
    Ui::updateLooperBpm(manualBpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false, (int)bpmRowFocus, true);
}

// ALT held + UP/DOWN: adjust the selected track's MIDI OUT channel
// (playback remap), repeating while held (same pattern as tempo/volume
// hold-adjust in FilePlayerMode). UP/DOWN alone move the row cursor
// instead (see handleRowCursor()'s guard) -- ALT is a pure modifier here,
// no standalone tap action. No-op while the BPM row is selected -- there's
// no track to act on. See handleChannelInHold() just below for the IN
// channel's own ALT-held counterpart, on the other pair of directional
// buttons.
void handleChannelOutHold() {
    if (!Input::isDown(BTN_ALT) || onBpmRow) return;

    const uint32_t REPEAT_INTERVAL_MS = 150;
    static uint32_t lastUpStep = 0;
    static uint32_t lastDownStep = 0;
    uint32_t now = millis();

    bool changed = false;
    if (Input::isDown(BTN_UP) &&
        (Input::justPressed(BTN_UP) || now - lastUpStep >= REPEAT_INTERVAL_MS)) {
        adjustChannelOutOnSelected(1);
        lastUpStep = now;
        changed = true;
    }
    if (Input::isDown(BTN_DOWN) &&
        (Input::justPressed(BTN_DOWN) || now - lastDownStep >= REPEAT_INTERVAL_MS)) {
        adjustChannelOutOnSelected(-1);
        lastDownStep = now;
        changed = true;
    }

    if (changed) {
        Ui::LoopTrackView views[4];
        buildTrackViews(views);
        Ui::updateLooperChannel(views, selectedTrack);
    }
}

// ALT held + LEFT/RIGHT: adjust the selected track's MIDI IN channel (the
// record-input filter), repeating while held -- same shape as
// handleChannelOutHold() above, just the other pair of directional
// buttons (ALT+UP/DOWN already means the OUT channel, so ALT alone can't
// disambiguate the two -- LEFT/RIGHT vs UP/DOWN does). No-op while the
// BPM row is selected -- there's no track to act on.
void handleChannelInHold() {
    if (!Input::isDown(BTN_ALT) || onBpmRow) return;

    const uint32_t REPEAT_INTERVAL_MS = 150;
    static uint32_t lastRightStep = 0;
    static uint32_t lastLeftStep = 0;
    uint32_t now = millis();

    bool changed = false;
    if (Input::isDown(BTN_RIGHT) &&
        (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= REPEAT_INTERVAL_MS)) {
        adjustChannelInOnSelected(1);
        lastRightStep = now;
        changed = true;
    }
    if (Input::isDown(BTN_LEFT) &&
        (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= REPEAT_INTERVAL_MS)) {
        adjustChannelInOnSelected(-1);
        lastLeftStep = now;
        changed = true;
    }

    if (changed) {
        Ui::LoopTrackView views[4];
        buildTrackViews(views);
        Ui::updateLooperChannel(views, selectedTrack);
    }
}

// EDIT held + LEFT/RIGHT: cycle the selected track's bar-length preset
// (Freeform, 1/2/4/8/16/32/64/128 bars) -- replaces the old separate
// bar-length picker screen with a direct on-row control. EDIT+UP/DOWN has
// no meaning on a track row (see handleRecordInput()'s comment for why
// that's fine to leave unclaimed), so EDIT alone is enough to disambiguate
// this from the BPM-row EDIT+LEFT/RIGHT meaning (BPM value ±1, see
// handleBpmValueAdjust()) since the two are mutually exclusive via
// onBpmRow. EDIT is otherwise free to mean this on a track row without
// colliding with its own tap-vs-hold meaning there (arm/record vs. "All
// Start", see handleRecordInput()) because editUsedForBarLength suppresses
// that release-time action whenever this fires first. A single tap per
// press (no hold-repeat) since there are only 9 discrete steps and each
// one is a deliberate choice, unlike BPM's continuous-feeling range.
// No-op while the BPM row is selected -- bar length is per-track.
void handleBarLengthAdjust() {
    if (!Input::isDown(BTN_EDIT) || onBpmRow) return;

    bool changed = false;
    if (Input::justPressed(BTN_RIGHT)) {
        cycleBarLengthOnSelected(1);
        changed = true;
    }
    if (Input::justPressed(BTN_LEFT)) {
        cycleBarLengthOnSelected(-1);
        changed = true;
    }

    if (changed) {
        editUsedForBarLength = true;
        Ui::LoopTrackView views[4];
        buildTrackViews(views);
        Ui::updateLooperBarLength(views, selectedTrack);
    }
}

// PLAY alone pauses/resumes the selected track in place (see
// togglePauseOnSelected()) -- arming/recording moved to EDIT (see
// handleRecordInput()), and Mute moved to RIGHT (see handleMuteInput()).
// ENTER is dual-purpose: tapped alone it opens the loop menu (Save /
// Delete Saved Loop); held while PLAY is tapped it erases the selected
// track instead. `enterUsedAsModifier` distinguishes the two on ENTER's
// release -- without it, simply pressing ENTER (on the way to the
// ENTER+PLAY chord) would open the menu immediately, before PLAY is even
// reachable. LEFT alone backs out to mode select -- NAV is dedicated to
// Stop on this screen (see handleStopInput()) rather than doubling as
// "back" the way it does elsewhere in the app. Returns true if this tick
// requested leaving Looper mode entirely (nothing was running, so no
// confirm needed).
bool updateMainScreen() {
    handleRowCursor();
    handleRecordInput();
    handleMuteInput();
    handleStopInput();
    handleChannelOutHold();
    handleChannelInHold();
    handleBarLengthAdjust();
    handleBpmRowFocus();
    handleBpmAdjust();
    handleMetronomeVolumeAdjust();
    handleCountInBarsAdjust();
    handleBpmRowEditToggle();

    // Erase (ENTER+PLAY) and pause-toggle (PLAY) both act on a specific
    // track, so both are no-ops while the BPM row is selected -- there's
    // no track to apply them to. The menu (ENTER tap) stays available
    // regardless: Erase Track N just refers to whichever track was
    // selected last.
    if (Input::isDown(BTN_ENTER)) {
        if (!onBpmRow && Input::justPressed(BTN_PLAY)) {
            eraseSelected();
            enterUsedAsModifier = true;
            redrawTrackRow(selectedTrack);
        }
    } else if (Input::justReleased(BTN_ENTER)) {
        if (!enterUsedAsModifier) {
            snprintf(menuEraseLabel, sizeof(menuEraseLabel), "Erase Track %d", selectedTrack + 1);
            menuLabels[0] = "Save";
            menuLabels[1] = "Load";
            menuLabels[2] = menuEraseLabel;
            menuLabels[3] = "Erase All Tracks";
            menuLabels[4] = "Delete Loop Bundle";
            menuCursor = 0;
            screen = SCREEN_MENU;
            Ui::drawEntryMenu("Loops", menuLabels, MENU_COUNT, menuCursor);
        }
        enterUsedAsModifier = false;
    } else if (Input::justPressed(BTN_PLAY)) {
        if (onBpmRow) {
            // The only way a count-in ever starts -- arming a track (see
            // toggleRecordOnSelected()) no longer triggers one on its
            // own, so several tracks can be armed in any order first and
            // then started together deliberately from here. justPressed
            // fires immediately on press, so this already covers "PLAY
            // pressed" and "PLAY held" alike -- there's nothing further
            // to do once held, unlike EDIT/NAV's tap-vs-hold split above,
            // since a hold here wouldn't mean anything different from a
            // tap.
            //
            // Only when nothing's already running, though (anyTrackAdvancing()
            // -- Recording/Playing/Muted, same "is something actually
            // happening" check updateMasterClock() itself uses): pressing
            // PLAY here while something's already going shouldn't
            // re-trigger a fresh count-in (or click-only pre-roll) over
            // music that's already playing. Any newly-armed track still
            // needs to actually start, though, so it joins in immediately
            // instead -- startArmedTracksTogether() is the same immediate,
            // no-count-in start a MIDI note on an armed track's channel
            // already triggers elsewhere (see handleIncomingMidi()), and
            // is a no-op if nothing's armed.
            //
            // With Count In on, that overlay already gives a "hear the
            // tempo before you play" lead-in; with it off,
            // startMetronomeClickOnly() serves the same purpose more
            // directly -- just start the click (and the session's shared
            // clock/epoch) running right away, with nothing armed yet
            // required. Either way, once a track's later triggered by an
            // incoming note, startArmedTracksTogether() snaps a freeform
            // track's start to the nearest beat this click already
            // established, rather than the raw note-onset instant.
            if (anyTrackAdvancing()) {
                startArmedTracksTogether();
            } else if (metronomeOn && countInEnabled) {
                maybeStartCountIn();
            } else if (metronomeOn) {
                startMetronomeClickOnly();
            }
            // Also doubles as a "play everything" gesture: any track
            // that already has content but is just sitting stopped
            // starts playing immediately, same as EDIT-held "All Start"
            // (see handleRecordInput()) -- this just makes that reachable
            // from PLAY too, without needing to select a track first.
            startAllStoppedTracks();
            redrawAllTrackRows();
        } else {
            togglePauseOnSelected();
            redrawTrackRow(selectedTrack);
        }
    }

    // LEFT backs out to mode select only from a track row with neither
    // ALT nor EDIT held -- on the BPM row it decreases the value instead
    // (handleBpmAdjust()), with ALT held it adjusts the in-channel down
    // instead (handleChannelInHold()), and with EDIT held it cycles bar
    // length down instead (handleBarLengthAdjust()).
    if (!onBpmRow && !Input::isDown(BTN_ALT) && !Input::isDown(BTN_EDIT) && Input::justPressed(BTN_LEFT)) {
        if (anyTrackActive()) {
            // Don't exit yet -- freezing tracks (and thus cutting sound)
            // is a real, if reversible, action, so confirm first rather
            // than surprising whoever's mid-performance.
            screen = SCREEN_EXIT_CONFIRM;
            Ui::drawMessage("Loops are playing", "ENTER exit & stop  NAV cancel");
        } else {
            return true; // nothing running -- exit immediately, no prompt needed
        }
    }
    return false;
}

bool updateExitConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        stopAllActiveTracks();
        return true;
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
    return false;
}

void updateMenu() {
    if (Input::justPressed(BTN_UP)) {
        if (menuCursor > 0) {
            int prev = menuCursor;
            menuCursor--;
            Ui::updateEntryMenuSelection(menuLabels, MENU_COUNT, prev, menuCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (menuCursor < MENU_COUNT - 1) {
            int prev = menuCursor;
            menuCursor++;
            Ui::updateEntryMenuSelection(menuLabels, MENU_COUNT, prev, menuCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (menuCursor == 0) {
            saveMenuCursor = 0;
            screen = SCREEN_SAVE_MENU;
            Ui::drawEntryMenu("Save", saveMenuLabels, SAVE_MENU_COUNT, saveMenuCursor);
        } else if (menuCursor == 1) {
            loadMenuCursor = 0;
            screen = SCREEN_LOAD_MENU;
            Ui::drawEntryMenu("Load", loadMenuLabels, LOAD_MENU_COUNT, loadMenuCursor);
        } else if (menuCursor == 2) {
            screen = SCREEN_ERASE_CONFIRM;
            char msg[24];
            snprintf(msg, sizeof(msg), "Erase Track %d?", selectedTrack + 1);
            Ui::drawMessage(msg, "ENTER confirm  NAV cancel");
        } else if (menuCursor == 3) {
            screen = SCREEN_ERASE_ALL_CONFIRM;
            Ui::drawMessage("Erase all 4 tracks?", "ENTER confirm  NAV cancel");
        } else {
            loadSavedSessions();
            if (savedSessionCount == 0) {
                showFlashMessage("No saved loops", nullptr, 1200);
            } else {
                deleteCursor = 0;
                screen = SCREEN_DELETE_BROWSE;
                const char* labels[MAX_SAVED_SESSIONS];
                for (int i = 0; i < savedSessionCount; i++) labels[i] = savedSessionNames[i];
                Ui::drawEntryMenu("Delete Loop", labels, savedSessionCount, deleteCursor);
            }
        }
    }
}

void updateSaveMenu() {
    if (Input::justPressed(BTN_UP)) {
        if (saveMenuCursor > 0) {
            int prev = saveMenuCursor;
            saveMenuCursor--;
            Ui::updateEntryMenuSelection(saveMenuLabels, SAVE_MENU_COUNT, prev, saveMenuCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (saveMenuCursor < SAVE_MENU_COUNT - 1) {
            int prev = saveMenuCursor;
            saveMenuCursor++;
            Ui::updateEntryMenuSelection(saveMenuLabels, SAVE_MENU_COUNT, prev, saveMenuCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MENU;
        Ui::drawEntryMenu("Loops", menuLabels, MENU_COUNT, menuCursor);
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (saveMenuCursor == 0) {
            beginSaveLoopName();
        } else {
            screen = SCREEN_SAVE_CONFIRM;
            Ui::drawMessage("Save all loops?", "ENTER confirm  NAV cancel");
        }
    }
}

// Moves the on-screen keyboard cursor for SCREEN_SAVE_LOOP_NAME by
// (dRow, dCol) -- same grid-clamping shape as FilePlayerMode's own
// moveKeyboardCursor(), just against this screen's own cursor state (see
// its declaration's comment for why this isn't shared directly).
void moveLoopSaveKeyboardCursor(int dRow, int dCol) {
    int newRow = loopSaveKeyRow + dRow;
    if (newRow < 0) newRow = 0;
    if (newRow >= KEYBOARD_ROW_COUNT) newRow = KEYBOARD_ROW_COUNT - 1;

    int newCol = (dRow != 0) ? loopSaveKeyCol : loopSaveKeyCol + dCol;
    if (newCol < 0) newCol = 0;
    if (newCol >= KEYBOARD_ROW_LENS[newRow]) newCol = KEYBOARD_ROW_LENS[newRow] - 1;

    if (newRow == loopSaveKeyRow && newCol == loopSaveKeyCol) return;

    int prevRow = loopSaveKeyRow, prevCol = loopSaveKeyCol;
    loopSaveKeyRow = newRow;
    loopSaveKeyCol = newCol;
    Ui::updateNameEntryKey(prevRow, prevCol, loopSaveKeyRow, loopSaveKeyCol);
}

// Opens SCREEN_SAVE_LOOP_NAME pre-filled with an auto-generated
// "LoopNNN" name (see generateLoopFileName()) -- good enough to just
// accept most of the time, so the cursor starts right on OK, same
// reasoning as FilePlayerMode's New Recording/Capture SysEx Dump (see its
// beginNameEntry()).
void beginSaveLoopName() {
    char base[LOOP_NAME_MAX_LEN + 1];
    generateLoopFileName(base, sizeof(base));
    strncpy(loopSaveNameBuf, base, sizeof(loopSaveNameBuf) - 1);
    loopSaveNameBuf[sizeof(loopSaveNameBuf) - 1] = '\0';
    loopSaveNameLen = (int)strlen(loopSaveNameBuf);
    loopSaveError[0] = '\0';
    loopSaveKeyRow = KEYBOARD_ROW_COUNT - 1;
    loopSaveKeyCol = KEYBOARD_ROW_LENS[KEYBOARD_ROW_COUNT - 1] - 1; // OK is always the last key of the last row
    screen = SCREEN_SAVE_LOOP_NAME;
    Ui::drawNameEntry("Save Loop", loopSaveNameBuf, ".mid", nullptr, loopSaveKeyRow, loopSaveKeyCol);
}

// Actually writes the file, once any name collision has already been
// resolved (there wasn't one, or the user just confirmed overwriting it
// via SCREEN_SAVE_LOOP_OVERWRITE) -- mirrors saveSession()'s own
// flash-message-then-SCREEN_MAIN ending, same as every other terminal
// action in this mode's menu tree.
void performSaveLoopName(const char* trimmed) {
    bool ok = saveSelectedLoop(trimmed);
    char msg[32];
    if (ok) snprintf(msg, sizeof(msg), "Saved as %s.mid", trimmed);
    else snprintf(msg, sizeof(msg), "Save failed");
    showFlashMessage(msg, nullptr, 1200);
}

// Trims trailing spaces off loopSaveNameBuf, then either saves directly
// or detours through SCREEN_SAVE_LOOP_OVERWRITE first if that name
// already exists -- same collision handling every other save/rename flow
// in this app uses (see Ui::drawConfirmOverwrite()).
void finishSaveLoopName() {
    char trimmed[LOOP_NAME_MAX_LEN + 1];
    strncpy(trimmed, loopSaveNameBuf, sizeof(trimmed));
    trimmed[LOOP_NAME_MAX_LEN] = '\0';
    int len = (int)strlen(trimmed);
    while (len > 0 && trimmed[len - 1] == ' ') trimmed[--len] = '\0';
    if (len == 0) return; // nothing typed -- stay on the naming screen

    char path[96];
    snprintf(path, sizeof(path), "%s/%s.mid", LOOPS_ROOT, trimmed);
    if (sd.exists(path)) {
        strncpy(pendingLoopSaveName, trimmed, sizeof(pendingLoopSaveName) - 1);
        pendingLoopSaveName[sizeof(pendingLoopSaveName) - 1] = '\0';
        screen = SCREEN_SAVE_LOOP_OVERWRITE;
        Ui::drawConfirmOverwrite(pendingLoopSaveName, false);
    } else {
        performSaveLoopName(trimmed);
    }
}

void updateSaveLoopName() {
    if (Input::justPressed(BTN_LEFT))  moveLoopSaveKeyboardCursor(0, -1);
    if (Input::justPressed(BTN_RIGHT)) moveLoopSaveKeyboardCursor(0, 1);
    if (Input::justPressed(BTN_UP))    moveLoopSaveKeyboardCursor(-1, 0);
    if (Input::justPressed(BTN_DOWN))  moveLoopSaveKeyboardCursor(1, 0);

    if (Input::justPressed(BTN_NAV)) {
        screen = SCREEN_SAVE_MENU;
        Ui::drawEntryMenu("Save", saveMenuLabels, SAVE_MENU_COUNT, saveMenuCursor);
        return;
    }

    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        const KeyDef& key = KEYBOARD_ROWS[loopSaveKeyRow][loopSaveKeyCol];
        bool hadError = (loopSaveError[0] != '\0');
        loopSaveError[0] = '\0';
        switch (key.kind) {
            case KEY_CHAR:
                if (loopSaveNameLen < LOOP_NAME_MAX_LEN) {
                    loopSaveNameBuf[loopSaveNameLen++] = key.ch;
                    loopSaveNameBuf[loopSaveNameLen] = '\0';
                    Ui::updateNameEntryPreview(loopSaveNameBuf, ".mid");
                    if (hadError) Ui::updateNameEntryError(nullptr);
                }
                break;
            case KEY_DEL:
                if (loopSaveNameLen > 0) {
                    loopSaveNameBuf[--loopSaveNameLen] = '\0';
                    Ui::updateNameEntryPreview(loopSaveNameBuf, ".mid");
                    if (hadError) Ui::updateNameEntryError(nullptr);
                }
                break;
            case KEY_OK:
                finishSaveLoopName();
                break;
        }
    }
}

void updateSaveLoopOverwrite() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_SAVE_LOOP_NAME;
        Ui::drawNameEntry("Save Loop", loopSaveNameBuf, ".mid", nullptr, loopSaveKeyRow, loopSaveKeyCol);
        return;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        // saveTrackToFile() opens O_TRUNC, so overwriting just means
        // writing again -- no separate remove needed first.
        performSaveLoopName(pendingLoopSaveName);
    }
}

void updateSaveConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        saveSession(); // transitions to SCREEN_FLASH_MESSAGE itself
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_SAVE_MENU;
        Ui::drawEntryMenu("Save", saveMenuLabels, SAVE_MENU_COUNT, saveMenuCursor);
    }
}

// Erases whichever track was selected at the moment the menu was opened
// (see updateMainScreen()) -- the menu-driven path is confirmed, unlike
// the quick ENTER-held+PLAY chord on the main screen, which stays
// immediate for anyone who already knows what they're doing.
void updateEraseConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        eraseSelected();
        char msg[24];
        snprintf(msg, sizeof(msg), "Erased Track %d", selectedTrack + 1);
        showFlashMessage(msg, nullptr, 1000);
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
}

void updateEraseAllConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        eraseAllTracks();
        showFlashMessage("Erased all tracks", nullptr, 1000);
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
}

void updateDeleteBrowse() {
    const char* labels[MAX_SAVED_SESSIONS];
    for (int i = 0; i < savedSessionCount; i++) labels[i] = savedSessionNames[i];

    if (Input::justPressed(BTN_UP)) {
        if (deleteCursor > 0) {
            int prev = deleteCursor;
            deleteCursor--;
            Ui::updateEntryMenuSelection(labels, savedSessionCount, prev, deleteCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (deleteCursor < savedSessionCount - 1) {
            int prev = deleteCursor;
            deleteCursor++;
            Ui::updateEntryMenuSelection(labels, savedSessionCount, prev, deleteCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        screen = SCREEN_DELETE_CONFIRM;
        char msg[32];
        snprintf(msg, sizeof(msg), "Delete %s?", savedSessionNames[deleteCursor]);
        Ui::drawMessage(msg, "ENTER confirm  NAV cancel");
    }
}

void updateDeleteConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        bool ok = deleteSavedSession(savedSessionNames[deleteCursor]);
        char msg[32];
        if (ok) snprintf(msg, sizeof(msg), "Deleted %s", savedSessionNames[deleteCursor]);
        else snprintf(msg, sizeof(msg), "Delete failed: %s", savedSessionNames[deleteCursor]);
        showFlashMessage(msg, nullptr, 1200);
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
}

void updateFlashMessage() {
    if (millis() >= flashUntilMs) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
}

void updateLoadMenu() {
    if (Input::justPressed(BTN_UP)) {
        if (loadMenuCursor > 0) {
            int prev = loadMenuCursor;
            loadMenuCursor--;
            Ui::updateEntryMenuSelection(loadMenuLabels, LOAD_MENU_COUNT, prev, loadMenuCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (loadMenuCursor < LOAD_MENU_COUNT - 1) {
            int prev = loadMenuCursor;
            loadMenuCursor++;
            Ui::updateEntryMenuSelection(loadMenuLabels, LOAD_MENU_COUNT, prev, loadMenuCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MENU;
        Ui::drawEntryMenu("Loops", menuLabels, MENU_COUNT, menuCursor);
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (loadMenuCursor == 0) {
            beginLoadLoopBrowse();
        } else {
            loadSavedSessions();
            if (savedSessionCount == 0) {
                showFlashMessage("No saved loops", nullptr, 1200);
            } else {
                loadSessionCursor = 0;
                screen = SCREEN_LOAD_BROWSE;
                const char* labels[MAX_SAVED_SESSIONS];
                for (int i = 0; i < savedSessionCount; i++) labels[i] = savedSessionNames[i];
                Ui::drawEntryMenu("Load Loop", labels, savedSessionCount, loadSessionCursor);
            }
        }
    }
}

// Clamps loopBrowseScrollOffset so loopBrowseSelectedIndex stays on
// screen -- same math as FilePlayerMode's own ensureSelectionVisible(),
// just against this screen's independent cursor/scroll state.
void ensureLoopBrowseVisible() {
    int rows = Ui::visibleRows();
    if (rows <= 0) return;
    if (loopBrowseSelectedIndex < loopBrowseScrollOffset || loopBrowseSelectedIndex >= loopBrowseScrollOffset + rows) {
        loopBrowseScrollOffset = (loopBrowseSelectedIndex / rows) * rows;
    }
    if (loopBrowseScrollOffset < 0) loopBrowseScrollOffset = 0;
}

// Always re-roots to LOOPS_ROOT and re-reads it, even if the browser was
// left inside a subfolder last time this screen was open -- "Load
// Selected Loop" should always start fresh at the top, same as opening
// any other browser-based menu item.
void beginLoadLoopBrowse() {
    loopBrowser.begin(LOOPS_ROOT);
    loopBrowseSelectedIndex = 0;
    loopBrowseScrollOffset = 0;
    screen = SCREEN_LOAD_LOOP_BROWSE;
    Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
}

// ENTER tap: descend into a folder, or import the selected .mid into
// whichever track was selected when "Load Selected Loop" was opened --
// same importFileIntoTrack() OMNI-in/this-slot-out/Freeform defaults
// requestImport() already uses for an arbitrary external file, since an
// individual loop save carries no metadata of its own either (see
// saveSelectedLoop()).
void selectHighlightedLoopEntry() {
    if (loopBrowseSelectedIndex < 0 || loopBrowseSelectedIndex >= loopBrowser.entryCount()) return;
    const BrowserEntry& e = loopBrowser.entry(loopBrowseSelectedIndex);
    if (e.isDir) {
        loopBrowser.enterDir(loopBrowseSelectedIndex);
        loopBrowseSelectedIndex = 0;
        loopBrowseScrollOffset = 0;
        Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
        return;
    }
    if (e.kind != FILE_MID) return; // nothing this screen can do with a non-.mid file
    char path[224];
    loopBrowser.buildFullPath(loopBrowseSelectedIndex, path, sizeof(path));
    bool ok = importFileIntoTrack(tracks[selectedTrack], path, selectedTrack);
    char msg[40];
    if (ok) snprintf(msg, sizeof(msg), "Loaded into Track %d", selectedTrack + 1);
    else snprintf(msg, sizeof(msg), "Load failed");
    // Leaving the screen for good (flash message, then SCREEN_MAIN) --
    // drop loopBrowser's buffers now rather than leaving them resident
    // for the rest of the looping session, same reasoning as the
    // NAV/LEFT-at-root exit below.
    loopBrowser.freeBuffers();
    showFlashMessage(msg, nullptr, 1000);
}

// ENTER held: opens the delete confirm for the selected .mid -- a no-op
// (returns false) for a folder, since this screen never offers deleting
// a whole folder, only individual loop files.
bool beginDeleteHighlightedLoop() {
    if (loopBrowseSelectedIndex < 0 || loopBrowseSelectedIndex >= loopBrowser.entryCount()) return false;
    const BrowserEntry& e = loopBrowser.entry(loopBrowseSelectedIndex);
    if (e.isDir) return false;
    loopDeleteFailed = false;
    screen = SCREEN_LOAD_LOOP_DELETE_CONFIRM;
    Ui::drawConfirmDelete(e.name, false, false);
    return true;
}

// Tap-vs-hold on ENTER, same press-start/threshold/"used" pattern as
// handleStopInput()/handleRecordInput() elsewhere in this file: a tap
// selects/enters (selectHighlightedLoopEntry()), a hold past
// HOLD_THRESHOLD_MS opens the delete confirm instead
// (beginDeleteHighlightedLoop()) -- gated so both never fire off one
// press. PLAY has no hold gesture here, so it always just taps.
void updateLoadLoopBrowse() {
    static uint32_t enterPressStartMs = 0;
    static bool enterUsedForDelete = false;

    if (Input::justPressed(BTN_UP)) {
        if (loopBrowseSelectedIndex > 0) {
            int prev = loopBrowseSelectedIndex;
            loopBrowseSelectedIndex--;
            int prevScroll = loopBrowseScrollOffset;
            ensureLoopBrowseVisible();
            if (loopBrowseScrollOffset == prevScroll) {
                Ui::updateBrowserSelection(loopBrowser, prev, loopBrowseSelectedIndex, loopBrowseScrollOffset);
            } else {
                Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
            }
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (loopBrowseSelectedIndex < loopBrowser.entryCount() - 1) {
            int prev = loopBrowseSelectedIndex;
            loopBrowseSelectedIndex++;
            int prevScroll = loopBrowseScrollOffset;
            ensureLoopBrowseVisible();
            if (loopBrowseScrollOffset == prevScroll) {
                Ui::updateBrowserSelection(loopBrowser, prev, loopBrowseSelectedIndex, loopBrowseScrollOffset);
            } else {
                Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
            }
        }
    }

    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        // SdBrowser::goUp() doesn't know about a custom root -- left
        // unchecked it would happily walk up past LOOPS_ROOT to "/".
        // Exit the screen instead once already sitting at LOOPS_ROOT
        // itself; only delegate to goUp() from somewhere further down.
        if (strcmp(loopBrowser.currentPath(), LOOPS_ROOT) == 0) {
            // Leaving the screen -- drop the buffers rather than holding
            // them resident for the rest of the looping session (this
            // mode's tracks[] is already 128KB; no reason to also carry a
            // large folder's index around while just recording/playing).
            // Cheap to rebuild: beginLoadLoopBrowse() always refreshes
            // unconditionally next time this screen opens.
            loopBrowser.freeBuffers();
            screen = SCREEN_LOAD_MENU;
            Ui::drawEntryMenu("Load", loadMenuLabels, LOAD_MENU_COUNT, loadMenuCursor);
        } else {
            loopBrowser.goUp();
            loopBrowseSelectedIndex = 0;
            loopBrowseScrollOffset = 0;
            Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
        }
        return;
    }

    if (Input::justPressed(BTN_ENTER)) {
        enterPressStartMs = millis();
        enterUsedForDelete = false;
    }
    if (Input::isDown(BTN_ENTER) && !enterUsedForDelete &&
        millis() - enterPressStartMs >= HOLD_THRESHOLD_MS) {
        if (beginDeleteHighlightedLoop()) enterUsedForDelete = true;
    }
    if (Input::justReleased(BTN_ENTER)) {
        if (!enterUsedForDelete) selectHighlightedLoopEntry();
        enterUsedForDelete = false;
    }
    if (Input::justPressed(BTN_PLAY)) {
        selectHighlightedLoopEntry();
    }
}

void updateLoadLoopDeleteConfirm() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_LOAD_LOOP_BROWSE;
        Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
        return;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (loopDeleteFailed) {
            screen = SCREEN_LOAD_LOOP_BROWSE;
            Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
            return;
        }

        char path[224];
        loopBrowser.buildFullPath(loopBrowseSelectedIndex, path, sizeof(path));
        bool ok = sd.remove(path);
        if (ok) {
            loopBrowser.refresh();
            int count = loopBrowser.entryCount();
            if (loopBrowseSelectedIndex >= count) loopBrowseSelectedIndex = count > 0 ? count - 1 : 0;
            if (loopBrowseSelectedIndex < 0) loopBrowseSelectedIndex = 0;
            ensureLoopBrowseVisible();
            screen = SCREEN_LOAD_LOOP_BROWSE;
            Ui::drawLoopBrowser(loopBrowser, loopBrowseSelectedIndex, loopBrowseScrollOffset);
        } else {
            loopDeleteFailed = true;
            const BrowserEntry& e = loopBrowser.entry(loopBrowseSelectedIndex);
            Ui::drawConfirmDelete(e.name, false, true);
        }
    }
}

void updateLoadBrowse() {
    const char* labels[MAX_SAVED_SESSIONS];
    for (int i = 0; i < savedSessionCount; i++) labels[i] = savedSessionNames[i];

    if (Input::justPressed(BTN_UP)) {
        if (loadSessionCursor > 0) {
            int prev = loadSessionCursor;
            loadSessionCursor--;
            Ui::updateEntryMenuSelection(labels, savedSessionCount, prev, loadSessionCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (loadSessionCursor < savedSessionCount - 1) {
            int prev = loadSessionCursor;
            loadSessionCursor++;
            Ui::updateEntryMenuSelection(labels, savedSessionCount, prev, loadSessionCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_LOAD_MENU;
        Ui::drawEntryMenu("Load", loadMenuLabels, LOAD_MENU_COUNT, loadMenuCursor);
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        strncpy(loadSessionName, savedSessionNames[loadSessionCursor], sizeof(loadSessionName) - 1);
        loadSessionName[sizeof(loadSessionName) - 1] = '\0';
        buildLoadPickList(loadSessionName);
        loadPickCursor = 0;
        screen = SCREEN_LOAD_PICK;
        Ui::drawEntryMenu(loadSessionName, loadPickLabels, loadPickLabelCount, loadPickCursor);
    }
}

void updateLoadPick() {
    if (Input::justPressed(BTN_UP)) {
        if (loadPickCursor > 0) {
            int prev = loadPickCursor;
            loadPickCursor--;
            Ui::updateEntryMenuSelection(loadPickLabels, loadPickLabelCount, prev, loadPickCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (loadPickCursor < loadPickLabelCount - 1) {
            int prev = loadPickCursor;
            loadPickCursor++;
            Ui::updateEntryMenuSelection(loadPickLabels, loadPickLabelCount, prev, loadPickCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        screen = SCREEN_LOAD_CONFIRM;
        char msg[40];
        if (loadPickCursor == 0) {
            if (anyTrackHasContent()) {
                snprintf(msg, sizeof(msg), "This erases all 4 tracks!");
            } else {
                snprintf(msg, sizeof(msg), "Load all from %s?", loadSessionName);
            }
        } else {
            if (trackHasContent(tracks[selectedTrack])) {
                snprintf(msg, sizeof(msg), "Track %d has a loop -- erase it?", selectedTrack + 1);
            } else {
                snprintf(msg, sizeof(msg), "Load Track %d into Track %d?",
                         loadPickTrackNums[loadPickCursor - 1], selectedTrack + 1);
            }
        }
        Ui::drawMessage(msg, "ENTER confirm  NAV cancel");
    }
}

void updateLoadConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (loadPickCursor == 0) loadEntireSession(loadSessionName);
        else loadSingleTrackIntoSelected(loadSessionName, loadPickTrackNums[loadPickCursor - 1]);
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
}

void updateImportConfirm() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        bool ok = importFileIntoTrack(tracks[pendingImportTrack], pendingImportPath, pendingImportTrack);
        char msg[32];
        if (ok) snprintf(msg, sizeof(msg), "Imported to Track %d", pendingImportTrack + 1);
        else snprintf(msg, sizeof(msg), "Import failed");
        showFlashMessage(msg, nullptr, 1200);
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_MAIN;
        needsRedraw = true;
    }
}

void buildTrackViews(Ui::LoopTrackView out[4]) {
    uint32_t now = millis();
    for (int i = 0; i < NUM_TRACKS; i++) {
        const LoopTrack& t = tracks[i];
        out[i].channelIn = t.channelIn;
        out[i].channelOut = t.channelOut;
        out[i].barLength = t.barLength;
        out[i].lengthMs = t.lengthMs;
        out[i].positionFrac = positionFrac(t);
        out[i].recordActive = t.lastRecordActivityMs != 0 && (now - t.lastRecordActivityMs) < ACTIVITY_FLASH_MS;
        out[i].playActive = t.lastPlaybackActivityMs != 0 && (now - t.lastPlaybackActivityMs) < ACTIVITY_FLASH_MS;
        out[i].bufferFull = t.bufferFull;
        switch (t.state) {
            case TRACK_ARMED:     out[i].state = Ui::LOOP_TRACK_ARMED; break;
            case TRACK_RECORDING: out[i].state = Ui::LOOP_TRACK_RECORDING; break;
            case TRACK_PLAYING:   out[i].state = Ui::LOOP_TRACK_PLAYING; break;
            case TRACK_MUTED:     out[i].state = Ui::LOOP_TRACK_MUTED; break;
            case TRACK_STOPPED:   out[i].state = Ui::LOOP_TRACK_STOPPED; break;
            case TRACK_PAUSED:    out[i].state = Ui::LOOP_TRACK_PAUSED; break;
            default:              out[i].state = Ui::LOOP_TRACK_EMPTY; break;
        }
    }
}

// Applies SettingsMode's current defaults as this mode's own live state
// -- called from enter() (switching into the mode; tracks is guaranteed
// allocated by the time this runs, see enter()'s own comment), but only
// while nothing's actually going on yet (!anyTrackHasContent()), so this
// is how a Settings change becomes visible without a reboot (just leave
// Settings and re-enter the looper) while never silently overwriting a
// session already in progress just because the user happened to also
// tweak an unrelated default. Per-track barLength is the one exception
// worth calling out: it's normally sticky per track (the user's own
// EDIT+LEFT/RIGHT choice survives erase/re-record), but with no track
// holding content yet there's nothing of the user's to protect, so
// resetting all 4 here is safe.
void applySettingsIfFresh() {
    if (anyTrackHasContent()) return;
    manualBpm = SettingsMode::defaultBpm();
    timeSigNum = SettingsMode::defaultTimeSigNum();
    timeSigDen = SettingsMode::defaultTimeSigDen();
    syncMode = SettingsMode::defaultSyncMode();
    metronomeOn = SettingsMode::defaultMetronomeOn();
    // No redraw call needed here (unlike showBeatIndicatorIfMetronomeOn()'s
    // other two call sites) -- enter() already set needsRedraw before
    // calling this, so the imminent full Ui::drawLooper() picks this up.
    if (metronomeOn) showBeatIndicator = true;
    countInEnabled = SettingsMode::defaultCountInEnabled();
    countInBars = SettingsMode::defaultCountInBars();
    metronomeVolumePercent = SettingsMode::metronomeVolume();
    for (int i = 0; i < NUM_TRACKS; i++) tracks[i].barLength = SettingsMode::defaultBarLength();
}

// -- Free-tracks-for-File-Player gate -------------------------------------
//
// Deliberately independent of `screen`/update()'s own state machine --
// see updateFreeTracksForFilePlayer()'s header comment in looper_mode.h
// for why: it runs while FilePlayerMode is active and about to open a
// .mod/.s3m file specifically (see its APP_FREE_LOOPER_RAM), not while
// MODE_LOOPER is the active top-level mode.
const char* const FREE_GATE_LABELS[] = { "Save & Continue", "Discard & Continue", "Cancel" };
const int FREE_GATE_COUNT = 3;
int freeGateCursor = 0;

} // namespace

bool hasUnsavedTrackContent() {
    return tracks != nullptr && anyTrackHasContent();
}

void freeTracksIfAllocated() {
    delete[] tracks;
    tracks = nullptr;
}

void beginFreeTracksForFilePlayer() {
    freeGateCursor = 0;
    Ui::drawEntryMenu("Free RAM for File Player", FREE_GATE_LABELS, FREE_GATE_COUNT, freeGateCursor);
}

FreeTracksResult updateFreeTracksForFilePlayer() {
    if (Input::justPressed(BTN_UP) && freeGateCursor > 0) {
        int prev = freeGateCursor--;
        Ui::updateEntryMenuSelection(FREE_GATE_LABELS, FREE_GATE_COUNT, prev, freeGateCursor);
    }
    if (Input::justPressed(BTN_DOWN) && freeGateCursor < FREE_GATE_COUNT - 1) {
        int prev = freeGateCursor++;
        Ui::updateEntryMenuSelection(FREE_GATE_LABELS, FREE_GATE_COUNT, prev, freeGateCursor);
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        return FREE_TRACKS_CANCELLED; // tracks left exactly as it was
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (freeGateCursor == 0) saveSession(); // "Save & Continue" -- reuses existing SD-write machinery unmodified
        freeTracksIfAllocated();                // "Discard & Continue": nothing to do first -- just free
        return FREE_TRACKS_DONE;
    }
    return FREE_TRACKS_PENDING;
}

void begin() {
    // /loops is created lazily on first Save -- nothing to pre-create for
    // that. tracks[] itself is allocated lazily too (see enter()), so
    // there's genuinely nothing to do here at startup -- deliberately NOT
    // calling applySettingsIfFresh() here anymore, since it touches
    // tracks[i] and tracks is still null at this point (begin() runs once
    // at firmware boot, before the user has necessarily even chosen to
    // enter Looper mode). enter()'s own call covers this instead, always
    // after the allocation below has already run.
}

void enter() {
    tracksAllocFailed = false;
    if (!tracks) {
        // Drop loopBrowser's own heap buffers (if it's holding any --
        // e.g. left over from a "Load Selected Loop" visit before tracks[]
        // was last freed for FilePlayerMode's benefit) before the big
        // allocation below -- pure disposable scan data, cheaply rebuilt
        // next time that screen opens (beginLoadLoopBrowse() always
        // refreshes unconditionally), so no confirmation needed. main.cpp
        // does the equivalent for FilePlayerMode's own browser right
        // before calling into here -- see FilePlayerMode::
        // freeBrowserBuffers()'s comment.
        loopBrowser.freeBuffers();
        tracks = new (std::nothrow) LoopTrack[NUM_TRACKS];
        if (!tracks) {
            // Extremely unlikely given the RAM budget this was sized
            // against, but fail safe rather than let anything below
            // dereference a null tracks -- bounce back to Mode Select
            // instead of pretending the looper is usable.
            tracksAllocFailed = true;
            showFlashMessage("Not enough RAM", "for looper", 1500);
            return;
        }
        // channelOut can't default per-track via a LoopTrack member
        // initializer (those can't see this track's own index) -- set it
        // here instead, once, right after a fresh allocation: track N
        // starts out routed to output channel N, so out of the box each
        // track can already drive a different channel/device without the
        // user dialing in anything. Only runs on first allocation, same
        // "once" scope channelIn's own OMNI default effectively has.
        for (int i = 0; i < NUM_TRACKS; i++) tracks[i].channelOut = (uint8_t)i;
    }
    MidiOutput::setInputHandler(handleIncomingMidi);
    MidiOutput::setRealtimeHandler(handleMidiRealtime);
    // Always land on the main screen with a track (not the BPM row or a
    // leftover confirm/menu screen) selected -- same "reset to home on
    // entry" convention FilePlayerMode uses for its own screen state.
    screen = SCREEN_MAIN;
    onBpmRow = false;
    needsRedraw = true;
    // Picks up any Settings change made since the last enter() -- see
    // applySettingsIfFresh()'s comment. Safe now that tracks is
    // guaranteed non-null above.
    applySettingsIfFresh();
}

bool update() {
    if (tracksAllocFailed) {
        // enter() already drew the message via showFlashMessage(); just
        // let it sit on screen for its duration, then bail out to Mode
        // Select -- don't fall through to anything below, all of which
        // assumes tracks != nullptr.
        return millis() >= flashUntilMs;
    }
    updatePlayback(); // always runs, regardless of which screen is up, so loops never stall for a menu/prompt
    updateBarQuantizedRecording();
    updateMasterClock(); // same reasoning -- shouldn't stall for a menu/prompt either
    updateCountIn();
    updatePendingResumes(); // same reasoning -- a Sync-mode Pause resume shouldn't stall for a menu/prompt either

    bool exitRequested = false;
    switch (screen) {
        case SCREEN_MAIN:           exitRequested = updateMainScreen(); break;
        case SCREEN_EXIT_CONFIRM:   exitRequested = updateExitConfirm(); break;
        case SCREEN_MENU:           updateMenu(); break;
        case SCREEN_SAVE_MENU:      updateSaveMenu(); break;
        case SCREEN_SAVE_LOOP_NAME: updateSaveLoopName(); break;
        case SCREEN_SAVE_LOOP_OVERWRITE: updateSaveLoopOverwrite(); break;
        case SCREEN_SAVE_CONFIRM:   updateSaveConfirm(); break;
        case SCREEN_ERASE_CONFIRM:  updateEraseConfirm(); break;
        case SCREEN_ERASE_ALL_CONFIRM: updateEraseAllConfirm(); break;
        case SCREEN_DELETE_BROWSE:  updateDeleteBrowse(); break;
        case SCREEN_DELETE_CONFIRM: updateDeleteConfirm(); break;
        case SCREEN_LOAD_MENU:      updateLoadMenu(); break;
        case SCREEN_LOAD_LOOP_BROWSE: updateLoadLoopBrowse(); break;
        case SCREEN_LOAD_LOOP_DELETE_CONFIRM: updateLoadLoopDeleteConfirm(); break;
        case SCREEN_LOAD_BROWSE:    updateLoadBrowse(); break;
        case SCREEN_LOAD_PICK:      updateLoadPick(); break;
        case SCREEN_LOAD_CONFIRM:   updateLoadConfirm(); break;
        case SCREEN_IMPORT_CONFIRM: updateImportConfirm(); break;
        case SCREEN_FLASH_MESSAGE:  updateFlashMessage(); break;
    }
    if (exitRequested) return true;

    // Every sub-screen draws itself at the moment it's entered (see each
    // update*() above); only SCREEN_MAIN has its own periodic redraw here.
    if (screen != SCREEN_MAIN) return false;

    // While counting in, the count-in overlay replaces the normal 4-track
    // view entirely (see maybeStartCountIn()/updateCountIn()) -- the
    // overlay's own per-beat updates happen directly from updateCountIn()
    // above, not through the needsRedraw-gated position tick below.
    if (countInActive) {
        if (needsRedraw) {
            needsRedraw = false;
            Ui::drawLooperCountIn(countInBars * timeSigNum - countInBeatShown);
        }
        return false;
    }

    updateMetronomeBeatIndicator(); // every tick, cheap comparison -- see its own comment
    updateBpmRowBeatFlash(); // every tick, same reasoning -- see its own comment

    Ui::LoopTrackView views[4];
    buildTrackViews(views);

    if (needsRedraw) {
        needsRedraw = false;
        // -1 ("nothing lit") when the row is hidden, not just whenever
        // metronomeOn is false -- masterClockCurrentBeat() tracks a running
        // track's beat position regardless of metronomeOn (see
        // updateMasterClock()'s own comment), so showBeatIndicator alone
        // decides whether to show it here.
        int currentBeat = showBeatIndicator ? masterClockCurrentBeat() : -1;
        Ui::drawLooper(views, selectedTrack, onBpmRow, syncMode, manualBpm, timeSigNum, timeSigDen,
                       metronomeOn, metronomeVolumePercent, false, countInEnabled, countInBars, false,
                       (int)bpmRowFocus, showBeatIndicator, currentBeat);
    } else {
        static uint32_t lastPosTick = 0;
        uint32_t now = millis();
        if (now - lastPosTick >= 100) {
            lastPosTick = now;
            // -1 when the BPM row is selected, so no track row is
            // (incorrectly) treated as highlighted for the activity dots'
            // "off" background color -- drawLooper()/updateLooperSelection()
            // already cleared that track's real highlight when the cursor
            // moved off it.
            Ui::updateLooperPositions(views, onBpmRow ? -1 : selectedTrack);
        }
    }

    return false;
}

void requestImport(const char* path, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= NUM_TRACKS) return;
    strncpy(pendingImportPath, path, sizeof(pendingImportPath) - 1);
    pendingImportPath[sizeof(pendingImportPath) - 1] = '\0';
    pendingImportTrack = trackIndex;

    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char msg[48];
    if (trackHasContent(tracks[trackIndex])) {
        snprintf(msg, sizeof(msg), "Track %d has a loop -- erase it?", trackIndex + 1);
    } else {
        snprintf(msg, sizeof(msg), "Import %.24s into Track %d?", base, trackIndex + 1);
    }
    screen = SCREEN_IMPORT_CONFIRM;
    Ui::drawMessage(msg, "ENTER confirm  NAV cancel");
}

} // namespace LooperMode

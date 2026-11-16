#pragma once
#include <Arduino.h>
#include "sd_browser.h"
#include "midi_file.h"
#include "stream_player.h"
#include "tracker_base.h"
#include "midi_output.h"
#include "midi_recorder.h"
#include "sysex_recorder.h"
#include "sysex_player.h"
#include "ui_modes.h"
#include "play_mode.h"

namespace Ui {

void begin();

// Sets the display backlight brightness via PWM on DISPLAY_BACKLIGHT (see
// pins.h) -- 0-100 percent, clamped. begin() calls this once at full
// brightness (before settings have loaded); SettingsMode::begin() then
// applies the saved value immediately afterward, and again live on every
// adjustment -- no "session freshness" concept here, same as MIDI Thru.
void setBacklightBrightness(int percent);

// Boot animation (UiSaver's Pattern decode). setup() starts it, calls
// bootAnimStep() between its load steps and bootAnimFinish() before the
// first other screen. A key press skips to the final frame; that press is
// then ignored until it's let go.
void bootAnimStart();

// Draws a frame if one is due. Returns true while the animation runs.
bool bootAnimStep();

// Runs the rest of the animation, to about 1 s from bootAnimStart().
void bootAnimFinish();

// Full redraw of the top-level mode-select screen (the boot screen):
// "midiTracker" header over a vertical list of mode names, `cursor`
// highlighted.
void drawModeSelect(const char* const* labels, int count, int cursor);

// Cheap partial redraw for moving the mode-select highlight: repaints
// only the previously and newly selected rows instead of the whole screen.
void updateModeSelectCursor(const char* const* labels, int count, int prevCursor, int newCursor);

// Full redraw of the file browser: current path, scrolling list with
// `selectedIndex` highlighted starting at `scrollOffset`.
void drawBrowser(const SdBrowser& browser, int selectedIndex, int scrollOffset);

// Cheap partial redraw for moving the highlight within the same visible
// page (no scroll, no header/footer change): repaints only the previously
// and newly selected rows instead of the whole screen.
void updateBrowserSelection(const SdBrowser& browser, int prevIndex, int newIndex, int scrollOffset);

// Redraws the browser's status strip, for when the MIDI output target it
// shows changes.
void updateBrowserHeader(const SdBrowser& browser);

// Full redraw of LooperMode's restricted "Load Selected Loop" browser --
// same header/row/scrollbar layout as drawBrowser(), just its own footer
// hint text (tap ENTER to select, hold to delete, no EDIT menu/ALT output
// here since this view offers neither). updateBrowserSelection() above is
// reused as-is for the cheap partial redraw -- it never touches the
// footer, so it works for either screen unmodified.
void drawLoopBrowser(const SdBrowser& browser, int selectedIndex, int scrollOffset);

// The .mid player screen (drawn by ui_player.cpp): time, tempo, output and
// synth, channel meters and a keybed. `audioOn` is whether the onboard
// synth is on, `volume` its level 0-100. `stopped` shows Stop instead of
// Pause while the player is in STATE_PAUSED after NAV.
void drawPlayer(const char* filename, const MidiPlayer& player, MidiOutTarget target,
                 bool audioOn, int volume, bool stopped);

// Time, tempo, channel meters and keybed, each only when it changed. Call
// as often as every loop while playing, and once after a seek or tempo
// change while paused. Not while STATE_ERROR.
void updatePlayerLive(const MidiPlayer& player);

// The volume readout on whichever player screen is up.
void updateVolume(int volume);

// The play mode tag on the player screens, for their next draw.
void setPlayMode(PlayMode mode);
// The same, drawn now. Only while a player screen is up.
void updatePlayMode(PlayMode mode);

// The play state in the status strip, after PLAY or NAV. Not across a
// change into or out of STATE_ERROR, which needs drawPlayer().
void updatePlayerState(MidiPlayer::State state, bool stopped);

// The output line, after ALT changes the target or ALT+EDIT the synth.
void updatePlayerOutputTarget(MidiOutTarget target);
void updatePlayerAudioState(bool audioOn);

// -- pariSynth live play screen (ui_modes.cpp) ----------------------------
// 16 channel cells, 2 columns of 8: number, family icon, instrument and a
// meter, over a keybed. `activeMask` has a bit per channel with notes
// down, which lights that cell's number and icon. Both PariSynthMode and
// FilePlayerMode's ALT+EDIT overlay show it; the footer shows
// PariSynthGrid's keys.
void drawPariSynthPlay(int selectedChannel, uint16_t activeMask = 0);

// Cell meters and the keybed, at their own 40 ms rate. Call every loop.
void updatePariSynthPlayLive();

// Bit per channel: the active state each cell was last drawn with.
uint16_t pariSynthDrawnActiveMask();

// One cell, after a program change or a held-note change.
void updatePariSynthChannelCell(int channel, bool selected, bool active);

// Moves the cursor: repaints the two cells.
void updatePariSynthSelection(int prevChannel, int newChannel, bool prevActive, bool newActive);

// VOL over channel 9's cell while EDIT+UP/DOWN changes the volume. The
// caller takes it away with updatePariSynthChannelCell(8, ...), and puts it
// back after a full drawPariSynthPlay() while it should still show.
void updatePariSynthVolumeOverlay(int volume);

// The WAV and MP3 player screen (ui_player.cpp): time and length, a
// waveform that fills in as it plays, and peak meters. `stopped` as
// drawPlayer().
void drawWavPlayer(const char* filename, const StreamPlayer& player, int volume, bool stopped);

// The play state, same rules as updatePlayerState().
void updateWavPlayerState(StreamPlayer::State state, bool stopped);

// Time, waveform and peak meters. Call as often as every loop while
// playing, and once after a seek or stop.
void updateWavPlayerLive(const StreamPlayer& player);

// The tracker player screen (MOD, S3M, XM, IT; ui_player.cpp): time,
// scope, speed and tempo, channel lanes with a row trail and meters, and
// the order position.
void drawTrackerPlayer(const char* filename, const TrackerPlayer& player, int volume, bool stopped);

// The play state, same rules as updatePlayerState().
void updateTrackerPlayerState(TrackerPlayer::State state, bool stopped);

// Time, position, lanes, meters and scope. Call as often as every loop
// while playing.
void updateTrackerPlayerLive(const TrackerPlayer& player);

// After a tracker song or WAV ends, keep the live updates going until this
// is true: the ring has played out and the meters are down.
bool playerLiveSettled();

// Full redraw of the on-screen QWERTY name entry screen shared by New
// Recording, New Folder, and Rename. `title` labels what's being named,
// `name` is the text typed so far (append/backspace-at-the-end, no mid-
// string cursor), `suffix` is appended after it in the preview (".mid"
// for recordings, "" for folders), `error`, if non-null, replaces the
// second footer hint line, and `keyRow`/`keyCol` highlight the currently
// selected key on the on-screen keyboard grid (see keyboard_layout.h).
void drawNameEntry(const char* title, const char* name, const char* suffix,
                    const char* error, int keyRow, int keyCol);

// Cheap partial redraw for moving the on-screen keyboard highlight:
// repaints only the previously and newly selected keys.
void updateNameEntryKey(int prevRow, int prevCol, int newRow, int newCol);

// Cheap partial redraw of just the text preview line, e.g. after a
// character is typed or deleted. Does not touch the keyboard grid.
void updateNameEntryPreview(const char* name, const char* suffix);

// Cheap partial redraw of just the footer's error/hint line (e.g. to
// clear a stale error after the user starts editing again, or to show
// one after a failed submit). Pass nullptr to restore the hint text.
void updateNameEntryError(const char* error);

// Full redraw of the per-entry action menu (Open/Rename/Delete/New
// Recording/New Folder, contextually filtered). `subtitle` names the
// entry the menu was opened on (or a generic label if none is selected).
// `scrollOffset` (0 by default) scrolls the list the same way
// drawBrowser()/drawSettings() do, drawing a scrollbar (see visibleRows())
// once `count` exceeds one screenful -- callers whose list always fits
// one screen (e.g. the fixed 3-item Save/Load/Reset menus) can omit it;
// callers with a growing list (a folder scan, a picker) should track
// their own scrollOffset the same way SettingsMode's ensureThemeVisible()/
// moveThemeCursor() already do for the Theme editor's own scrollable list,
// and pass it through here.
void drawEntryMenu(const char* subtitle, const char* const* labels, int count, int cursor, int scrollOffset = 0);

// Cheap partial redraw for moving the entry-menu highlight within the
// current scroll position: repaints only the previously and newly
// selected rows. Caller is responsible for a full drawEntryMenu() instead
// whenever the move also changes scrollOffset.
void updateEntryMenuSelection(const char* const* labels, int count, int prevCursor, int newCursor, int scrollOffset = 0);

// Convert's target list: the strip names the file, then a row per target
// (up to 4) with its title and note, as the Settings page list draws them.
void drawConvertTargets(const char* name, const char* const* titles, const char* const* notes, int count, int cursor);
void updateConvertTargetsCursor(const char* const* titles, const char* const* notes, int count, int prevCursor, int newCursor);

// Full redraw of the delete confirmation screen. `failed`, once set,
// switches the screen from "Delete X?" to an error message.
void drawConfirmDelete(const char* name, bool isDir, bool failed);

// Full redraw of the overwrite confirmation screen -- same shape as
// drawConfirmDelete() (title/question/warning/footer), shown when saving a
// new recording/SysEx capture or renaming something would collide with an
// existing file/folder of the same name (see FilePlayerMode's
// finishNameEntry()/handleConfirmOverwriteInput()). Unlike
// drawConfirmDelete(), there's no `failed` state here -- a failure to
// actually remove the old file on confirm just drops back to the name
// entry screen with an inline error instead (same as any other name-entry
// failure), rather than needing its own display here.
void drawConfirmOverwrite(const char* name, bool isDir);

// One entry in the live MIDI-in log shown on the Recording screen, between
// the Events row and the footer (see updateRecordingLog()). A SysEx entry
// only carries its byte length -- the raw bytes aren't kept around for
// display, just a count.
struct MidiLogEntry {
    bool isSysEx;
    uint8_t status; // channel-voice entries only
    uint8_t data1;
    uint8_t data2;
    uint8_t len;       // 1 or 2, channel-voice entries only
    uint16_t sysExLen; // SysEx entries only
};

// The recording screen (waiting, recording or error), see ui_rec.h.
// `log`/`logCount` are the newest MIDI-in events, oldest first.
void drawRecording(const char* filename, const MidiRecorder& recorder,
                    const MidiLogEntry* log, int logCount);

// Elapsed time, event count and the activity dot, only what changed. Call
// at a fixed rate while STATE_RECORDING.
void updateRecordingLive(const MidiRecorder& recorder);

// Repaints the log rows whose text changed. Call when something new has
// arrived, on the recording or capture screen.
void updateRecordingLog(const MidiLogEntry* entries, int count);

// The SysEx capture screen: time, messages, bytes and dropped count, with
// the same log as the recording screen.
void drawSysExCapture(const char* filename, const SysExRecorder& recorder,
                       const MidiLogEntry* log, int logCount);

// Time, counts and the activity dot. Call at a fixed rate while
// STATE_RECORDING.
void updateSysExCaptureLive(const SysExRecorder& recorder);

// Discard confirm over the recording or capture screen.
void drawConfirmDiscard(bool sysex, const char* filename);

// The .syx send screen: time, messages, a byte progress bar whose head
// lights while bytes go out, and skipped and junk counts when not 0.
void drawSysExPlayer(const char* filename, const SysExPlayer& player);

// Time, counts and the progress bar. Call at a fixed rate while
// STATE_SENDING.
void updateSysExPlayerLive(const SysExPlayer& player);

// Centered one/two line status/error message (e.g. "SD card not found").
void drawMessage(const char* line1, const char* line2 = nullptr);

// How many browser rows fit on screen -- used by main.cpp to page/scroll.
int visibleRows();

// -- Looper screen (ui_modes.cpp) -----------------------------------------
// The BPM block (big BPM, time signature, METRO, COUNT, SYNC, bar and
// beat) over 4 track rows (state chip, route, bar length, a lane with bar
// ticks, event dots and a playhead, and the length).
using UiModes::LoopTrackState;
using UiModes::LoopTrackView;
using UiModes::LOOP_TRACK_EMPTY;
using UiModes::LOOP_TRACK_ARMED;
using UiModes::LOOP_TRACK_RECORDING;
using UiModes::LOOP_TRACK_PLAYING;
using UiModes::LOOP_TRACK_MUTED;
using UiModes::LOOP_TRACK_STOPPED;
using UiModes::LOOP_TRACK_PAUSED;

// Full redraw. `onBpmRow` puts the cursor on the BPM block instead of
// `selectedTrack`; `focusIndex` is the focused field there (0 BPM, 1 time
// sig, 2 Metro, 3 Count In, 4 Sync). `showMetronomeVolume`/
// `showCountInBars` show the volume or bars in place of METRO/COUNT while
// EDIT is held. `showBeatIndicator` shows the beat squares in the status
// strip, `metronomeCurrentBeat` (0-based, -1 none) lights one.
void drawLooper(const LoopTrackView tracks[4], int selectedTrack, bool onBpmRow, bool syncMode, float bpm,
                 int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                 bool showMetronomeVolume, bool countInEnabled, int countInBars, bool showCountInBars,
                 int focusIndex, bool showBeatIndicator, int metronomeCurrentBeat);

// Playheads, fills, event dots and activity lights, only what moved. Call
// at the 40 ms frame rate, which the looper keeps. `selectedTrack` is -1 on
// the BPM row.
void updateLooperPositions(const LoopTrackView tracks[4], int selectedTrack);

// Moves the cursor between track rows and the BPM row.
void updateLooperSelection(const LoopTrackView tracks[4], int prevSelected, bool prevOnBpm,
                            int newSelected, bool newOnBpm, bool syncMode, float bpm,
                            int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                            bool showMetronomeVolume, bool countInEnabled, int countInBars,
                            bool showCountInBars, int focusIndex);

// Track trackIdx's whole row, after its state changes.
void updateLooperTrackRow(const LoopTrackView tracks[4], int trackIdx, bool selected);

// The selected track's route, after ALT+UP/DOWN/LEFT/RIGHT.
void updateLooperChannel(const LoopTrackView tracks[4], int selectedTrack);

// The selected track's bar length, after EDIT+LEFT/RIGHT.
void updateLooperBarLength(const LoopTrackView tracks[4], int selectedTrack);

// Track trackIdx's length, after a BPM change rescales it.
void updateLooperLength(const LoopTrackView tracks[4], int trackIdx, bool selected);

// The BPM block, after one of its fields changes. `selected` is whether
// the cursor is on it.
void updateLooperBpm(float bpm, int timeSigNum, int timeSigDen, bool syncMode, bool metronomeOn,
                      int metronomeVolumePercent, bool showMetronomeVolume, bool countInEnabled,
                      int countInBars, bool showCountInBars, int focusIndex, bool selected);

// The beat flash round the BPM block: the downbeat colour on beat 1, the
// pulse colour on the others; `active` false takes it away.
void updateLooperBpmRowBeatFlash(bool active, bool downbeat, bool selected);

// Bar and beat in the BPM block, 1-based; bar 0 shows dashes.
void updateLooperBarBeat(int bar, int beat);

// The count-in screen, in place of the tracks, counting down to 1.
void drawLooperCountIn(int beatsRemaining);
void updateLooperCountInBeat(int beatsRemaining);

// The beat squares in the status strip, one per beat of the time
// signature. `currentBeat` is 0-based or -1 for none.
void updateLooperBeatIndicator(bool visible, int currentBeat, int timeSigNum);

// Settings' first view: its pages as a list in the home screen's style,
// `labels` and `notes` `count` long (up to 4), `cursor` selected.
void drawSettingsPageList(const char* const* labels, const char* const* notes, int count, int cursor);

// Repaints only the two rows the page list cursor moved between.
void updateSettingsPageListCursor(const char* const* labels, const char* const* notes, int count, int prevCursor, int newCursor);

// Full redraw of one Settings page: a scrollable list of labelled
// values, `cursor` highlighted, `scrollOffset` rows scrolled down
// (0 = top). `labels`/`values` are parallel arrays, `count` long.
// Draws a scrollbar (see visibleRows()) when `count` exceeds one
// screenful, same convention as drawBrowser(). `pageTitle` is shown in
// the header after "Settings".
// `footerLine1`/`footerLine2` default to the Settings page hints.
void drawSettings(const char* const* labels, const char* const* values, int count, int cursor,
                   int scrollOffset, const char* pageTitle,
                   const char* footerLine1 = "UD move  LEFT back",
                   const char* footerLine2 = "EDIT+LR change");

// Cheap partial redraw for moving the settings cursor within the current
// scroll position: repaints only the previously and newly selected rows.
// Caller is responsible for a full drawSettings() instead whenever the
// move also changes scrollOffset (see SettingsMode::ensureSettingsVisible()).
void updateSettingsSelection(const char* const* labels, const char* const* values, int count,
                              int prevCursor, int newCursor, int scrollOffset);

// Cheap partial redraw of just one row's value, e.g. after EDIT+LEFT/
// RIGHT adjusts it.
void updateSettingsValue(const char* label, const char* value, int index, int scrollOffset);

// -- Theme editor (opened via ENTER on its row in Settings' MIDI/System
// page -- user-editable color palette) ---------------------------------
// Every color the rest of the UI draws with is exposed here by index
// (0..themeColorCount()-1), independent of the generic label/value Settings
// list -- each item needs 3 co-editable numbers (R/G/B), not the one value
// every generic Settings row has, so this gets its own small API and its
// own page-drawing functions rather than being forced through
// drawSettings(). SettingsMode owns the actual file I/O (save/load/reset
// menu, .thm parsing) and just calls through this to read/write values.

int themeColorCount();
const char* themeColorLabel(int index); // short row label, e.g. "Background", "Recording"
void getThemeColorRGB(int index, uint8_t& r, uint8_t& g, uint8_t& b);
void setThemeColorRGB(int index, uint8_t r, uint8_t g, uint8_t b);
void resetThemeColorsToDefault(); // every color back to its hardcoded shipped value
// Around loading a .thm: Begin resets to the defaults. End with ok fills
// entries the file didn't have from the ones an older theme used for those
// parts; without ok it puts back the colours from before Begin.
void themeLoadBegin();
void themeLoadEnd(bool ok);

// Full redraw of the Theme editor: one row per themeColorCount() color
// (label, R/G/B fields, a small solid swatch previewing the result),
// `colorCursor` highlighted, `channelCursor` (0=R,1=G,2=B) drawn inverted
// within that row to show which one EDIT+UP/DOWN/LEFT/RIGHT currently
// adjusts (see SettingsMode). Scrolls the same way drawSettings() does
// when the list exceeds one screenful.
void drawThemePage(int colorCursor, int channelCursor, int scrollOffset);

// Cheap partial redraw of a single Theme row, e.g. after EDIT+UP/DOWN/
// LEFT/RIGHT changes the focused channel's value, or bare LEFT/RIGHT moves
// channelCursor to a different field within the same row.
void updateThemeRow(int index, int channelCursor, bool selected, int scrollOffset);

// Cheap partial redraw for moving the Theme page's row cursor: repaints
// only the previously and newly selected rows, same shape as
// updateSettingsSelection(). Caller is responsible for a full
// drawThemePage() instead whenever the move also changes scrollOffset.
void updateThemeSelection(int prevCursor, int newCursor, int channelCursor, int scrollOffset);

// The battery and the MIDI in/out LEDs in the status strip, which every
// screen draws. Both only redraw their own pixels when what they show
// changes. main.cpp calls updateBatteryMeter() every loop; it also lights
// the LEDs on MIDI activity. drawBatteryMeter() refreshes just the battery.
void drawBatteryMeter();
void updateBatteryMeter();

} // namespace Ui

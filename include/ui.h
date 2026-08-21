#pragma once
#include <Arduino.h>
#include "sd_browser.h"
#include "midi_file.h"
#include "wav_file.h"
#include "mod_file.h"
#include "s3m_file.h"
#include "xm_file.h"
#include "midi_output.h"
#include "midi_recorder.h"
#include "sysex_recorder.h"
#include "sysex_player.h"

namespace Ui {

void begin();

// Boot splash: "midiTracker" / "track your MIDI", centered. Just draws --
// caller (main.cpp) is responsible for holding it on screen (e.g. delay(2000)).
void drawSplash();

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

// Full redraw of the now-playing screen. `audioOn` is whether the onboard
// synth is currently enabled (see MidiOutput::setAudioOutput()); `volume`
// is its master output level, 0-100 (see Synth::setVolume()) -- both shown
// regardless of whether audio is actually on, same as the tempo/output
// rows are always shown regardless of playback state. `stopped` shows
// "Stopped" instead of "Paused" while State is STATE_PAUSED -- see
// updatePlayerState()'s comment on why that distinction lives outside
// MidiPlayer::State itself.
void drawPlayer(const char* filename, const MidiPlayer& player, MidiOutTarget target,
                 bool audioOn, int volume, bool stopped);

// Cheap partial redraw of the now-playing screen's elapsed-time and tempo
// rows (file tempo can change mid-playback via tempo meta events,
// independent of the user's speed adjustment), plus the live note-
// activity strip. Meant to be called at a low fixed rate while
// STATE_PLAYING, but also safe as a one-off right after NAV resets
// playback to the start (Time/Tempo both changed then too) -- the only
// hazard is STATE_ERROR, whose completely different layout doesn't have
// these rows at all; never call this then. Does not touch the header/
// filename/state/output/volume rows drawn by drawPlayer().
void updatePlayerLive(const MidiPlayer& player);

// Cheap partial redraw of just the volume row, e.g. after the user
// adjusts it via ENTER+UP/DOWN on the now-playing screen. Does not touch
// any other row.
void updateVolume(int volume);

// Cheap partial redraw of just the State value, e.g. after PLAY toggles
// pause/resume or NAV stops. `stopped` shows "Stopped" instead of
// "Paused" while state is STATE_PAUSED -- MidiPlayer itself has no
// separate state for "stopped" (NAV resets to the start and pauses there,
// same STATE_PAUSED a plain PLAY-pause leaves it in), so FilePlayerMode
// tracks which one happened and passes it through here. Not valid across
// a transition into or out of STATE_ERROR -- that swaps the whole screen
// layout, so use drawPlayer() there instead.
void updatePlayerState(MidiPlayer::State state, bool stopped);

// Cheap partial redraw of just the MIDI-target half of its row, e.g.
// after ALT cycles hardware/USB/both. Does not touch the Audio field.
void updatePlayerOutputTarget(MidiOutTarget target);

// Cheap partial redraw of just the Audio on/off half of its row, e.g.
// after EDIT toggles the onboard synth. Does not touch the MIDI field.
void updatePlayerAudioState(bool audioOn);

// Full redraw of the WAV "Now Playing" screen -- Filename/State/Time
// (total known up front, unlike MidiPlayer)/Volume/format info (sample
// rate, bit depth, channel count), no Tempo/Tracks/MIDI-target rows since
// none apply to WAV playback. `stopped` is the same NAV-vs-PLAY-pause
// distinction drawPlayer() uses, see updatePlayerState()'s comment.
void drawWavPlayer(const char* filename, const WavPlayer& player, int volume, bool stopped);

// Cheap partial redraw of just the State value -- same convention as
// updatePlayerState(), including the same STATE_ERROR caveat: a
// transition into or out of STATE_ERROR swaps the whole screen layout
// (see drawWavPlayer()), so use that instead in that case.
void updateWavPlayerState(WavPlayer::State state, bool stopped);

// Cheap partial redraw of the WAV screen's elapsed-time row. Meant to be
// called at a low fixed rate while STATE_PLAYING, and as a one-off after
// a seek/NAV-stop (Time changed then too). Does not touch Filename/State/
// Volume/format rows.
void updateWavPlayerLive(const WavPlayer& player);

// Full redraw of the MOD "Now Playing" screen -- Filename/State/Time
// (elapsed only, no fixed total -- a tracker's song can loop
// indefinitely, see ModPlayer's header comment)/Volume/`Pattern: N/M
// Row: R` position/format info (channel and instrument counts). No
// progress bar (no fixed total to show progress against), no seek
// controls (ModPlayer doesn't support seeking).
void drawModPlayer(const char* filename, const ModPlayer& player, int volume, bool stopped);

// Cheap partial redraw of just the State value -- same convention as
// updateWavPlayerState()/updatePlayerState().
void updateModPlayerState(ModPlayer::State state, bool stopped);

// Cheap partial redraw of the MOD screen's elapsed-time and pattern-
// position rows. Meant to be called at a low fixed rate while
// STATE_PLAYING. Does not touch Filename/State/Volume/format rows.
void updateModPlayerLive(const ModPlayer& player);

// Full redraw of the S3M "Now Playing" screen -- same layout as
// drawModPlayer() (Filename/State/Time elapsed-only/Volume/`Pattern: N/M
// Row: R`/format info), adapted for S3M's own channel/instrument counts.
void drawS3mPlayer(const char* filename, const S3mPlayer& player, int volume, bool stopped);

// Cheap partial redraw of just the State value -- same convention as
// updateModPlayerState().
void updateS3mPlayerState(S3mPlayer::State state, bool stopped);

// Cheap partial redraw of the S3M screen's elapsed-time and pattern-
// position rows -- same convention as updateModPlayerLive().
void updateS3mPlayerLive(const S3mPlayer& player);

// Full redraw of the XM "Now Playing" screen -- same layout as
// drawS3mPlayer() (Filename/State/Time elapsed-only/Volume/`Pattern: N/M
// Row: R`/format info), adapted for XM's own channel/instrument counts.
void drawXmPlayer(const char* filename, const XmPlayer& player, int volume, bool stopped);

// Cheap partial redraw of just the State value -- same convention as
// updateS3mPlayerState().
void updateXmPlayerState(XmPlayer::State state, bool stopped);

// Cheap partial redraw of the XM screen's elapsed-time and pattern-
// position rows -- same convention as updateS3mPlayerLive().
void updateXmPlayerLive(const XmPlayer& player);

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
void drawEntryMenu(const char* subtitle, const char* const* labels, int count, int cursor);

// Cheap partial redraw for moving the entry-menu highlight: repaints only
// the previously and newly selected rows instead of the whole screen.
void updateEntryMenuSelection(const char* const* labels, int count, int prevCursor, int newCursor);

// Full redraw of the delete confirmation screen. `failed`, once set,
// switches the screen from "Delete X?" to an error message.
void drawConfirmDelete(const char* name, bool isDir, bool failed);

// Full redraw of the recording status screen (waiting for input / actively
// recording / error).
void drawRecording(const char* filename, const MidiRecorder& recorder);

// Cheap partial redraw of the recording screen: updates only the elapsed-
// time and event-count rows. Meant to be called at a low fixed rate while
// STATE_RECORDING; does not touch the header/filename/state rows drawn by
// drawRecording().
void updateRecordingLive(const MidiRecorder& recorder);

// Full redraw of the SysEx capture screen -- deliberately the same shape
// as drawRecording() (filename/state/elapsed/message-count rows), plus a
// receive-activity dot next to the message count: lit while a message
// arrived within the last moment, dim once it's been quiet a while, so
// it's obvious at a glance when the dump has actually stopped arriving
// and it's time to stop & save rather than having to watch the count.
void drawSysExCapture(const char* filename, const SysExRecorder& recorder);

// Cheap partial redraw of the SysEx capture screen: elapsed time, message
// count, and the activity dot. Meant to be called at a fairly fast fixed
// rate (faster than updateRecordingLive()'s -- the activity dot's whole
// point is tracking short-lived blips) while STATE_RECORDING.
void updateSysExCaptureLive(const SysExRecorder& recorder);

// Full redraw of the .syx playback screen. Reuses the same filename/
// state/elapsed/message-count rows as drawSysExCapture() for a consistent
// look, but replaces drawPlayer()'s note strip -- meaningless here, a raw
// SysEx dump has no notes -- with a byte-progress bar (meaningful because,
// unlike a live performance, the total size is known upfront) whose fill
// color briefly brightens each time a message is actually sent, so it
// reads as "actively transmitting" rather than a static progress meter.
void drawSysExPlayer(const char* filename, const SysExPlayer& player);

// Cheap partial redraw of the .syx playback screen: elapsed time, message
// count, and the progress bar (including its send-activity flash). Meant
// to be called at a fairly fast fixed rate while STATE_SENDING.
void updateSysExPlayerLive(const SysExPlayer& player);

// Centered one/two line status/error message (e.g. "SD card not found").
void drawMessage(const char* line1, const char* line2 = nullptr);

// How many browser rows fit on screen -- used by main.cpp to page/scroll.
int visibleRows();

// Looper mode: one track's display state for the always-visible 4-track
// screen. A plain snapshot struct (like MidiPlayer/MidiRecorder feed the
// player/recording screens) rather than exposing LooperMode's internal
// track representation to ui.cpp.
enum LoopTrackState { LOOP_TRACK_EMPTY, LOOP_TRACK_ARMED, LOOP_TRACK_RECORDING, LOOP_TRACK_PLAYING, LOOP_TRACK_MUTED, LOOP_TRACK_STOPPED, LOOP_TRACK_PAUSED };

struct LoopTrackView {
    uint8_t channel = 0;     // 0-15 (displayed as 1-16), or 16 for OMNI
    LoopTrackState state = LOOP_TRACK_EMPTY;
    int barLength = 0;       // 0 = Freeform, else the number of bars this track's length is quantized to
    uint32_t lengthMs = 0;   // 0 if empty, armed, or still on an open-ended first recording pass
    float positionFrac = 0;  // 0.0-1.0 position within the loop, for the progress bar; meaningless if lengthMs==0
    // Two separate indicators, not one: a track mid-overdub is
    // simultaneously capturing new events (recordActive) and playing back
    // its existing content (playActive), so one "active" flag couldn't
    // represent both at once.
    bool recordActive = false; // true briefly whenever this track actually captures an incoming MIDI event
    bool playActive = false;   // true briefly whenever this track actually sends an event out during playback
    bool bufferFull = false;   // this track's event buffer is full; further recorded events are being dropped
};

// Full redraw of the looper's main screen: all 4 tracks' state, plus a
// 5th BPM row (see updateLooperBpm()) -- `onBpmRow` says whether the
// cursor is on that row rather than `selectedTrack`. BPM/Time Signature/
// Sync/Metronome/Count In are shared, global settings (not per-track),
// which is why they live here as one shared row instead of being
// duplicated per track. `metronomeOn`/`countInEnabled` are shown as
// Metro/Count In between the time signature and Sync text on that same
// row (their own text color is the on/off indicator). `showMetronomeVolume`/
// `showCountInBars` briefly replace Metro's/Count In's own label with
// `metronomeVolumePercent`/`countInBars` while EDIT is held with that
// field focused (see LooperMode's handleMetronomeVolumeAdjust()/
// handleCountInBarsAdjust()) -- meaningless (and always false) unless
// focusIndex is 2 or 3 respectively and onBpmRow is true. `focusIndex`
// says which of the row's five fields ALT+UP/DOWN/LEFT/RIGHT (or EDIT,
// for BPM value/Time Sig) currently targets -- 0 = BPM value, 1 = Time
// Sig, 2 = Metro, 3 = Count In, 4 = Sync (see LooperMode's
// handleBpmRowFocus()) -- meaningless unless onBpmRow is also true.
// `metronomeCurrentBeat` (0-based, 0 = downbeat) drives the header's beat-
// indicator squares (see updateLooperBeatIndicator()) -- meaningless
// (ignored) unless `metronomeOn` is also true.
void drawLooper(const LoopTrackView tracks[4], int selectedTrack, bool onBpmRow, bool syncMode, float bpm,
                 int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                 bool showMetronomeVolume, bool countInEnabled, int countInBars, bool showCountInBars,
                 int focusIndex, int metronomeCurrentBeat);

// Cheap partial redraw of just the 4 tracks' progress bars and MIDI
// activity indicators, meant to be called at a low fixed rate to animate
// them without redrawing the rest of the screen (track labels/state/
// cursor don't change here). `selectedTrack` is needed so the activity
// dot's "off" color matches that row's current background.
void updateLooperPositions(const LoopTrackView tracks[4], int selectedTrack);

// Cheap partial redraw for moving the looper's row cursor: repaints only
// the previously and newly selected rows instead of the whole screen.
// Covers both track rows and the BPM row -- prevOnBpm/newOnBpm say
// whether that endpoint is the BPM row rather than a track index.
void updateLooperSelection(const LoopTrackView tracks[4], int prevSelected, bool prevOnBpm,
                            int newSelected, bool newOnBpm, bool syncMode, float bpm,
                            int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                            bool showMetronomeVolume, bool countInEnabled, int countInBars,
                            bool showCountInBars, int focusIndex);

// Cheap partial redraw of trackIdx's whole row (label text, progress bar,
// activity dots) -- e.g. after a button press changes that track's state,
// without touching any other row, the BPM row, header, or footer.
void updateLooperTrackRow(const LoopTrackView tracks[4], int trackIdx, bool selected);

// Cheap partial redraw of just the selected track's channel field, e.g.
// after ALT+UP/DOWN changes it. Does not touch the rest of that row or
// any other row.
void updateLooperChannel(const LoopTrackView tracks[4], int selectedTrack);

// Cheap partial redraw of just the selected track's bar-length field, e.g.
// after ALT+LEFT/RIGHT changes it. Does not touch the rest of that row or
// any other row.
void updateLooperBarLength(const LoopTrackView tracks[4], int selectedTrack);

// Cheap partial redraw of just trackIdx's length field, e.g. after a BPM
// change rescales a bar-quantized track. Unlike the channel/bar-length
// updates above, this isn't limited to the selected track -- a BPM change
// can rescale several tracks at once -- so `selected` is passed explicitly.
void updateLooperLength(const LoopTrackView tracks[4], int trackIdx, bool selected);

// Cheap partial redraw of just the BPM row -- e.g. after ALT+UP/DOWN/
// LEFT/RIGHT (or EDIT) adjusts whichever of BPM/Time Sig/Metro/Count
// In/Sync has focus (see drawLooper()'s comment on `focusIndex`/
// `showMetronomeVolume`/`showCountInBars`). `selected` false means the
// row isn't actually highlighted right now (focusIndex is meaningless
// then); callers pass it false for symmetry, never true off the BPM row.
// Does not touch any track row.
void updateLooperBpm(float bpm, int timeSigNum, int timeSigDen, bool syncMode, bool metronomeOn,
                      int metronomeVolumePercent, bool showMetronomeVolume, bool countInEnabled,
                      int countInBars, bool showCountInBars, int focusIndex, bool selected);

// Full redraw of the looper's count-in overlay, shown in place of the
// normal 4-track view while counting in before recording starts (see
// LooperMode's maybeStartCountIn()). `beatsRemaining` counts down to 1
// (e.g. 4, 3, 2, 1), not up -- recording starts right after 1.
void drawLooperCountIn(int beatsRemaining);

// Cheap partial redraw of just the beat number/caption, called once per
// beat during the count-in. Does not touch the header/footer.
void updateLooperCountInBeat(int beatsRemaining);

// Cheap partial redraw of just the header's metronome beat-indicator row --
// a square per beat of the current time signature, between "MIDI Looper"
// and "midiTracker". `visible` false (Metro off) hides the whole row.
// `currentBeat` is 0-based (0 = downbeat), meaningless unless `visible`.
void updateLooperBeatIndicator(bool visible, int currentBeat, int timeSigNum);

// Full redraw of the Settings screen: a scrollable list of labeled
// default values, `cursor` highlighted, `scrollOffset` rows scrolled down
// (0 = top). UP/DOWN moves the cursor; EDIT+LEFT/RIGHT adjusts the
// highlighted item's value (see SettingsMode). `labels`/`values` are
// parallel arrays, `count` long. Draws a scrollbar (see visibleRows())
// when `count` exceeds one screenful, same convention as drawBrowser().
void drawSettings(const char* const* labels, const char* const* values, int count, int cursor,
                   int scrollOffset);

// Cheap partial redraw for moving the settings cursor within the current
// scroll position: repaints only the previously and newly selected rows.
// Caller is responsible for a full drawSettings() instead whenever the
// move also changes scrollOffset (see SettingsMode::ensureSettingsVisible()).
void updateSettingsSelection(const char* const* labels, const char* const* values, int count,
                              int prevCursor, int newCursor, int scrollOffset);

// Cheap partial redraw of just one row's value, e.g. after EDIT+LEFT/
// RIGHT adjusts it.
void updateSettingsValue(const char* label, const char* value, int index, int scrollOffset);

} // namespace Ui

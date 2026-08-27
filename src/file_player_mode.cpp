#include "file_player_mode.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <new> // std::nothrow, see player/modPlayer/s3mPlayer's declarations below

#include "sd_card.h"
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
#include "looper_mode.h" // hasUnsavedTrackContent()/freeTracksIfAllocated()/the free-RAM gate -- see APP_FREE_LOOPER_RAM
#include "shared_ram_arena.h" // ModPlayer/S3mPlayer/XmPlayer placement-new into the same block as LooperMode::tracks[] -- see that header
#include "settings_mode.h" // defaultVolume() -- see enter()'s volumeSeededFromSettings comment
#include "synth.h"
#include "synth_editor.h"
#include "pari_synth_grid.h"
#include "input.h"
#include "ui.h"
#include "keyboard_layout.h"

namespace FilePlayerMode {
namespace {

enum AppState {
    APP_BROWSE,
    APP_PLAY,
    APP_PLAY_SYSEX,   // sending a .syx dump back out (see openSelected())
    APP_PLAY_WAV,     // playing back a .wav file (see openSelected())
    APP_PLAY_MOD,     // playing back a .mod file (see openSelected())
    APP_PLAY_S3M,     // playing back a .s3m file (see openSelected())
    APP_PLAY_XM,      // playing back a .xm file (see openSelected())
    APP_ENTRY_MENU,   // Open/Rename/Delete/Load to Looper/New Recording/Capture SysEx Dump/New Folder
    APP_NAME_ENTRY,   // shared on-screen-keyboard editor for the actions above
    APP_CONFIRM_OVERWRITE, // "overwrite X?" -- the name just entered collides with an existing file/folder, see finishNameEntry()
    APP_CONFIRM_DELETE,
    APP_RECORDING,
    APP_CONFIRM_CANCEL_RECORDING, // "cancel recording? unsaved progress will be lost"
    APP_CAPTURING_SYSEX,          // waiting for/capturing an incoming .syx dump
    APP_CONFIRM_CANCEL_SYSEX_CAPTURE, // same as APP_CONFIRM_CANCEL_RECORDING, for a SysEx capture
    APP_LOOPER_TRACK_PICK, // "Load to Looper": pick which of the 4 tracks to load the selected .mid into
    // pariSynth quick-edit overlay, opened via EDIT (tap) from APP_PLAY
    // (only -- .wav/.mod/.s3m/.xm playback never drives
    // Synth::noteOn/programChange at all, so editing presets while one of
    // those plays would have no audible effect, not worth wiring a
    // shortcut for). Two sub-screens, tracked by synthEditShowingGrid:
    // first the same 16-channel grid PariSynthMode's own play screen shows
    // (Ui::drawPariSynthPlay(), reused as-is), then -- once a channel's
    // picked -- SynthEditor's field editor (see synth_editor.h) for that
    // channel's instrument/drum. player->update() keeps being ticked
    // unconditionally while either sub-screen is active (see update()'s
    // top, before the input-handling switch) so the file keeps playing in
    // the background the whole time -- see handlePlayInput()'s ALT+EDIT
    // check and this state's own case in update().
    APP_SYNTH_EDIT,
    // Shown instead of actually opening a .mod/.s3m/.xm file (see openSelected())
    // when LooperMode::tracks[] is holding unsaved loop content that needs
    // freeing first -- browsing, MIDI preview, and WAV playback never
    // trigger this, only the big tracker-format players do (see
    // openSelected()'s FILE_MOD/FILE_S3M/FILE_XM branches). The actual menu is
    // drawn by LooperMode::beginFreeTracksForFilePlayer(), not here --
    // this state only needs to forward input to
    // LooperMode::updateFreeTracksForFilePlayer() and act on the result;
    // see handleFreeLooperRamGateInput().
    APP_FREE_LOOPER_RAM,
};

enum NamePurpose { NAME_NEW_RECORDING, NAME_NEW_SYSEX_CAPTURE, NAME_NEW_FOLDER, NAME_RENAME };

enum MenuAction { MENU_OPEN, MENU_RENAME, MENU_DELETE, MENU_LOAD_TO_LOOPER, MENU_NEW_RECORDING,
                   MENU_CAPTURE_SYSEX, MENU_NEW_FOLDER };
struct MenuEntry { MenuAction action; const char* label; };

const char* const LOOPER_TRACK_LABELS[4] = { "Track 1", "Track 2", "Track 3", "Track 4" };

const int NAME_ENTRY_MAX_LEN = 16;

SdBrowser browser;
// player/modPlayer/s3mPlayer/xmPlayer are the four big format players
// (MidiPlayer 8KB, ModPlayer 11KB, S3mPlayer 25.5KB, XmPlayer ~64KB) --
// allocated on demand in openSelected() when a file of that type is
// actually opened, freed on the existing "LEFT -> back to browser"
// handlers each already has (see
// handlePlayInput()/handlePlayModInput()/handlePlayS3mInput()/
// handlePlayXmInput()). Mirrors LooperMode::tracks' same on-demand
// pattern (see looper_mode.h) -- applied symmetrically here rather than
// as a special case for any one format. WavPlayer/SysExPlayer/
// MidiRecorder/SysExRecorder stay plain value objects -- each only a few
// hundred bytes, not worth this.
//
// modPlayer/s3mPlayer/xmPlayer specifically placement-new into
// SharedRamArena (the same fixed block LooperMode::tracks[] uses) rather
// than the general heap -- see that header's comment. player (MidiPlayer)
// stays on the plain heap: unlike the other three, opening a MIDI file
// was never gated behind freeing tracks[] first (browsing/MIDI preview/
// WAV playback never touch that gate, see openSelected()'s FILE_MOD/
// FILE_S3M/FILE_XM check), so its lifetime isn't guaranteed disjoint from
// tracks[]'s the way the other three's is -- it can't safely share that
// block.
MidiPlayer* player = nullptr;
WavPlayer wavPlayer;
ModPlayer* modPlayer = nullptr;
S3mPlayer* s3mPlayer = nullptr;
XmPlayer* xmPlayer = nullptr;
MidiRecorder recorder;
SysExPlayer sysexPlayer;
SysExRecorder sysexRecorder;

// -- live MIDI-in log, shared by the Recording and Capture SysEx screens
// (see Ui::updateRecordingLog()) -- recorder and sysexRecorder are never
// both active at once (see enter()'s comment), so one buffer for whichever
// of the two is actually running is enough; each arm() clears it so a
// previous take's entries never bleed into the next.
// A small fixed shift-array rather than a ring buffer -- MIDI_LOG_CAPACITY
// is tiny (a handful of struct-copies on push is nothing next to an SPI
// text draw) and this is far easier to read than index/modulo bookkeeping
// for so little gain. Capacity is a bit larger than what actually fits on
// screen (Ui::updateRecordingLog() clips to whatever rows fit) so scrolling
// has a little history to draw from rather than clipping to exactly one
// screen's worth.
const int MIDI_LOG_CAPACITY = 8;
Ui::MidiLogEntry midiLog[MIDI_LOG_CAPACITY];
int midiLogCount = 0;
// Set on every push, cleared once the log's been repainted -- lets the
// APP_RECORDING/APP_CAPTURING_SYSEX ticks below skip the redraw (and its
// SPI cost) entirely during idle stretches with nothing new to show.
bool midiLogDirty = false;

// Pushed from the channel-voice/SysEx input-handler callbacks in enter()
// below, only while the relevant recorder is actually STATE_RECORDING --
// cheap enough (a struct copy, worst case a 7-element memmove) to call
// straight from those callbacks with no throttling; the drawing side is
// what's rate-limited, see APP_RECORDING/APP_CAPTURING_SYSEX's ticks.
void pushMidiLogEntry(const Ui::MidiLogEntry& e) {
    if (midiLogCount == MIDI_LOG_CAPACITY) {
        memmove(&midiLog[0], &midiLog[1], sizeof(Ui::MidiLogEntry) * (MIDI_LOG_CAPACITY - 1));
        midiLog[MIDI_LOG_CAPACITY - 1] = e;
    } else {
        midiLog[midiLogCount++] = e;
    }
    midiLogDirty = true;
}

AppState appState = APP_BROWSE;
int selectedIndex = 0;
int scrollOffset = 0;
bool needsRedraw = true;

// Remembers selectedIndex per directory level, so LEFT/NAV ("go up")
// restores the cursor to wherever it was before descending, instead of
// always landing back on the first entry. Pushed right before entering a
// folder (see openSelected()), popped in handleBrowseInput()'s "go up"
// branch -- scrollOffset isn't stored separately, since
// ensureSelectionVisible() derives it purely from selectedIndex.
const int MAX_DIR_DEPTH = 16;
int dirSelectionStack[MAX_DIR_DEPTH];
int dirStackDepth = 0;

// -- device-wide MIDI/audio settings, shown/adjusted from the player
// screen. Currently only meaningful to this mode (nothing else uses
// MidiOutput/Synth yet); if the looper ends up needing its own MIDI
// out/audio monitoring, these likely want to move up to main.cpp as
// genuinely shared, mode-independent settings.
MidiOutTarget outputTarget = MIDI_OUT_BOTH;
bool audioOn = false; // onboard synth on/off, see MidiOutput::setAudioOutput()
int volume = 80; // percent, see Synth::setVolume(); mirrored here so the UI can show it
// True once `volume`/`audioOn` have been seeded from SettingsMode::
// defaultVolume()/synthAudioEnabled() -- see enter()'s comment for why
// this only happens once, not on every (re-)entry into this mode.
bool volumeSeededFromSettings = false;
bool audioSeededFromSettings = false;

char nowPlayingName[MAX_FILENAME_LEN] = {0};
char nowRecordingName[MAX_FILENAME_LEN] = {0}; // base name of the in-progress recording
char nowCapturingSysExName[MAX_FILENAME_LEN] = {0}; // base name of the in-progress SysEx capture

// True exactly while STATE_PAUSED is showing because NAV stopped
// playback (reset to the start), rather than PLAY pausing it in place --
// MidiPlayer itself has only one STATE_PAUSED for both, so this is what
// lets the State row say "Stopped" instead of "Paused" (see
// handlePlayInput()). Reset false on every fresh open and every PLAY
// press; only NAV's stop sets it true.
bool stoppedViaNav = false;

// True once EDIT-held has already been used as a modifier (for volume)
// during the current press -- set by handleVolumeHold(), reset on every
// fresh EDIT press. Lets EDIT's release toggle audio only for a genuine
// tap, same tap-vs-hold disambiguation LooperMode uses for its own
// Metro/Count-In EDIT-tap-vs-EDIT-held rows.
bool editUsedAsModifier = false;

// Same tap-vs-hold-modifier disambiguation as editUsedAsModifier above,
// for ALT: a bare tap cycles the MIDI output target (cycleOutputTarget()),
// but ALT held + EDIT (tap) toggles Audio instead (see handlePlayInput()'s
// ALT+EDIT check) -- set true the moment that combo fires, so ALT's own
// release doesn't ALSO cycle the output target for the same physical
// press.
bool altUsedAsModifier = false;

// -- APP_SYNTH_EDIT: pariSynth channel-grid quick-edit overlay -----------
//
// ALT+EDIT opens the same 16-channel grid PariSynthMode's own play screen
// shows -- input handling for it is owned by the shared PariSynthGrid
// component (see pari_synth_grid.h), not hand-written here, so this
// screen behaves identically to PariSynthMode's own (manual EDIT+LEFT/
// RIGHT reassignment, EDIT-tap to edit, ENTER for the bank menu, PLAY for
// the picker, EDIT+UP/DOWN for volume, all included) instead of the
// smaller hand-rolled subset this used to implement separately. Wraps
// SynthEditor the same way PariSynthMode itself does: SynthEditor::
// update() returning true means "the field editor was backed out of",
// which resumes this grid rather than exiting APP_SYNTH_EDIT outright --
// NAV/LEFT from the grid itself is what actually returns to APP_PLAY (see
// pari_synth_grid.h's own comment on why that deliberately does NOT call
// MidiOutput::allNotesOffAllChannels() the way PariSynthMode's own exit
// does -- the file is still playing underneath). No live-input
// registration here either way -- this is reached mid-playback, where the
// file's own Program Change events already drive every channel, and
// FilePlayerMode's existing MIDI Thru/input handler stays registered
// throughout (see APP_SYNTH_EDIT's own enum comment).
bool synthEditShowingGrid = false; // false once the field editor (SynthEditor) has draw/input focus

// -- shared name-entry editor state (New Recording / New Folder / Rename) --
NamePurpose namePurpose = NAME_NEW_RECORDING;
char nameEntryBuf[NAME_ENTRY_MAX_LEN + 1] = {0}; // editable base name, no extension
int nameEntryLen = 0;         // current length of nameEntryBuf (append/backspace at the end)
int nameEntryKeyRow = 1;      // on-screen keyboard cursor -- deliberately NOT reset per
int nameEntryKeyCol = 0;      // keystroke, only when a new name-entry session begins
char nameEntryError[40] = {0};
char renameOldPath[224] = {0}; // NAME_RENAME only: absolute path being renamed
bool renameIsDir = false;      // NAME_RENAME only
FileKind renameKind = FILE_MID; // NAME_RENAME only, meaningless if renameIsDir -- which extension to re-append

// -- APP_CONFIRM_OVERWRITE state -- see finishNameEntry()/
// handleConfirmOverwriteInput(). NAME_NEW_FOLDER never reaches this (see
// finishNameEntry()'s own comment on why), so these are only ever
// meaningful for NAME_NEW_RECORDING/NAME_NEW_SYSEX_CAPTURE/NAME_RENAME.
char pendingName[NAME_ENTRY_MAX_LEN + 1] = {0}; // trimmed name to actually act on once the collision's resolved
char pendingOverwritePath[224] = {0}; // the colliding path that would be removed on confirm
bool pendingOverwriteIsDir = false;   // whether pendingOverwritePath is a directory (only reachable via NAME_RENAME)

// -- entry action menu state --
MenuEntry menuEntries[7];
int menuEntryCount = 0;
int menuCursor = 0;
char menuSubtitle[MAX_FILENAME_LEN + 8] = {0};

bool deleteFailed = false;

// -- APP_LOOPER_TRACK_PICK / pending-import handoff state --
// See beginLoopTrackPick() and hasPendingLooperImport()/consumeLooperImport().
char looperImportPath[224] = {0};
int looperTrackPickCursor = 0;
bool pendingLooperImport = false;
int pendingLooperImportTrack = 0;

// Doesn't redraw anything itself -- the browse screen never displays
// outputTarget, so a redraw there would be a full-screen flash for zero
// visible change, while the player screen needs a specific partial update
// (see updatePlayerOutputTarget()). Left to each call site instead.
void cycleOutputTarget() {
    switch (outputTarget) {
        case MIDI_OUT_HARDWARE: outputTarget = MIDI_OUT_USB; break;
        case MIDI_OUT_USB:      outputTarget = MIDI_OUT_BOTH; break;
        case MIDI_OUT_BOTH:     outputTarget = MIDI_OUT_OFF; break;
        default:                outputTarget = MIDI_OUT_HARDWARE; break; // MIDI_OUT_OFF falls here too
    }
    MidiOutput::setTarget(outputTarget);
}

// Onboard synth on/off -- a separate axis from outputTarget, which only
// chooses between the two MIDI wire protocols and has no bearing on
// whether the synth is making any sound. Only ever called from the player
// screen, which redraws its own affected row at the call site.
void toggleAudio() {
    audioOn = !audioOn;
    MidiOutput::setAudioOutput(audioOn);
    if (!audioOn) {
        // MidiOutput::sendNoteOn()/sendNoteOff() only forward to Synth
        // while audioOutEnabled() -- so a note whose NoteOn arrived while
        // audio was on but whose NoteOff arrives after this toggle would
        // never reach Synth::noteOff(), leaving that voice stuck sounding
        // forever. Release everything on the way out, same as playback
        // stop/panic already does (see MidiOutput::allNotesOffAllChannels()'s
        // own Synth::allNotesOff() call) -- calling Synth::allNotesOff()
        // directly rather than that wrapper, since this is purely a local-
        // monitoring toggle and firing CC 120/123 out the actual MIDI ports
        // too would be an unrelated, unwanted side effect.
        Synth::allNotesOff();
    }
}

void adjustVolume(int delta) {
    int v = volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == volume) return;
    volume = v;
    Synth::setVolume((uint8_t)volume);
    Ui::updateVolume(volume);
}

// Whenever selectedIndex lands outside the currently visible page, snaps
// straight to the (fixed, `rows`-aligned) page that contains it, rather
// than scrolling by just enough to keep it in view. That minimal-shift
// approach used to mean DOWN on the last visible row dragged every other
// row up by one to reveal it -- confusing to watch, and not actually "the
// next page". Snapping instead means DOWN on a page's last item always
// lands on the next page's first item (row 0), and UP on a page's first
// item lands on the previous page's last item -- a clean page flip either
// way, for a single-step move same as for ALT+UP/DOWN's explicit jump.
void ensureSelectionVisible() {
    int rows = Ui::visibleRows();
    if (rows <= 0) return;
    if (selectedIndex < scrollOffset || selectedIndex >= scrollOffset + rows) {
        scrollOffset = (selectedIndex / rows) * rows;
    }
    if (scrollOffset < 0) scrollOffset = 0;
}

// Moves the browser selection to `newIndex`. If the visible page doesn't
// need to scroll, this repaints only the two affected rows instead of
// going through a full needsRedraw pass (avoids a full-screen flicker on
// every up/down press).
void moveSelection(int newIndex) {
    int prevIndex = selectedIndex;
    int prevScroll = scrollOffset;
    selectedIndex = newIndex;
    ensureSelectionVisible();
    if (scrollOffset == prevScroll) {
        Ui::updateBrowserSelection(browser, prevIndex, selectedIndex, scrollOffset);
    } else {
        needsRedraw = true;
    }
}

// Moves the selection by `delta` (positive or negative, any magnitude),
// wrapping around the list either direction -- UP on the first entry
// lands on the last, DOWN on the last lands back on the first. Used by
// handleBrowseScroll()'s single-step scroll; handleBrowsePaging()'s
// full-page jump uses stepSelectionClamped() instead, which stops at
// either end rather than wrapping.
void stepSelection(int delta) {
    int count = browser.entryCount();
    if (count <= 0) return;
    int newIndex = ((selectedIndex + delta) % count + count) % count;
    moveSelection(newIndex);
}

// Same as stepSelection(), but clamps to [0, count-1] instead of
// wrapping -- so repeated full-page jumps stop dead at the first/last
// entry instead of looping back around to the other end.
void stepSelectionClamped(int delta) {
    int count = browser.entryCount();
    if (count <= 0) return;
    int newIndex = selectedIndex + delta;
    if (newIndex < 0) newIndex = 0;
    if (newIndex >= count) newIndex = count - 1;
    moveSelection(newIndex);
}

// Holding UP/DOWN keeps stepping through the list (wrapping, see
// stepSelection()) instead of just moving once per press, speeding up
// the longer it's held: normal pace for the first second, faster from
// 1-3s, fastest past 3s. Skipped while ALT is held -- that's a full-page
// jump instead (see handleBrowsePaging()), not a continuation of this.
//
// The first repeat waits for INITIAL_DELAY_MS, distinct from (and longer
// than) NORMAL_INTERVAL_MS between the repeats after that -- standard
// typematic-delay-vs-typematic-rate behavior. Without that separate,
// longer delay, an ordinary press-and-release lasting anywhere over
// NORMAL_INTERVAL_MS (150ms -- not a long press by human standards) would
// already trigger a second step before the button was even released,
// which read as a spurious double-move, especially jarring right at a
// page boundary where the first step alone already flips the whole page.
void handleBrowseScroll() {
    if (Input::isDown(BTN_ALT)) return;

    const uint32_t INITIAL_DELAY_MS = 400;
    const uint32_t STAGE1_AFTER_MS = 1000;
    const uint32_t STAGE2_AFTER_MS = 3000;
    const uint32_t NORMAL_INTERVAL_MS = 150;
    const uint32_t STAGE1_INTERVAL_MS = 60;
    const uint32_t STAGE2_INTERVAL_MS = 25;

    static uint32_t upPressedAtMs = 0, downPressedAtMs = 0;
    static uint32_t lastUpStep = 0, lastDownStep = 0;
    uint32_t now = millis();

    if (Input::justPressed(BTN_UP)) upPressedAtMs = now;
    if (Input::justPressed(BTN_DOWN)) downPressedAtMs = now;

    if (Input::isDown(BTN_UP)) {
        uint32_t held = now - upPressedAtMs;
        bool ready;
        if (Input::justPressed(BTN_UP)) {
            ready = true;
        } else if (held < INITIAL_DELAY_MS) {
            ready = false;
        } else {
            uint32_t interval = held >= STAGE2_AFTER_MS ? STAGE2_INTERVAL_MS
                               : held >= STAGE1_AFTER_MS ? STAGE1_INTERVAL_MS
                                                          : NORMAL_INTERVAL_MS;
            ready = now - lastUpStep >= interval;
        }
        if (ready) {
            stepSelection(-1);
            lastUpStep = now;
        }
    }
    if (Input::isDown(BTN_DOWN)) {
        uint32_t held = now - downPressedAtMs;
        bool ready;
        if (Input::justPressed(BTN_DOWN)) {
            ready = true;
        } else if (held < INITIAL_DELAY_MS) {
            ready = false;
        } else {
            uint32_t interval = held >= STAGE2_AFTER_MS ? STAGE2_INTERVAL_MS
                               : held >= STAGE1_AFTER_MS ? STAGE1_INTERVAL_MS
                                                          : NORMAL_INTERVAL_MS;
            ready = now - lastDownStep >= interval;
        }
        if (ready) {
            stepSelection(1);
            lastDownStep = now;
        }
    }
}

// ALT held + UP/DOWN jumps a full page (one screenful of rows) at a time
// instead of one entry -- tap-triggered, not hold-repeated, since this is
// meant as a deliberate page-at-a-time jump rather than a faster version
// of handleBrowseScroll()'s continuous scroll. Sets `altUsedAsModifier`
// whenever it fires, so handleBrowseInput() knows NOT to also treat this
// same ALT press as its stand-alone "cycle output target" tap on release
// -- same tap-vs-modifier pattern used elsewhere in this app (e.g.
// LooperMode's ENTER+PLAY erase chord).
//
// Unlike handleBrowseScroll()'s single-step scroll, this clamps at the
// list ends instead of wrapping: a page jump that overshoots the last
// (or first) entry lands exactly on it rather than looping around to the
// other end, so repeated ALT+DOWN reliably parks on the last item.
bool handleBrowsePaging() {
    if (!Input::isDown(BTN_ALT)) return false;

    bool usedAsModifier = false;
    int rows = Ui::visibleRows();
    if (Input::justPressed(BTN_UP)) {
        stepSelectionClamped(-rows);
        usedAsModifier = true;
    }
    if (Input::justPressed(BTN_DOWN)) {
        stepSelectionClamped(rows);
        usedAsModifier = true;
    }
    return usedAsModifier;
}

void openSelected() {
    int count = browser.entryCount();
    if (selectedIndex < 0 || selectedIndex >= count) return;

    const BrowserEntry& e = browser.entry(selectedIndex);
    if (e.isDir) {
        if (dirStackDepth < MAX_DIR_DEPTH) dirSelectionStack[dirStackDepth++] = selectedIndex;
        browser.enterDir(selectedIndex);
        selectedIndex = 0;
        scrollOffset = 0;
        appState = APP_BROWSE;
        needsRedraw = true;
        return;
    }

    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));

    // nowPlayingName is shared by APP_PLAY/APP_PLAY_SYSEX/APP_PLAY_WAV/
    // APP_PLAY_MOD/APP_PLAY_S3M/APP_PLAY_XM (Ui::drawPlayer()/
    // Ui::drawSysExPlayer()/Ui::drawWavPlayer()/Ui::drawModPlayer()/
    // Ui::drawS3mPlayer()/Ui::drawXmPlayer() respectively) -- the six
    // states are mutually exclusive, so there's no risk of one clobbering
    // another's in-use name.
    strncpy(nowPlayingName, e.name, sizeof(nowPlayingName) - 1);
    nowPlayingName[sizeof(nowPlayingName) - 1] = '\0';

    if (e.kind == FILE_SYX) {
        sysexPlayer.load(path); // failure surfaces via sysexPlayer.state() on the playback screen
        appState = APP_PLAY_SYSEX;
        needsRedraw = true;
        return;
    }

    stoppedViaNav = false; // a fresh open is never "stopped", see its own comment

    if (e.kind == FILE_WAV) {
        if (wavPlayer.load(path)) wavPlayer.play();
        // On failure, wavPlayer.state() == STATE_ERROR and drawWavPlayer()
        // shows wavPlayer.errorMessage() -- still switch screens so the
        // user sees it, same convention as the MIDI branch below.
        appState = APP_PLAY_WAV;
        needsRedraw = true;
        return;
    }

    if (e.kind == FILE_MOD || e.kind == FILE_S3M || e.kind == FILE_XM) {
        // The big tracker-format players are the only reason File Player
        // needs LooperMode's tracks[] RAM back -- browsing, MIDI preview,
        // and WAV playback never hit this. Only worth the interactive gate
        // if there's actually something to lose; otherwise free (a no-op
        // if tracks was never allocated) and fall straight through below,
        // same as always. See APP_FREE_LOOPER_RAM's declaration and
        // handleFreeLooperRamGateInput() -- re-entering openSelected() is
        // the retry mechanism once the gate resolves (selectedIndex/browser
        // are untouched while it's up, so it lands right back here, and by
        // then hasUnsavedTrackContent() is false).
        if (LooperMode::hasUnsavedTrackContent()) {
            appState = APP_FREE_LOOPER_RAM;
            needsRedraw = false; // LooperMode::beginFreeTracksForFilePlayer() below draws synchronously
            LooperMode::beginFreeTracksForFilePlayer();
            return;
        }
        LooperMode::freeTracksIfAllocated();
    }

    if (e.kind == FILE_MOD) {
        // Placement-new into SharedRamArena rather than a plain heap
        // new() -- see that header's comment: this shares LooperMode::
        // tracks[]'s fixed block (the LooperMode::freeTracksIfAllocated()
        // call just above guarantees tracks is down first), so opening a
        // .mod can never fail or hang on heap fragmentation the way the
        // old new(nothrow) path theoretically could. Freed on the "LEFT ->
        // back to browser" handler in handlePlayModInput().
        static_assert(sizeof(ModPlayer) <= SharedRamArena::SIZE,
                      "ModPlayer no longer fits SharedRamArena -- see shared_ram_arena.h");
        if (!modPlayer) modPlayer = new (SharedRamArena::data()) ModPlayer();
        if (modPlayer->load(path)) modPlayer->play();
        // Same "switch screens either way, error shows on-screen" convention.
        appState = APP_PLAY_MOD;
        needsRedraw = true;
        return;
    }

    if (e.kind == FILE_S3M) {
        // See modPlayer's identical comment just above.
        static_assert(sizeof(S3mPlayer) <= SharedRamArena::SIZE,
                      "S3mPlayer no longer fits SharedRamArena -- see shared_ram_arena.h");
        if (!s3mPlayer) s3mPlayer = new (SharedRamArena::data()) S3mPlayer();
        if (s3mPlayer->load(path)) s3mPlayer->play();
        // Same "switch screens either way, error shows on-screen" convention.
        appState = APP_PLAY_S3M;
        needsRedraw = true;
        return;
    }

    if (e.kind == FILE_XM) {
        // See modPlayer's identical comment just above -- this also
        // resolves the heap-fragmentation crash the g_xmDiag*/Serial
        // diagnostics in xm_file.cpp were added to chase (a same-session
        // CHUNK_SIZE increase crashing on real hardware): XmPlayer no
        // longer competes with the general heap at all.
        static_assert(sizeof(XmPlayer) <= SharedRamArena::SIZE,
                      "XmPlayer no longer fits SharedRamArena -- see shared_ram_arena.h");
        if (!xmPlayer) xmPlayer = new (SharedRamArena::data()) XmPlayer();
        if (xmPlayer->load(path)) xmPlayer->play();
        // Same "switch screens either way, error shows on-screen" convention.
        appState = APP_PLAY_XM;
        needsRedraw = true;
        return;
    }

    // See modPlayer's identical on-demand comment above.
    if (!player) player = new (std::nothrow) MidiPlayer();
    if (!player) {
        Ui::drawMessage("Not enough RAM", "to open this file");
        return;
    }
    if (player->load(path)) {
        // A genuinely fresh open (as opposed to restarting a just-finished
        // file via PLAY, see handlePlayInput()) starts at the file's own
        // tempo, not whatever was last dialed in, and with a clean synth
        // instrument slate so the previous file's Program Change choices
        // don't bleed into this one before it sends its own.
        player->setTempoScale(1.0f);
        Synth::resetPrograms();
        player->play();
    }
    // On failure, player->state() == STATE_ERROR and drawPlayer() shows
    // player->errorMessage() -- still switch screens so the user sees it.
    appState = APP_PLAY;
    needsRedraw = true;
}

// Picks "REC001", "REC002", etc. -- one past the highest existing
// REC<N>.mid in the current directory, so repeated recordings in the same
// folder never collide without the user having to think about it.
void generateDefaultRecordingName(char* out, size_t outSize) {
    int nextNum = 1;
    for (int i = 0; i < browser.entryCount(); i++) {
        const BrowserEntry& e = browser.entry(i);
        if (e.isDir) continue;
        if (strncasecmp(e.name, "REC", 3) != 0) continue;
        int num = atoi(e.name + 3);
        if (num >= nextNum) nextNum = num + 1;
    }
    snprintf(out, outSize, "REC%03d", nextNum);
}

// Same idea as generateDefaultRecordingName(), "DUMP001", "DUMP002", etc.,
// scanning existing .syx entries instead of .mid ones.
void generateDefaultSysExName(char* out, size_t outSize) {
    int nextNum = 1;
    for (int i = 0; i < browser.entryCount(); i++) {
        const BrowserEntry& e = browser.entry(i);
        if (e.isDir || e.kind != FILE_SYX) continue;
        if (strncasecmp(e.name, "DUMP", 4) != 0) continue;
        int num = atoi(e.name + 4);
        if (num >= nextNum) nextNum = num + 1;
    }
    snprintf(out, outSize, "DUMP%03d", nextNum);
}

// Loads `initial` (truncated to NAME_ENTRY_MAX_LEN) into the shared
// editor buffer. The on-screen keyboard cursor resets for each new
// session, but -- per the whole point of this screen -- is left wherever
// the user leaves it between keystrokes within a session, so picking
// several nearby letters stays quick. Where it resets *to* depends on
// purpose: New Recording/SysEx Capture start pre-filled with an
// auto-generated name (see generateDefaultRecordingName()/
// generateDefaultSysExName()) that's good enough to just accept most of
// the time, so the cursor starts right on OK -- a bare ENTER saves it
// immediately, no detour up to 'Q' first needed just to reach OK. New
// Folder (always empty) and Rename (the existing name, which -- unlike
// the other two -- you've very deliberately opened this screen to
// *change*) still start at the top-left letter key, since there's real
// typing to do either way.
void beginNameEntry(NamePurpose purpose, const char* initial) {
    namePurpose = purpose;
    nameEntryError[0] = '\0';

    int len = (int)strlen(initial);
    if (len > NAME_ENTRY_MAX_LEN) len = NAME_ENTRY_MAX_LEN;
    memcpy(nameEntryBuf, initial, len);
    nameEntryBuf[len] = '\0';
    nameEntryLen = len;

    if (purpose == NAME_NEW_RECORDING || purpose == NAME_NEW_SYSEX_CAPTURE) {
        nameEntryKeyRow = KEYBOARD_ROW_COUNT - 1;
        nameEntryKeyCol = KEYBOARD_ROW_LENS[KEYBOARD_ROW_COUNT - 1] - 1; // OK is always the last key of the last row
    } else {
        nameEntryKeyRow = 1; // top-left letter key ('Q')
        nameEntryKeyCol = 0;
    }

    appState = APP_NAME_ENTRY;
    needsRedraw = true;
}

void beginNewRecordingName() {
    char base[NAME_ENTRY_MAX_LEN + 1];
    generateDefaultRecordingName(base, sizeof(base));
    beginNameEntry(NAME_NEW_RECORDING, base);
}

void beginNewSysExCaptureName() {
    char base[NAME_ENTRY_MAX_LEN + 1];
    generateDefaultSysExName(base, sizeof(base));
    beginNameEntry(NAME_NEW_SYSEX_CAPTURE, base);
}

void beginNewFolderName() {
    beginNameEntry(NAME_NEW_FOLDER, "");
}

// The extension that goes with a given file kind -- used for both
// stripping (rename entry) and re-appending (rename commit).
const char* extensionForKind(FileKind kind) {
    switch (kind) {
        case FILE_SYX: return ".syx";
        case FILE_WAV: return ".wav";
        case FILE_MOD: return ".mod";
        case FILE_S3M: return ".s3m";
        default:        return ".mid";
    }
}

void beginRename() {
    if (selectedIndex < 0 || selectedIndex >= browser.entryCount()) return;
    const BrowserEntry& e = browser.entry(selectedIndex);
    browser.buildFullPath(selectedIndex, renameOldPath, sizeof(renameOldPath));
    renameIsDir = e.isDir;
    renameKind = e.kind;

    char base[NAME_ENTRY_MAX_LEN + 1];
    strncpy(base, e.name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    if (!e.isDir) {
        // Strip the extension for editing; re-appended on save.
        size_t nlen = strlen(base);
        const char* ext = extensionForKind(renameKind);
        if (nlen > 4 && strcasecmp(base + nlen - 4, ext) == 0) base[nlen - 4] = '\0';
    }
    beginNameEntry(NAME_RENAME, base);
}

// Moves the on-screen keyboard cursor by (dRow, dCol). Rows have
// different key counts (10/10/9/9/3), so a vertical move clamps the
// column into the destination row's range rather than trying to track an
// exact pixel position -- simple and predictable, same as most such grids.
// Repaints only the two affected keys instead of the whole screen.
void moveKeyboardCursor(int dRow, int dCol) {
    int newRow = nameEntryKeyRow + dRow;
    if (newRow < 0) newRow = 0;
    if (newRow >= KEYBOARD_ROW_COUNT) newRow = KEYBOARD_ROW_COUNT - 1;

    int newCol = (dRow != 0) ? nameEntryKeyCol : nameEntryKeyCol + dCol;
    if (newCol < 0) newCol = 0;
    if (newCol >= KEYBOARD_ROW_LENS[newRow]) newCol = KEYBOARD_ROW_LENS[newRow] - 1;

    if (newRow == nameEntryKeyRow && newCol == nameEntryKeyCol) return; // at an edge, nothing moved

    int prevRow = nameEntryKeyRow, prevCol = nameEntryKeyCol;
    nameEntryKeyRow = newRow;
    nameEntryKeyCol = newCol;
    Ui::updateNameEntryKey(prevRow, prevCol, nameEntryKeyRow, nameEntryKeyCol);
}

// ".mid"/".syx"/".wav" for a recording/capture or a file rename, "" for a
// folder rename/create -- shared by the draw-dispatch (full redraw) and
// the live preview update (partial redraw) so both agree on what's
// appended after the typed name.
const char* currentNameSuffix() {
    if (namePurpose == NAME_RENAME) return renameIsDir ? "" : extensionForKind(renameKind);
    if (namePurpose == NAME_NEW_RECORDING) return ".mid";
    if (namePurpose == NAME_NEW_SYSEX_CAPTURE) return ".syx";
    return "";
}

// Actually performs whatever namePurpose calls for -- called once any
// name collision has already been resolved, either because there wasn't
// one or because the user just confirmed overwriting it (see
// finishNameEntry()/handleConfirmOverwriteInput()). `trimmed` is the
// already-validated, trailing-space-trimmed name to act on. Failures
// (only filesystem errors at this point -- the collision case is handled
// before this is ever called) stay on the editor screen with
// `nameEntryError` set rather than navigating away, so the user can just
// change the name and retry.
void performNameEntryAction(const char* trimmed) {
    if (namePurpose == NAME_NEW_RECORDING) {
        char filename[NAME_ENTRY_MAX_LEN + 5];
        snprintf(filename, sizeof(filename), "%s.mid", trimmed);
        char path[224];
        browser.buildPath(filename, path, sizeof(path));

        strncpy(nowRecordingName, trimmed, sizeof(nowRecordingName) - 1);
        nowRecordingName[sizeof(nowRecordingName) - 1] = '\0';

        recorder.arm(path); // failure (e.g. the collision we already cleared reappearing from a race) surfaces via recorder.state() on the recording screen
        midiLogCount = 0; // clear any previous take's log before this one starts receiving
        appState = APP_RECORDING;
        needsRedraw = true;
        return;
    }

    if (namePurpose == NAME_NEW_SYSEX_CAPTURE) {
        char filename[NAME_ENTRY_MAX_LEN + 5];
        snprintf(filename, sizeof(filename), "%s.syx", trimmed);
        char path[224];
        browser.buildPath(filename, path, sizeof(path));

        strncpy(nowCapturingSysExName, trimmed, sizeof(nowCapturingSysExName) - 1);
        nowCapturingSysExName[sizeof(nowCapturingSysExName) - 1] = '\0';

        sysexRecorder.arm(path); // failure surfaces via sysexRecorder.state() on the capture screen
        midiLogCount = 0; // clear any previous take's log before this one starts receiving
        appState = APP_CAPTURING_SYSEX;
        needsRedraw = true;
        return;
    }

    if (namePurpose == NAME_NEW_FOLDER) {
        char path[224];
        browser.buildPath(trimmed, path, sizeof(path));
        if (!sd.mkdir(path)) {
            strncpy(nameEntryError, "could not create folder", sizeof(nameEntryError) - 1);
        } else {
            browser.refresh();
            appState = APP_BROWSE;
        }
        // Leaving APP_NAME_ENTRY needs a full redraw (browser replaces the
        // whole screen); staying on it after an error only needs the
        // footer's error line touched.
        if (appState == APP_BROWSE) needsRedraw = true;
        else Ui::updateNameEntryError(nameEntryError);
        return;
    }

    // NAME_RENAME
    char newPath[224];
    if (renameIsDir) {
        browser.buildPath(trimmed, newPath, sizeof(newPath));
    } else {
        char filename[NAME_ENTRY_MAX_LEN + 5];
        snprintf(filename, sizeof(filename), "%s%s", trimmed, extensionForKind(renameKind));
        browser.buildPath(filename, newPath, sizeof(newPath));
    }
    if (strcmp(newPath, renameOldPath) == 0) {
        appState = APP_BROWSE; // unchanged -- nothing to do
    } else if (!sd.rename(renameOldPath, newPath)) {
        strncpy(nameEntryError, "rename failed", sizeof(nameEntryError) - 1);
    } else {
        browser.refresh();
        appState = APP_BROWSE;
    }
    if (appState == APP_BROWSE) needsRedraw = true;
    else Ui::updateNameEntryError(nameEntryError);
}

// Trims any trailing spaces the user typed via the SPACE key off
// nameEntryBuf, then either performs whatever namePurpose calls for
// directly (performNameEntryAction()) or, if that would collide with an
// existing file/folder, detours through APP_CONFIRM_OVERWRITE first (see
// handleConfirmOverwriteInput()) -- same "already exists" collision every
// one of these actions can hit, now resolved with an explicit overwrite
// choice instead of an outright reject. NAME_NEW_FOLDER is the one
// exception: creating a folder that already exists as a folder isn't a
// meaningful "overwrite" (there's no content to replace, just a name to
// reuse), and treating it as one would mean recursively deleting whatever
// that folder already contains just to recreate it empty -- a much bigger
// destructive surprise than replacing a single file, so it keeps the
// plain reject-and-retry behavior it already had.
void finishNameEntry() {
    char trimmed[NAME_ENTRY_MAX_LEN + 1];
    strncpy(trimmed, nameEntryBuf, sizeof(trimmed));
    trimmed[NAME_ENTRY_MAX_LEN] = '\0';
    int len = (int)strlen(trimmed);
    while (len > 0 && trimmed[len - 1] == ' ') trimmed[--len] = '\0';
    if (len == 0) return; // nothing typed -- stay on the naming screen

    if (namePurpose == NAME_NEW_FOLDER) {
        performNameEntryAction(trimmed);
        return;
    }

    // Compute the same target path performNameEntryAction() itself would,
    // just to check for a collision before doing anything.
    char path[224];
    bool isDir = false;
    if (namePurpose == NAME_RENAME) {
        if (renameIsDir) {
            browser.buildPath(trimmed, path, sizeof(path));
            isDir = true; // describes whatever's already AT this path, not the thing being renamed
        } else {
            char filename[NAME_ENTRY_MAX_LEN + 5];
            snprintf(filename, sizeof(filename), "%s%s", trimmed, extensionForKind(renameKind));
            browser.buildPath(filename, path, sizeof(path));
        }
        if (strcmp(path, renameOldPath) == 0) {
            performNameEntryAction(trimmed); // unchanged name -- its own no-op path handles this
            return;
        }
    } else {
        char filename[NAME_ENTRY_MAX_LEN + 5];
        snprintf(filename, sizeof(filename), "%s%s", trimmed, currentNameSuffix());
        browser.buildPath(filename, path, sizeof(path));
    }

    if (sd.exists(path)) {
        strncpy(pendingName, trimmed, sizeof(pendingName) - 1);
        pendingName[sizeof(pendingName) - 1] = '\0';
        strncpy(pendingOverwritePath, path, sizeof(pendingOverwritePath) - 1);
        pendingOverwritePath[sizeof(pendingOverwritePath) - 1] = '\0';
        pendingOverwriteIsDir = isDir;
        appState = APP_CONFIRM_OVERWRITE;
        needsRedraw = true;
    } else {
        performNameEntryAction(trimmed);
    }
}

// ENTER removes whatever's at pendingOverwritePath and then proceeds with
// the original action, same as if there'd been no collision at all; NAV
// goes back to APP_NAME_ENTRY (not all the way to APP_BROWSE) so a
// change-my-mind cancel lands back on editing the name, same place every
// other name-entry error already leaves you.
void handleConfirmOverwriteInput() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        appState = APP_NAME_ENTRY;
        needsRedraw = true;
        return;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        bool ok = pendingOverwriteIsDir ? sd.rmdir(pendingOverwritePath) : sd.remove(pendingOverwritePath);
        if (!ok) {
            strncpy(nameEntryError, "overwrite failed", sizeof(nameEntryError) - 1);
            appState = APP_NAME_ENTRY;
            needsRedraw = true;
            return;
        }
        performNameEntryAction(pendingName);
    }
}

// Builds the contextual action list for whatever's currently selected in
// the browser: Open/Rename/Delete only appear when there's a valid
// selection, New Recording/Capture SysEx Dump/New Folder are always
// offered.
void beginEntryMenu() {
    bool hasSelection = (selectedIndex >= 0 && selectedIndex < browser.entryCount());
    // The browser only ever lists directories, .mid files, .syx files,
    // .wav files, .mod files, and .s3m files (see drawBrowser()'s "(no
    // .mid/.syx/.wav/.mod/.s3m files here)" case) -- so a non-directory
    // selection is always one of those five. "Load to Looper" only makes
    // sense for .mid -- the looper is a MIDI-event looper, not an audio
    // one, same reason .syx/.wav/.mod/.s3m never offer it.
    // Fetched once into a local reference (rather than three separate
    // browser.entry(selectedIndex) calls) since entry() now materializes
    // on demand -- see SdBrowser's own comment.
    static const BrowserEntry emptySelection = {};
    const BrowserEntry& selected = hasSelection ? browser.entry(selectedIndex) : emptySelection;
    bool isFile = hasSelection && !selected.isDir;
    bool isMidFile = isFile && selected.kind == FILE_MID;

    menuEntryCount = 0;
    menuEntries[menuEntryCount++] = {MENU_NEW_RECORDING, "New Recording"};
    menuEntries[menuEntryCount++] = {MENU_CAPTURE_SYSEX, "Capture SysEx Dump"};
    if (isMidFile) {
        // Not offered for a .syx selection -- the looper works in terms of
        // MIDI events/tracks, which a raw SysEx dump doesn't have.
        menuEntries[menuEntryCount++] = {MENU_LOAD_TO_LOOPER, "Load to Looper"};
    }
    if (hasSelection) {
        menuEntries[menuEntryCount++] = {MENU_OPEN, "Open"};
        menuEntries[menuEntryCount++] = {MENU_RENAME, "Rename"};
        menuEntries[menuEntryCount++] = {MENU_DELETE, "Delete"};
    }
    menuEntries[menuEntryCount++] = {MENU_NEW_FOLDER, "New Folder"};

    if (hasSelection) {
        strncpy(menuSubtitle, selected.name, sizeof(menuSubtitle) - 1);
        menuSubtitle[sizeof(menuSubtitle) - 1] = '\0';
    } else {
        strncpy(menuSubtitle, "Menu", sizeof(menuSubtitle) - 1);
    }

    menuCursor = 0;
    appState = APP_ENTRY_MENU;
    needsRedraw = true;
}

void beginConfirmDelete() {
    if (selectedIndex < 0 || selectedIndex >= browser.entryCount()) return;
    deleteFailed = false;
    appState = APP_CONFIRM_DELETE;
    needsRedraw = true;
}

// "Load to Looper" from the entry menu: stashes the selected file's full
// path and drops into a small "which track" picker. The actual import
// (and any overwrite warning, since only LooperMode knows track state)
// happens after main.cpp hands off to LooperMode::requestImport() -- see
// consumeLooperImport().
void beginLoopTrackPick() {
    if (selectedIndex < 0 || selectedIndex >= browser.entryCount()) return;
    browser.buildFullPath(selectedIndex, looperImportPath, sizeof(looperImportPath));
    looperTrackPickCursor = 0;
    appState = APP_LOOPER_TRACK_PICK;
    needsRedraw = true;
}

// Returns true if this tick requested backing all the way out to the
// top-level mode-select screen (BTN_LEFT/BTN_NAV at the browser's root,
// with nowhere further to go up).
bool handleBrowseInput() {
    static bool altUsedAsModifier = false;
    if (handleBrowsePaging()) altUsedAsModifier = true;
    handleBrowseScroll();

    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_RIGHT) ||
        Input::justPressed(BTN_PLAY)) {
        openSelected();
    }
    // ALT means "cycle output target" here, decided on release and
    // suppressed if it was just used as the page-jump modifier above (see
    // handleBrowsePaging()) -- same tap-vs-modifier pattern used
    // elsewhere in this app.
    if (Input::justReleased(BTN_ALT)) {
        if (!altUsedAsModifier) {
            // The browse screen never displays outputTarget -- nothing to
            // redraw here, unlike the player-screen call site below.
            cycleOutputTarget();
        }
        altUsedAsModifier = false;
    }
    if (Input::justPressed(BTN_EDIT)) {
        beginEntryMenu();
    }
    // LEFT and NAV both mean "back" here, same as everywhere else in this
    // mode -- at the root there's nowhere further to go up, so that's the
    // signal to hand off to the top-level mode-select screen instead.
    if (Input::justPressed(BTN_LEFT) || Input::justPressed(BTN_NAV)) {
        if (browser.goUp()) {
            // Restore wherever the cursor was in this directory before
            // descending (see openSelected()'s push), rather than always
            // landing back on the first entry -- clamped in case the
            // directory's contents changed while we were inside.
            if (dirStackDepth > 0) {
                selectedIndex = dirSelectionStack[--dirStackDepth];
                int count = browser.entryCount();
                if (selectedIndex >= count) selectedIndex = count > 0 ? count - 1 : 0;
                if (selectedIndex < 0) selectedIndex = 0;
                ensureSelectionVisible();
            } else {
                selectedIndex = 0;
                scrollOffset = 0;
            }
            needsRedraw = true;
        } else {
            // Leaving the browser entirely from its true root -- unlike
            // every other goUp() call above, there's no smaller parent
            // folder's loadEntries() to naturally replace whatever's
            // currently indexed. If the root itself was a huge folder,
            // its full index (up to MAX_INDEX_ENTRIES) would otherwise
            // sit resident for as long as Mode Select/Settings is up,
            // same disposable-data reclaim reasoning as freeBrowserBuffers()
            // -- just triggered here instead of waiting for a future
            // Looper entry to need the RAM back.
            browser.freeBuffers();
            return true;
        }
    }
    return false;
}

// While held, steps the tempo every REPEAT_INTERVAL_MS instead of once per
// press -- the first step still fires immediately on the press edge, then
// it free-runs until release. While actually playing, the existing 50ms
// updatePlayerLive() tick already redraws the tempo row on its own, so we
// don't force a redraw per step here (that would reintroduce full-screen
// flicker); while paused there's no live tick running, so we do force one
// for immediate feedback.
void handleTempoHold() {
    // EDIT held turns UP/DOWN into the volume control instead (see
    // handleVolumeHold()) -- bail so the two don't both fire off the same
    // button presses.
    if (Input::isDown(BTN_EDIT)) return;

    const uint32_t REPEAT_INTERVAL_MS = 120;
    static uint32_t lastUpStep = 0;
    static uint32_t lastDownStep = 0;
    uint32_t now = millis();
    bool playing = (player->state() == MidiPlayer::STATE_PLAYING);
    bool changed = false;

    if (Input::isDown(BTN_UP) &&
        (Input::justPressed(BTN_UP) || now - lastUpStep >= REPEAT_INTERVAL_MS)) {
        player->adjustTempoScale(0.05f);
        lastUpStep = now;
        changed = true;
    }
    if (Input::isDown(BTN_DOWN) &&
        (Input::justPressed(BTN_DOWN) || now - lastDownStep >= REPEAT_INTERVAL_MS)) {
        player->adjustTempoScale(-0.05f);
        lastDownStep = now;
        changed = true;
    }
    if (changed && !playing) {
        // STATE_ERROR uses a completely different screen layout (see
        // handleOpen()), which updatePlayerLive() isn't safe to draw over.
        if (player->state() == MidiPlayer::STATE_ERROR) needsRedraw = true;
        else Ui::updatePlayerLive(*player);
    }
}

// EDIT + UP/DOWN, held: steps volume every REPEAT_INTERVAL_MS the same
// way handleTempoHold() steps tempo on UP/DOWN alone. Always redraws just
// the volume row directly (via adjustVolume()) rather than relying on the
// periodic live tick -- unlike tempo, volume never changes on its own
// during playback, so there's no reason to poll it there. Marks
// editUsedAsModifier so EDIT's release (see handlePlayInput()) doesn't
// also toggle audio.
void handleVolumeHold() {
    if (!Input::isDown(BTN_EDIT)) return;

    const uint32_t REPEAT_INTERVAL_MS = 120;
    static uint32_t lastUpStep = 0;
    static uint32_t lastDownStep = 0;
    uint32_t now = millis();

    if (Input::isDown(BTN_UP) &&
        (Input::justPressed(BTN_UP) || now - lastUpStep >= REPEAT_INTERVAL_MS)) {
        adjustVolume(5);
        lastUpStep = now;
        editUsedAsModifier = true;
    }
    if (Input::isDown(BTN_DOWN) &&
        (Input::justPressed(BTN_DOWN) || now - lastDownStep >= REPEAT_INTERVAL_MS)) {
        adjustVolume(-5);
        lastDownStep = now;
        editUsedAsModifier = true;
    }
}

// ENTER + LEFT/RIGHT, held: scrubs playback position. Same hold-to-
// accelerate shape as handleTempoHold()/handleVolumeHold() for the
// repeat rate, but the step size itself grows too once accelerated --
// otherwise scrubbing through a long file at a fixed couple-seconds-per-
// tap pace would take forever. Unlike EDIT, ENTER has no standalone tap
// action on this screen, so there's no tap-vs-hold ambiguity to guard
// against here. No-ops outside PLAYING/PAUSED/DONE (nothing loaded, or
// already showing the error layout).
void handleSeekHold() {
    if (!Input::isDown(BTN_ENTER)) return;
    MidiPlayer::State st = player->state();
    if (st != MidiPlayer::STATE_PLAYING && st != MidiPlayer::STATE_PAUSED &&
        st != MidiPlayer::STATE_DONE) {
        return;
    }

    const uint32_t NORMAL_INTERVAL_MS = 150;
    const uint32_t FAST_INTERVAL_MS = 60;
    const uint32_t ACCEL_AFTER_MS = 1500;
    const uint32_t NORMAL_STEP_MS = 2000;
    const uint32_t FAST_STEP_MS = 8000;

    static uint32_t rightPressedAtMs = 0;
    static uint32_t leftPressedAtMs = 0;
    static uint32_t lastRightStep = 0;
    static uint32_t lastLeftStep = 0;
    uint32_t now = millis();

    if (Input::justPressed(BTN_RIGHT)) rightPressedAtMs = now;
    if (Input::justPressed(BTN_LEFT)) leftPressedAtMs = now;

    bool changed = false;
    if (Input::isDown(BTN_RIGHT)) {
        bool accel = now - rightPressedAtMs >= ACCEL_AFTER_MS;
        uint32_t interval = accel ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval) {
            player->seekTo(player->elapsedMs() + (accel ? FAST_STEP_MS : NORMAL_STEP_MS));
            lastRightStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_LEFT)) {
        bool accel = now - leftPressedAtMs >= ACCEL_AFTER_MS;
        uint32_t interval = accel ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) {
            uint32_t step = accel ? FAST_STEP_MS : NORMAL_STEP_MS;
            uint32_t current = player->elapsedMs();
            player->seekTo(current > step ? current - step : 0);
            lastLeftStep = now;
            changed = true;
        }
    }
    if (changed) {
        // A deliberate reposition, not a NAV "stop" -- if this lands on
        // PAUSED (e.g. seeking backward out of STATE_DONE), it should
        // read "Paused", not inherit a stale "Stopped" from an earlier
        // NAV press.
        stoppedViaNav = false;
        if (player->state() == MidiPlayer::STATE_ERROR) {
            needsRedraw = true;
        } else {
            Ui::updatePlayerState(player->state(), stoppedViaNav);
            Ui::updatePlayerLive(*player);
        }
    }
}

// ENTER + LEFT/RIGHT, held: same idea as handleSeekHold() above, for the
// WAV player screen. WavPlayer::seekTo() is real O(1) byte math (see
// wav_file.h) rather than MIDI's reopen-and-rescan, so there's no
// meaningful cost difference between a small and a large jump here --
// still uses the same step-size-grows-too shape for consistency/muscle
// memory with the MIDI player's scrub, not because WAV needs it for
// performance reasons. Can never produce STATE_ERROR (no file reopen
// happens), unlike the MIDI version.
void handleWavSeekHold() {
    if (!Input::isDown(BTN_ENTER)) return;
    WavPlayer::State st = wavPlayer.state();
    if (st != WavPlayer::STATE_PLAYING && st != WavPlayer::STATE_PAUSED &&
        st != WavPlayer::STATE_DONE) {
        return;
    }

    const uint32_t NORMAL_INTERVAL_MS = 150;
    const uint32_t FAST_INTERVAL_MS = 60;
    const uint32_t ACCEL_AFTER_MS = 1500;
    const uint32_t NORMAL_STEP_MS = 2000;
    const uint32_t FAST_STEP_MS = 8000;

    static uint32_t rightPressedAtMs = 0;
    static uint32_t leftPressedAtMs = 0;
    static uint32_t lastRightStep = 0;
    static uint32_t lastLeftStep = 0;
    uint32_t now = millis();

    if (Input::justPressed(BTN_RIGHT)) rightPressedAtMs = now;
    if (Input::justPressed(BTN_LEFT)) leftPressedAtMs = now;

    bool changed = false;
    if (Input::isDown(BTN_RIGHT)) {
        bool accel = now - rightPressedAtMs >= ACCEL_AFTER_MS;
        uint32_t interval = accel ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval) {
            wavPlayer.seekTo(wavPlayer.elapsedMs() + (accel ? FAST_STEP_MS : NORMAL_STEP_MS));
            lastRightStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_LEFT)) {
        bool accel = now - leftPressedAtMs >= ACCEL_AFTER_MS;
        uint32_t interval = accel ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) {
            uint32_t step = accel ? FAST_STEP_MS : NORMAL_STEP_MS;
            uint32_t current = wavPlayer.elapsedMs();
            wavPlayer.seekTo(current > step ? current - step : 0);
            lastLeftStep = now;
            changed = true;
        }
    }
    if (changed) {
        stoppedViaNav = false;
        Ui::updateWavPlayerState(wavPlayer.state(), stoppedViaNav);
        Ui::updateWavPlayerLive(wavPlayer);
    }
}

void handlePlayWavInput() {
    if (Input::justPressed(BTN_PLAY)) {
        if (wavPlayer.state() == WavPlayer::STATE_PLAYING) {
            wavPlayer.pause();
            stoppedViaNav = false;
            Ui::updateWavPlayerState(wavPlayer.state(), stoppedViaNav);
        } else if (wavPlayer.state() == WavPlayer::STATE_PAUSED) {
            wavPlayer.play();
            stoppedViaNav = false;
            Ui::updateWavPlayerState(wavPlayer.state(), stoppedViaNav);
        } else if (wavPlayer.state() == WavPlayer::STATE_DONE) {
            wavPlayer.seekTo(0);
            wavPlayer.play();
            stoppedViaNav = false;
            Ui::updateWavPlayerState(wavPlayer.state(), stoppedViaNav);
            Ui::updateWavPlayerLive(wavPlayer);
        }
    }
    // ENTER held excludes this -- ENTER+LEFT is scrub-back (see handleWavSeekHold()).
    if (Input::justPressed(BTN_LEFT) && !Input::isDown(BTN_ENTER)) {
        wavPlayer.stop();
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    // NAV stops playback: resets to the start and pauses there, shown as
    // "Stopped" -- same stoppedViaNav convention as the MIDI player's NAV
    // handler. Always ends paused regardless of prior state (unlike a
    // plain seekTo(0), which preserves PLAYING) -- that's what makes this
    // "Stop" rather than "seek to the start".
    if (Input::justPressed(BTN_NAV)) {
        if (wavPlayer.state() == WavPlayer::STATE_PLAYING ||
            wavPlayer.state() == WavPlayer::STATE_PAUSED ||
            wavPlayer.state() == WavPlayer::STATE_DONE) {
            wavPlayer.seekTo(0);
            wavPlayer.pause(); // no-op if seekTo() already landed on PAUSED/DONE
            stoppedViaNav = true;
            Ui::updateWavPlayerState(wavPlayer.state(), stoppedViaNav);
            Ui::updateWavPlayerLive(wavPlayer);
        }
    }
    // EDIT+UP/DOWN volume, reused as-is: only touches the shared
    // volume/Synth::setVolume() state, no MidiPlayer coupling. No EDIT-tap
    // action exists on this screen (WAV playback is always audible, no
    // separate on/off toggle), so editUsedAsModifier's tap-vs-hold
    // disambiguation is simply unused here, harmlessly.
    handleVolumeHold();
    handleWavSeekHold();
}

// Reloads the currently-open .mod from the start -- ModPlayer has no
// seekTo() (see its header comment on why a tracker's position isn't a
// simple time offset to jump to), so unlike WavPlayer's O(1) seekTo(0),
// "reset to the start" for MOD means a fresh load() -- the only way to
// correctly reset every channel's state/sequencer position/tempo at once.
bool restartModFromTop() {
    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));
    return modPlayer->load(path);
}

void handlePlayModInput() {
    if (Input::justPressed(BTN_PLAY)) {
        if (modPlayer->state() == ModPlayer::STATE_PLAYING) {
            modPlayer->pause();
            stoppedViaNav = false;
            Ui::updateModPlayerState(modPlayer->state(), stoppedViaNav);
        } else if (modPlayer->state() == ModPlayer::STATE_PAUSED) {
            modPlayer->play();
            stoppedViaNav = false;
            Ui::updateModPlayerState(modPlayer->state(), stoppedViaNav);
        } else if (modPlayer->state() == ModPlayer::STATE_DONE) {
            if (restartModFromTop()) modPlayer->play();
            stoppedViaNav = false;
            // A reload resets far more than State at once (Time/Pattern/
            // Row), same "full redraw is the right tool" reasoning as the
            // MIDI player's own PLAY-on-STATE_DONE branch.
            needsRedraw = true;
        }
    }
    if (Input::justPressed(BTN_LEFT)) {
        modPlayer->stop(); // already closes SD handles internally -- safe to destroy right after
        modPlayer->~ModPlayer(); // not heap-owned (see SharedRamArena) -- no delete
        modPlayer = nullptr;
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    // NAV stops playback: reloads to the start and pauses there, shown as
    // "Stopped" -- same stoppedViaNav convention as the other two players.
    if (Input::justPressed(BTN_NAV)) {
        if (modPlayer->state() == ModPlayer::STATE_PLAYING ||
            modPlayer->state() == ModPlayer::STATE_PAUSED ||
            modPlayer->state() == ModPlayer::STATE_DONE) {
            restartModFromTop();
            stoppedViaNav = true;
            needsRedraw = true;
        }
    }
    // EDIT+UP/DOWN volume, reused as-is -- same reasoning as the WAV
    // screen (only touches shared volume state, no player coupling). No
    // seek (ModPlayer doesn't support it) and no ALT/MIDI-target.
    handleVolumeHold();
}

// Reloads the currently-open .s3m from the start -- same reasoning as
// restartModFromTop() (no seekTo(), a fresh load() is the only way to
// correctly reset every channel/sequencer/tempo state at once).
bool restartS3mFromTop() {
    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));
    return s3mPlayer->load(path);
}

void handlePlayS3mInput() {
    if (Input::justPressed(BTN_PLAY)) {
        if (s3mPlayer->state() == S3mPlayer::STATE_PLAYING) {
            s3mPlayer->pause();
            stoppedViaNav = false;
            Ui::updateS3mPlayerState(s3mPlayer->state(), stoppedViaNav);
        } else if (s3mPlayer->state() == S3mPlayer::STATE_PAUSED) {
            s3mPlayer->play();
            stoppedViaNav = false;
            Ui::updateS3mPlayerState(s3mPlayer->state(), stoppedViaNav);
        } else if (s3mPlayer->state() == S3mPlayer::STATE_DONE) {
            if (restartS3mFromTop()) s3mPlayer->play();
            stoppedViaNav = false;
            // A reload resets far more than State at once (Time/Pattern/
            // Row), same "full redraw is the right tool" reasoning as the
            // MOD player's own PLAY-on-STATE_DONE branch.
            needsRedraw = true;
        }
    }
    if (Input::justPressed(BTN_LEFT)) {
        s3mPlayer->stop(); // already closes SD handles internally -- safe to destroy right after
        s3mPlayer->~S3mPlayer(); // not heap-owned (see SharedRamArena) -- no delete
        s3mPlayer = nullptr;
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    // NAV stops playback: reloads to the start and pauses there, shown as
    // "Stopped" -- same stoppedViaNav convention as the other players.
    if (Input::justPressed(BTN_NAV)) {
        if (s3mPlayer->state() == S3mPlayer::STATE_PLAYING ||
            s3mPlayer->state() == S3mPlayer::STATE_PAUSED ||
            s3mPlayer->state() == S3mPlayer::STATE_DONE) {
            restartS3mFromTop();
            stoppedViaNav = true;
            needsRedraw = true;
        }
    }
    // EDIT+UP/DOWN volume, reused as-is -- same reasoning as the MOD
    // screen. No seek (S3mPlayer doesn't support it) and no ALT/MIDI-target.
    handleVolumeHold();
}

// Reloads the currently-open .xm from the start -- same reasoning as
// restartS3mFromTop() (no seekTo(), a fresh load() is the only way to
// correctly reset every channel/sequencer/tempo state at once).
bool restartXmFromTop() {
    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));
    return xmPlayer->load(path);
}

void handlePlayXmInput() {
    if (Input::justPressed(BTN_PLAY)) {
        if (xmPlayer->state() == XmPlayer::STATE_PLAYING) {
            xmPlayer->pause();
            stoppedViaNav = false;
            Ui::updateXmPlayerState(xmPlayer->state(), stoppedViaNav);
        } else if (xmPlayer->state() == XmPlayer::STATE_PAUSED) {
            xmPlayer->play();
            stoppedViaNav = false;
            Ui::updateXmPlayerState(xmPlayer->state(), stoppedViaNav);
        } else if (xmPlayer->state() == XmPlayer::STATE_DONE) {
            if (restartXmFromTop()) xmPlayer->play();
            stoppedViaNav = false;
            // A reload resets far more than State at once (Time/Pattern/
            // Row), same "full redraw is the right tool" reasoning as the
            // MOD/S3M players' own PLAY-on-STATE_DONE branch.
            needsRedraw = true;
        }
    }
    if (Input::justPressed(BTN_LEFT)) {
        xmPlayer->stop(); // already closes SD handles internally -- safe to destroy right after
        xmPlayer->~XmPlayer(); // not heap-owned (see SharedRamArena) -- no delete
        xmPlayer = nullptr;
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    // NAV stops playback: reloads to the start and pauses there, shown as
    // "Stopped" -- same stoppedViaNav convention as the other players.
    if (Input::justPressed(BTN_NAV)) {
        if (xmPlayer->state() == XmPlayer::STATE_PLAYING ||
            xmPlayer->state() == XmPlayer::STATE_PAUSED ||
            xmPlayer->state() == XmPlayer::STATE_DONE) {
            restartXmFromTop();
            stoppedViaNav = true;
            needsRedraw = true;
        }
    }
    // EDIT+UP/DOWN volume, reused as-is -- same reasoning as the MOD/S3M
    // screens. No seek (XmPlayer doesn't support it) and no ALT/MIDI-target.
    handleVolumeHold();
}

// Reloads the currently-open file from the start -- shared by PLAY's
// "restart a just-finished file" and NAV's "stop" (see handlePlayInput()).
// Deliberately does NOT touch tempo scale (a restart keeps whatever speed
// you'd dialed in, unlike a fresh openSelected() open) or auto-play --
// callers that want to keep playing call player->play() themselves
// afterward. Returns true on success (state is now STATE_PAUSED at
// position 0); false means the reload failed and state is now
// STATE_ERROR instead -- callers use this to decide between a targeted
// partial update and a full redraw (STATE_ERROR needs the completely
// different error layout).
bool restartFromTop() {
    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));
    bool ok = player->load(path);
    if (ok) {
        // Unlike tempo, instrument choices aren't a "live user setting"
        // worth preserving across a restart -- reset so they're re-derived
        // cleanly from the file's own Program Change events as it replays
        // from the top.
        Synth::resetPrograms();
    }
    return ok;
}

void handlePlayInput() {
    if (Input::justPressed(BTN_PLAY)) {
        if (player->state() == MidiPlayer::STATE_PLAYING) {
            player->pause();
            stoppedViaNav = false;
            Ui::updatePlayerState(player->state(), stoppedViaNav);
        } else if (player->state() == MidiPlayer::STATE_PAUSED) {
            player->play();
            stoppedViaNav = false;
            Ui::updatePlayerState(player->state(), stoppedViaNav);
        } else if (player->state() == MidiPlayer::STATE_DONE) {
            // selectedIndex/browser haven't changed since this file was
            // opened (nothing mutates them while in APP_PLAY), so
            // restartFromTop() still points at the same entry.
            if (restartFromTop()) player->play();
            stoppedViaNav = false;
            // A restart resets far more than State (Time, Tempo, possibly
            // Tracks) all at once, and a failed reload switches to the
            // completely different STATE_ERROR layout -- a full redraw is
            // the right tool for either, unlike the simple pause/resume
            // partial update.
            needsRedraw = true;
        }
    }
    // ENTER held excludes this -- ENTER+LEFT is scrub-back (see
    // handleSeekHold()), not "back to browser".
    if (Input::justPressed(BTN_LEFT) && !Input::isDown(BTN_ENTER)) {
        player->stop(); // already closes SD handles internally -- safe to delete right after
        delete player;
        player = nullptr;
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    // NAV stops playback: resets straight to the start and pauses there
    // (shown as "Stopped", not "Paused" -- see stoppedViaNav), rather
    // than freezing wherever it happened to be. LEFT above is the "back
    // to browser" gesture, consistent with every other screen. Only
    // Time/Tempo/State actually change here, so this stays a partial
    // update via updatePlayerLive()/updatePlayerState() -- unless the
    // reload itself fails, which switches to the completely different
    // STATE_ERROR layout and needs a full redraw instead. Silence
    // whatever's currently sounding first: unlike pause(), a reload's
    // internal close() doesn't send an all-notes-off, so a note held at
    // the moment of a PLAYING->reload transition would otherwise never
    // get its note-off.
    if (Input::justPressed(BTN_NAV)) {
        if (player->state() == MidiPlayer::STATE_PLAYING ||
            player->state() == MidiPlayer::STATE_PAUSED ||
            player->state() == MidiPlayer::STATE_DONE) {
            MidiOutput::allNotesOffAllChannels();
            if (restartFromTop()) {
                stoppedViaNav = true;
                Ui::updatePlayerState(player->state(), stoppedViaNav);
                Ui::updatePlayerLive(*player);
            } else {
                stoppedViaNav = false;
                needsRedraw = true;
            }
        }
    }
    // STATE_ERROR replaces this whole row (and everything below State)
    // with the error message -- see drawPlayer() -- so a partial update
    // isn't safe there; fall back to a full redraw in that case.
    //
    // ALT is a tap-vs-hold modifier here, same disambiguation as EDIT just
    // below: a bare tap cycles the MIDI output target, but ALT held + EDIT
    // (tap) toggles Audio instead (see the combo check just after EDIT's
    // own tap-vs-hold split) -- the cycle itself is deferred to
    // justReleased, gated on altUsedAsModifier, so a press that turns into
    // that combo doesn't ALSO cycle the output target for the same
    // physical press.
    if (Input::justPressed(BTN_ALT)) {
        altUsedAsModifier = false;
    } else if (Input::justReleased(BTN_ALT)) {
        if (!altUsedAsModifier) {
            cycleOutputTarget();
            if (player->state() == MidiPlayer::STATE_ERROR) needsRedraw = true;
            else Ui::updatePlayerOutputTarget(outputTarget);
        }
    }
    // EDIT is a tap-vs-hold modifier here, same disambiguation LooperMode
    // uses for its own Metro/Count-In rows: tap opens the pariSynth quick-
    // edit overlay, but only if this press wasn't also used as a hold-
    // modifier for Volume (handleVolumeHold()), Scrub (handleSeekHold()),
    // or ALT+EDIT (below, which toggles Audio instead).
    if (Input::justPressed(BTN_EDIT)) {
        editUsedAsModifier = false;
    } else if (Input::justReleased(BTN_EDIT)) {
        if (!editUsedAsModifier) {
            // See APP_SYNTH_EDIT's own case in update(), which keeps
            // player->update() ticking underneath so playback doesn't
            // stall while the editor has input/draw focus.
            appState = APP_SYNTH_EDIT;
            synthEditShowingGrid = true;
            PariSynthGrid::enter();
            needsRedraw = true;
        }
    }
    // ALT held + EDIT (tap): toggles Audio on/off, replacing EDIT's own
    // bare-tap meaning above for this press. Marks both modifiers "used"
    // so releasing either afterward doesn't also cycle the output target
    // or open the editor for this same physical press.
    if (Input::isDown(BTN_ALT) && Input::justPressed(BTN_EDIT)) {
        altUsedAsModifier = true;
        editUsedAsModifier = true;
        toggleAudio();
        if (player->state() == MidiPlayer::STATE_ERROR) needsRedraw = true;
        else Ui::updatePlayerAudioState(audioOn);
    }
    handleTempoHold();
    handleVolumeHold();
    handleSeekHold();
}

// Repaints only the two affected rows instead of the whole screen.
void moveMenuCursor(int newCursor) {
    if (newCursor == menuCursor) return;
    int prev = menuCursor;
    menuCursor = newCursor;
    const char* labels[7];
    for (int i = 0; i < menuEntryCount; i++) labels[i] = menuEntries[i].label;
    Ui::updateEntryMenuSelection(labels, menuEntryCount, prev, menuCursor);
}

void handleEntryMenuInput() {
    if (Input::justPressed(BTN_UP)) {
        moveMenuCursor(menuCursor > 0 ? menuCursor - 1 : menuEntryCount - 1); // wraps to the bottom
    }
    if (Input::justPressed(BTN_DOWN)) {
        moveMenuCursor(menuCursor < menuEntryCount - 1 ? menuCursor + 1 : 0); // wraps to the top
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        switch (menuEntries[menuCursor].action) {
            case MENU_OPEN:            openSelected(); break;
            case MENU_RENAME:          beginRename(); break;
            case MENU_DELETE:          beginConfirmDelete(); break;
            case MENU_LOAD_TO_LOOPER:  beginLoopTrackPick(); break;
            case MENU_NEW_RECORDING:   beginNewRecordingName(); break;
            case MENU_CAPTURE_SYSEX:   beginNewSysExCaptureName(); break;
            case MENU_NEW_FOLDER:      beginNewFolderName(); break;
        }
    }
}

void handleNameEntryInput() {
    if (Input::justPressed(BTN_LEFT))  moveKeyboardCursor(0, -1);
    if (Input::justPressed(BTN_RIGHT)) moveKeyboardCursor(0, 1);
    if (Input::justPressed(BTN_UP))    moveKeyboardCursor(-1, 0);
    if (Input::justPressed(BTN_DOWN))  moveKeyboardCursor(1, 0);

    if (Input::justPressed(BTN_NAV)) {
        appState = APP_BROWSE;
        needsRedraw = true;
    }

    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        const KeyDef& key = KEYBOARD_ROWS[nameEntryKeyRow][nameEntryKeyCol];
        bool hadError = (nameEntryError[0] != '\0');
        nameEntryError[0] = '\0'; // editing clears a stale "already exists" etc.
        switch (key.kind) {
            case KEY_CHAR:
                if (nameEntryLen < NAME_ENTRY_MAX_LEN) {
                    nameEntryBuf[nameEntryLen++] = key.ch;
                    nameEntryBuf[nameEntryLen] = '\0';
                    Ui::updateNameEntryPreview(nameEntryBuf, currentNameSuffix());
                    if (hadError) Ui::updateNameEntryError(nullptr);
                }
                break;
            case KEY_DEL:
                if (nameEntryLen > 0) {
                    nameEntryBuf[--nameEntryLen] = '\0';
                    Ui::updateNameEntryPreview(nameEntryBuf, currentNameSuffix());
                    if (hadError) Ui::updateNameEntryError(nullptr);
                }
                break;
            case KEY_OK:
                finishNameEntry();
                break;
        }
    }
}

void handleConfirmDeleteInput() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        appState = APP_BROWSE;
        needsRedraw = true;
        return;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (deleteFailed) {
            appState = APP_BROWSE;
            needsRedraw = true;
            return;
        }

        const BrowserEntry& e = browser.entry(selectedIndex);
        char path[224];
        browser.buildFullPath(selectedIndex, path, sizeof(path));
        bool ok = e.isDir ? sd.rmdir(path) : sd.remove(path);

        if (ok) {
            browser.refresh();
            int count = browser.entryCount();
            if (selectedIndex >= count) selectedIndex = count - 1;
            if (selectedIndex < 0) selectedIndex = 0;
            scrollOffset = 0;
            ensureSelectionVisible();
            appState = APP_BROWSE;
        } else {
            deleteFailed = true;
        }
        needsRedraw = true;
    }
}

// Confirming a track here doesn't perform the import itself -- it just
// records the request and lets main.cpp's next tick see
// hasPendingLooperImport() and hand off to LooperMode, which is the only
// place that can check whether the destination track already has content
// worth warning about (see LooperMode::requestImport()).
void handleLooperTrackPickInput() {
    if (Input::justPressed(BTN_UP)) {
        if (looperTrackPickCursor > 0) {
            int prev = looperTrackPickCursor;
            looperTrackPickCursor--;
            Ui::updateEntryMenuSelection(LOOPER_TRACK_LABELS, 4, prev, looperTrackPickCursor);
        }
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (looperTrackPickCursor < 3) {
            int prev = looperTrackPickCursor;
            looperTrackPickCursor++;
            Ui::updateEntryMenuSelection(LOOPER_TRACK_LABELS, 4, prev, looperTrackPickCursor);
        }
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        pendingLooperImportTrack = looperTrackPickCursor;
        pendingLooperImport = true;
    }
}

// See APP_FREE_LOOPER_RAM's declaration. All the actual UP/DOWN/ENTER/NAV
// handling and drawing lives in LooperMode (it owns tracks[]/saveSession())
// -- this just forwards input to it and acts on the result: DONE re-enters
// openSelected() as the retry (tracks is null by then, so this time the
// MOD/S3M branch falls straight through instead of gating again);
// CANCELLED goes back to the browser with the file never opened.
void handleFreeLooperRamGateInput() {
    switch (LooperMode::updateFreeTracksForFilePlayer()) {
        case LooperMode::FREE_TRACKS_DONE:
            openSelected();
            break;
        case LooperMode::FREE_TRACKS_CANCELLED:
            appState = APP_BROWSE;
            needsRedraw = true;
            break;
        case LooperMode::FREE_TRACKS_PENDING:
            break;
    }
}

void handleRecordingInput() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        recorder.stop();
        browser.refresh(); // so the new file shows up in the list
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    // EDIT cancels instead of saving -- skip the confirm entirely if
    // nothing's actually been captured yet (STATE_ARMED, or STATE_
    // RECORDING with zero events), since there's nothing to lose.
    if (Input::justPressed(BTN_EDIT)) {
        if (recorder.eventCount() == 0) {
            recorder.cancel();
            appState = APP_BROWSE;
            needsRedraw = true;
        } else {
            appState = APP_CONFIRM_CANCEL_RECORDING;
            needsRedraw = true;
        }
    }
}

void handleConfirmCancelRecordingInput() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        recorder.cancel();
        browser.refresh(); // in case a partial file had already been created and just got deleted
        appState = APP_BROWSE;
        needsRedraw = true;
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        // Back to the recording screen -- still recording, nothing lost.
        appState = APP_RECORDING;
        needsRedraw = true;
    }
}

// Same shape as handleRecordingInput()/handleConfirmCancelRecordingInput()
// above -- NAV/LEFT stops & saves, EDIT cancels (skipping the confirm if
// nothing's arrived yet).
void handleCapturingSysExInput() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        sysexRecorder.stop();
        browser.refresh(); // so the new file shows up in the list
        appState = APP_BROWSE;
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_EDIT)) {
        if (sysexRecorder.messageCount() == 0) {
            sysexRecorder.cancel();
            appState = APP_BROWSE;
            needsRedraw = true;
        } else {
            appState = APP_CONFIRM_CANCEL_SYSEX_CAPTURE;
            needsRedraw = true;
        }
    }
}

void handleConfirmCancelSysExCaptureInput() {
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        sysexRecorder.cancel();
        browser.refresh(); // in case a partial file had already been created and just got deleted
        appState = APP_BROWSE;
        needsRedraw = true;
    } else if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        // Back to the capture screen -- still capturing, nothing lost.
        appState = APP_CAPTURING_SYSEX;
        needsRedraw = true;
    }
}

// One-shot "send the dump" screen -- unlike the regular MIDI player there's
// no pause/resume/tempo/output-target to control, just stop-and-go-back.
void handlePlaySysExInput() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        sysexPlayer.close();
        appState = APP_BROWSE;
        needsRedraw = true;
    }
}

} // namespace

void begin() {
    browser.begin();
}

void enter() {
    // (Re)claim the shared MIDI input hook and MidiOutput/Synth settings
    // -- only this mode uses them today, but claiming them here (rather
    // than once in begin()) is what lets a future mode (the looper) take
    // them over instead whenever it's the active one.
    MidiOutput::setInputHandler([](uint8_t status, uint8_t d1, uint8_t d2, uint8_t len) {
        recorder.feed(status, d1, d2, len);
        if (recorder.state() == MidiRecorder::STATE_RECORDING) {
            pushMidiLogEntry({false, status, d1, d2, len, 0});
        }
    });
    // Both feed() calls below self-guard on their own ARMED/RECORDING
    // state (same as the channel-voice handler above), so it's safe to
    // register both unconditionally rather than switching on appState --
    // recorder and sysexRecorder are never both active at once (only one
    // of APP_RECORDING/APP_CAPTURING_SYSEX can be current), and this is
    // also what makes Capture SysEx Dump naturally "not respond to
    // standard MIDI": recorder just stays idle while sysexRecorder is the
    // one actually armed.
    MidiOutput::setSysExHandler([](const uint8_t* data, size_t len) {
        recorder.feedSysEx(data, len);
        sysexRecorder.feed(data, len);
        // Both recorders self-guard on their own ARMED/RECORDING state
        // (see the comment below), and only one of APP_RECORDING/
        // APP_CAPTURING_SYSEX is ever current, so at most one of these is
        // ever true -- same log/gate either way.
        if (recorder.state() == MidiRecorder::STATE_RECORDING ||
            sysexRecorder.state() == SysExRecorder::STATE_RECORDING) {
            pushMidiLogEntry({true, 0, 0, 0, 0, (uint16_t)len});
        }
    });
    MidiOutput::setTarget(outputTarget);
    // Seed from the Settings default exactly once, same "don't clobber a
    // live in-session toggle" reasoning as volumeSeededFromSettings below
    // -- someone who's flipped Audio on/off with EDIT this session should
    // keep that choice switching to Looper/Settings and back, not have it
    // silently reset to the Settings default every re-entry.
    if (!audioSeededFromSettings) {
        audioOn = SettingsMode::synthAudioEnabled();
        audioSeededFromSettings = true;
    }
    MidiOutput::setAudioOutput(audioOn);
    // Output Level/Reverb/LFO Rate/LFO Voices/Stereo Spread have no in-
    // session live-adjust control of their own (unlike Volume, see
    // handleVolumeHold()), so they're simply re-applied from Settings
    // every entry -- always current, nothing to protect.
    Synth::setOutputLevel((uint8_t)SettingsMode::defaultOutputLevel());
    Synth::setReverbEnabled(SettingsMode::reverbEnabled());
    Synth::setReverbMix((uint8_t)SettingsMode::reverbMix());
    Synth::setReverbType((uint8_t)SettingsMode::reverbType());
    Synth::setLfoRateTenthsHz((uint16_t)SettingsMode::lfoRateTenthsHz());
    Synth::setLfoVoices((uint8_t)SettingsMode::lfoVoices());
    Synth::setStereoSpread((uint8_t)SettingsMode::stereoSpreadPercent());
    // Seed from the Settings default exactly once (the first time this
    // mode is entered after boot), not on every re-entry -- otherwise
    // switching to Looper/Settings and back would silently yank any
    // in-session volume adjustment back to the default, the same "don't
    // clobber a live value" reasoning LooperMode's own defaults use (see
    // settings_mode.h's header comment).
    if (!volumeSeededFromSettings) {
        volume = SettingsMode::defaultVolume();
        volumeSeededFromSettings = true;
    }
    Synth::setVolume((uint8_t)volume);

    // Normally the browser's list just stays valid across a mode switch
    // (nothing else touches the SD card while we're away) -- but
    // freeBrowserBuffers() may have silently dropped it in the meantime
    // (see its own comment), so rebuild here if that happened rather than
    // showing an incorrectly-empty browser. A no-op the vast majority of
    // the time (needsRebuild() only true right after that reclaim).
    if (browser.needsRebuild()) browser.refresh();

    appState = APP_BROWSE;
    needsRedraw = true;
}

bool update() {
    bool exitRequested = false;

    // MidiPlayer::update() paces itself off wall-clock time (micros()
    // comparisons against each event's own scheduled time), not a tick
    // counter -- so ticking it here, unconditionally before the state
    // switch below, rather than nested inside APP_PLAY's own case, keeps
    // playback exactly on schedule even while APP_SYNTH_EDIT has input/
    // draw focus instead. This is the only structural change needed to
    // let a file keep playing in the background while the user edits an
    // instrument mid-song (see APP_SYNTH_EDIT's own case below).
    // wavPlayer/modPlayer/s3mPlayer/xmPlayer are untouched -- none of
    // them drive Synth::noteOn/programChange at all (they render their
    // own audio directly), so APP_SYNTH_EDIT is only ever reached from
    // APP_PLAY (see the AppState enum's own comment on why).
    if (appState == APP_PLAY || appState == APP_SYNTH_EDIT) {
        player->update();
    }

    switch (appState) {
        case APP_BROWSE:
            exitRequested = handleBrowseInput();
            break;

        case APP_PLAY: {
            // player->update() already ticked above.
            // Once the file finishes, keep showing the final "Finished"
            // screen until the user presses a button to go back.
            handlePlayInput();

            // Elapsed time and the note-activity strip change continuously
            // while playing. Rather than trigger a full-screen needsRedraw,
            // repaint just those parts directly at a low fixed rate.
            static uint32_t lastPlayTick = 0;
            uint32_t now = millis();
            if (player->state() == MidiPlayer::STATE_PLAYING && now - lastPlayTick >= 50) {
                lastPlayTick = now;
                Ui::updatePlayerLive(*player);
            }
            break;
        }

        case APP_SYNTH_EDIT: {
            if (synthEditShowingGrid) {
                // Own input/redraw here, same shape as APP_PLAY's own
                // handling just above -- see PariSynthGrid::update().
                PariSynthGrid::Result result = PariSynthGrid::update(volume);
                if (result == PariSynthGrid::RESULT_OPEN_EDITOR) {
                    synthEditShowingGrid = false;
                    break;
                }
                if (result == PariSynthGrid::RESULT_EXIT) {
                    // Deliberately no MidiOutput::allNotesOffAllChannels()
                    // here, unlike PariSynthMode's own exit -- the file is
                    // still playing underneath this overlay, and closing
                    // it must not silence its notes. See pari_synth_grid.h's
                    // own header comment.
                    appState = APP_PLAY;
                    needsRedraw = true;
                    break;
                }
                static uint32_t lastSynthEditLiveMs = 0;
                uint32_t now = millis();
                if (now - lastSynthEditLiveMs >= 50) {
                    lastSynthEditLiveMs = now;
                    Ui::updatePariSynthPlayLive();
                    // Unlike PariSynthMode's own grid, nothing here calls
                    // back into this screen as the file's notes start/stop
                    // (MidiPlayer just drives MidiOutput::sendNoteOn/
                    // sendNoteOff on its own schedule, with no hook into
                    // whatever UI happens to be showing) -- so this poll is
                    // the only thing keeping each channel's glow current
                    // while the file keeps playing underneath. Diffed
                    // against the mask from last tick so only the channels
                    // that actually changed get repainted, not all 16.
                    static uint16_t lastSynthEditActiveMask = 0;
                    uint16_t activeMask = MidiOutput::activeChannelMask();
                    if (activeMask != lastSynthEditActiveMask) {
                        uint16_t changed = activeMask ^ lastSynthEditActiveMask;
                        for (int ch = 0; ch < 16; ch++) {
                            if (changed & (1u << ch)) {
                                Ui::updatePariSynthChannelCell(ch, ch == PariSynthGrid::selectedChannel(), (activeMask & (1u << ch)) != 0);
                            }
                        }
                        lastSynthEditActiveMask = activeMask;
                    }
                }
            } else {
                // SynthEditor::update() handles its own input and redraws
                // itself every call (see synth_editor.h) -- resume the
                // channel grid once it reports done, same wrapping
                // PariSynthMode's own play screen does around SynthEditor.
                if (SynthEditor::update()) {
                    synthEditShowingGrid = true;
                    needsRedraw = true;
                }
            }
            break;
        }

        case APP_PLAY_WAV: {
            wavPlayer.update();
            handlePlayWavInput();

            static uint32_t lastWavPlayTick = 0;
            uint32_t now = millis();
            if (wavPlayer.state() == WavPlayer::STATE_PLAYING && now - lastWavPlayTick >= 50) {
                lastWavPlayTick = now;
                Ui::updateWavPlayerLive(wavPlayer);
            }
            break;
        }

        case APP_PLAY_MOD: {
            modPlayer->update();
            handlePlayModInput();

            // 200ms, not 50ms -- see the identical change (and the
            // real-hardware measurement behind it) on APP_PLAY_S3M below.
            static uint32_t lastModPlayTick = 0;
            uint32_t now = millis();
            if (modPlayer->state() == ModPlayer::STATE_PLAYING && now - lastModPlayTick >= 200) {
                lastModPlayTick = now;
                Ui::updateModPlayerLive(*modPlayer);
            }
            break;
        }

        case APP_PLAY_S3M: {
            s3mPlayer->update();
            handlePlayS3mInput();

            // 200ms, not 50ms -- a busy S3M passage's own mixing cost
            // leaves little headroom for a frequent SPI redraw on top of
            // it; a "Row: N" counter updating 5x/sec instead of 20x/sec
            // is imperceptible, so widening the throttle is free headroom.
            static uint32_t lastS3mPlayTick = 0;
            uint32_t now = millis();
            if (s3mPlayer->state() == S3mPlayer::STATE_PLAYING && now - lastS3mPlayTick >= 200) {
                lastS3mPlayTick = now;
                Ui::updateS3mPlayerLive(*s3mPlayer);
            }
            break;
        }

        case APP_PLAY_XM: {
            xmPlayer->update();
            handlePlayXmInput();

            // 200ms, not 50ms -- see the identical change (and the
            // real-hardware measurement behind it) on APP_PLAY_S3M above.
            static uint32_t lastXmPlayTick = 0;
            uint32_t now = millis();
            if (xmPlayer->state() == XmPlayer::STATE_PLAYING && now - lastXmPlayTick >= 200) {
                lastXmPlayTick = now;
                Ui::updateXmPlayerLive(*xmPlayer);
            }
            break;
        }

        case APP_PLAY_SYSEX: {
            sysexPlayer.update();
            handlePlaySysExInput();

            static uint32_t lastSysExPlayTick = 0;
            uint32_t now = millis();
            if (sysexPlayer.state() == SysExPlayer::STATE_SENDING && now - lastSysExPlayTick >= 100) {
                lastSysExPlayTick = now;
                Ui::updateSysExPlayerLive(sysexPlayer);
            }
            // STATE_SENDING -> STATE_DONE/STATE_ERROR happens inside
            // update() above, so catch it the same way APP_RECORDING
            // catches its ARMED->RECORDING transition -- otherwise the
            // "Sending" state text (and STATE_ERROR's different layout)
            // would never update once the file actually finishes.
            static SysExPlayer::State lastSysExPlayState = SysExPlayer::STATE_IDLE;
            if (sysexPlayer.state() != lastSysExPlayState) {
                lastSysExPlayState = sysexPlayer.state();
                needsRedraw = true;
            }
            break;
        }

        case APP_ENTRY_MENU:
            handleEntryMenuInput();
            break;

        case APP_NAME_ENTRY:
            handleNameEntryInput();
            break;

        case APP_CONFIRM_OVERWRITE:
            handleConfirmOverwriteInput();
            break;

        case APP_CONFIRM_DELETE:
            handleConfirmDeleteInput();
            break;

        case APP_RECORDING: {
            handleRecordingInput();

            // The ARMED -> RECORDING (or -> ERROR) transition happens
            // inside the input-handler callback, asynchronously to this
            // loop, so catch it here and force a full redraw -- otherwise
            // the "Waiting for input" state text would never update once
            // the first MIDI byte actually arrives.
            static MidiRecorder::State lastRecorderState = MidiRecorder::STATE_IDLE;
            if (recorder.state() != lastRecorderState) {
                lastRecorderState = recorder.state();
                needsRedraw = true;
            }

            // Elapsed time, event count, and the activity dot change
            // continuously while recording; repaint just those at a low
            // fixed rate rather than a full needsRedraw. 100ms rather than
            // a slower rate so the activity dot (a RECORDING_ACTIVITY_MS =
            // 250ms blip, see ui.cpp) is sampled often enough to actually
            // show up.
            static uint32_t lastRecTick = 0;
            uint32_t now = millis();
            if (recorder.state() == MidiRecorder::STATE_RECORDING && now - lastRecTick >= 100) {
                lastRecTick = now;
                Ui::updateRecordingLive(recorder);
                // Only when something new actually arrived -- most 100ms
                // ticks during a quiet stretch have nothing to redraw, and
                // skipping the call skips its SPI cost entirely.
                if (midiLogDirty) {
                    midiLogDirty = false;
                    Ui::updateRecordingLog(midiLog, midiLogCount);
                }
            }
            break;
        }

        case APP_CONFIRM_CANCEL_RECORDING:
            handleConfirmCancelRecordingInput();
            break;

        case APP_CAPTURING_SYSEX: {
            handleCapturingSysExInput();

            // Same reasoning as APP_RECORDING above: the ARMED->RECORDING
            // (or ->ERROR) transition happens inside the SysEx input-
            // handler callback, asynchronously to this loop.
            static SysExRecorder::State lastSysExRecState = SysExRecorder::STATE_IDLE;
            if (sysexRecorder.state() != lastSysExRecState) {
                lastSysExRecState = sysexRecorder.state();
                needsRedraw = true;
            }

            // Faster than updateRecordingLive()'s 200ms -- the activity
            // dot's whole point is tracking short-lived (SYSEX_ACTIVITY_MS)
            // blips, so it needs to be sampled often enough to catch them.
            static uint32_t lastSysExRecTick = 0;
            uint32_t now = millis();
            if (sysexRecorder.state() == SysExRecorder::STATE_RECORDING && now - lastSysExRecTick >= 100) {
                lastSysExRecTick = now;
                Ui::updateSysExCaptureLive(sysexRecorder);
                if (midiLogDirty) {
                    midiLogDirty = false;
                    Ui::updateRecordingLog(midiLog, midiLogCount);
                }
            }
            break;
        }

        case APP_CONFIRM_CANCEL_SYSEX_CAPTURE:
            handleConfirmCancelSysExCaptureInput();
            break;

        case APP_LOOPER_TRACK_PICK:
            handleLooperTrackPickInput();
            break;

        case APP_FREE_LOOPER_RAM:
            handleFreeLooperRamGateInput();
            break;
    }

    if (exitRequested) return true;

    if (needsRedraw) {
        needsRedraw = false;
        switch (appState) {
            case APP_BROWSE:
                Ui::drawBrowser(browser, selectedIndex, scrollOffset);
                break;

            case APP_PLAY:
                Ui::drawPlayer(nowPlayingName, *player, outputTarget, audioOn, volume, stoppedViaNav);
                break;

            case APP_SYNTH_EDIT:
                // Only the channel-grid half of this state draws through
                // needsRedraw -- while SynthEditor has focus instead
                // (!synthEditShowingGrid), it redraws itself internally
                // and this flag is never set for this state at all.
                // activeChannelMask() reflects the file's own notes here
                // (MidiPlayer plays them via MidiOutput::sendNoteOn/
                // sendNoteOff, which feed the same per-note bookkeeping
                // isChannelActive() reads) -- see the live-tick above for
                // why this alone isn't enough to keep it current while the
                // file keeps playing underneath this screen. No explicit
                // footer args -- both entry points into this screen now
                // share one default (see drawPariSynthPlay()'s own
                // comment), which is itself what keeps the footer from
                // silently drifting out of sync between them again.
                Ui::drawPariSynthPlay(PariSynthGrid::selectedChannel(), MidiOutput::activeChannelMask());
                if (PariSynthGrid::volumeOverlayVisible()) Ui::updatePariSynthVolumeOverlay(volume);
                break;

            case APP_PLAY_SYSEX:
                Ui::drawSysExPlayer(nowPlayingName, sysexPlayer);
                break;

            case APP_PLAY_WAV:
                Ui::drawWavPlayer(nowPlayingName, wavPlayer, volume, stoppedViaNav);
                break;

            case APP_PLAY_MOD:
                Ui::drawModPlayer(nowPlayingName, *modPlayer, volume, stoppedViaNav);
                break;

            case APP_PLAY_S3M:
                Ui::drawS3mPlayer(nowPlayingName, *s3mPlayer, volume, stoppedViaNav);
                break;

            case APP_PLAY_XM:
                Ui::drawXmPlayer(nowPlayingName, *xmPlayer, volume, stoppedViaNav);
                break;

            case APP_ENTRY_MENU: {
                const char* labels[7];
                for (int i = 0; i < menuEntryCount; i++) labels[i] = menuEntries[i].label;
                Ui::drawEntryMenu(menuSubtitle, labels, menuEntryCount, menuCursor);
                break;
            }

            case APP_NAME_ENTRY: {
                const char* title = (namePurpose == NAME_NEW_RECORDING)     ? "New Recording"
                                   : (namePurpose == NAME_NEW_SYSEX_CAPTURE) ? "Capture SysEx Dump"
                                   : (namePurpose == NAME_NEW_FOLDER)       ? "New Folder"
                                                                             : "Rename";
                Ui::drawNameEntry(title, nameEntryBuf, currentNameSuffix(),
                                   nameEntryError[0] ? nameEntryError : nullptr,
                                   nameEntryKeyRow, nameEntryKeyCol);
                break;
            }

            case APP_CONFIRM_OVERWRITE: {
                // pendingOverwritePath is a full absolute path -- just the
                // filename/folder-name portion after the last '/' is what
                // drawConfirmDelete()'s own `name` convention expects too.
                const char* slash = strrchr(pendingOverwritePath, '/');
                const char* name = slash ? slash + 1 : pendingOverwritePath;
                Ui::drawConfirmOverwrite(name, pendingOverwriteIsDir);
                break;
            }

            case APP_CONFIRM_DELETE: {
                bool valid = (selectedIndex >= 0 && selectedIndex < browser.entryCount());
                const BrowserEntry& e = browser.entry(valid ? selectedIndex : 0);
                Ui::drawConfirmDelete(valid ? e.name : "", valid && e.isDir, deleteFailed);
                break;
            }

            case APP_RECORDING:
                Ui::drawRecording(nowRecordingName, recorder, midiLog, midiLogCount);
                midiLogDirty = false; // just drawn above, nothing new to catch on the next tick
                break;

            case APP_CONFIRM_CANCEL_RECORDING:
                // Deliberately avoids "NAV cancel" here (the usual hint
                // line elsewhere) -- on a screen that's itself about
                // canceling something, that reads ambiguously as "NAV
                // cancels the recording" rather than its actual meaning,
                // "back out of this confirmation".
                Ui::drawMessage("Discard this recording?", "ENTER discard  NAV keep recording");
                break;

            case APP_CAPTURING_SYSEX:
                Ui::drawSysExCapture(nowCapturingSysExName, sysexRecorder, midiLog, midiLogCount);
                midiLogDirty = false; // just drawn above, nothing new to catch on the next tick
                break;

            case APP_CONFIRM_CANCEL_SYSEX_CAPTURE:
                // Same reasoning as APP_CONFIRM_CANCEL_RECORDING above.
                Ui::drawMessage("Discard this capture?", "ENTER discard  NAV keep capturing");
                break;

            case APP_LOOPER_TRACK_PICK:
                Ui::drawEntryMenu("Load into Track", LOOPER_TRACK_LABELS, 4, looperTrackPickCursor);
                break;

            case APP_FREE_LOOPER_RAM:
                // Shouldn't normally fire -- openSelected() already draws
                // this synchronously via beginFreeTracksForFilePlayer() and
                // leaves needsRedraw false for this state (see its own
                // comment) -- but redraw defensively rather than leave a
                // blank screen if something upstream ever sets needsRedraw
                // while this state is active.
                LooperMode::beginFreeTracksForFilePlayer();
                break;
        }
    }

    return false;
}

bool hasPendingLooperImport() { return pendingLooperImport; }

void consumeLooperImport(char* pathOut, size_t pathOutSize, int& trackIndexOut) {
    strncpy(pathOut, looperImportPath, pathOutSize - 1);
    pathOut[pathOutSize - 1] = '\0';
    trackIndexOut = pendingLooperImportTrack;
    pendingLooperImport = false;
}

void freeBrowserBuffers() {
    browser.freeBuffers();
}

} // namespace FilePlayerMode

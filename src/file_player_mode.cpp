#include "file_player_mode.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sd_card.h"
#include "sd_browser.h"
#include "midi_file.h"
#include "midi_output.h"
#include "midi_recorder.h"
#include "sysex_recorder.h"
#include "sysex_player.h"
#include "synth.h"
#include "input.h"
#include "ui.h"
#include "keyboard_layout.h"

namespace FilePlayerMode {
namespace {

enum AppState {
    APP_BROWSE,
    APP_PLAY,
    APP_PLAY_SYSEX,   // sending a .syx dump back out (see openSelected())
    APP_ENTRY_MENU,   // Open/Rename/Delete/Load to Looper/New Recording/Capture SysEx Dump/New Folder
    APP_NAME_ENTRY,   // shared on-screen-keyboard editor for the actions above
    APP_CONFIRM_DELETE,
    APP_RECORDING,
    APP_CONFIRM_CANCEL_RECORDING, // "cancel recording? unsaved progress will be lost"
    APP_CAPTURING_SYSEX,          // waiting for/capturing an incoming .syx dump
    APP_CONFIRM_CANCEL_SYSEX_CAPTURE, // same as APP_CONFIRM_CANCEL_RECORDING, for a SysEx capture
    APP_LOOPER_TRACK_PICK, // "Load to Looper": pick which of the 4 tracks to load the selected .mid into
};

enum NamePurpose { NAME_NEW_RECORDING, NAME_NEW_SYSEX_CAPTURE, NAME_NEW_FOLDER, NAME_RENAME };

enum MenuAction { MENU_OPEN, MENU_RENAME, MENU_DELETE, MENU_LOAD_TO_LOOPER, MENU_NEW_RECORDING,
                   MENU_CAPTURE_SYSEX, MENU_NEW_FOLDER };
struct MenuEntry { MenuAction action; const char* label; };

const char* const LOOPER_TRACK_LABELS[4] = { "Track 1", "Track 2", "Track 3", "Track 4" };

const int NAME_ENTRY_MAX_LEN = 16;

SdBrowser browser;
MidiPlayer player;
MidiRecorder recorder;
SysExPlayer sysexPlayer;
SysExRecorder sysexRecorder;

AppState appState = APP_BROWSE;
int selectedIndex = 0;
int scrollOffset = 0;
bool needsRedraw = true;

// -- device-wide MIDI/audio settings, shown/adjusted from the player
// screen. Currently only meaningful to this mode (nothing else uses
// MidiOutput/Synth yet); if the looper ends up needing its own MIDI
// out/audio monitoring, these likely want to move up to main.cpp as
// genuinely shared, mode-independent settings.
MidiOutTarget outputTarget = MIDI_OUT_BOTH;
bool audioOn = false; // onboard synth on/off, see MidiOutput::setAudioOutput()
int volume = 80; // percent, see Synth::setVolume(); mirrored here so the UI can show it

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

// -- shared name-entry editor state (New Recording / New Folder / Rename) --
NamePurpose namePurpose = NAME_NEW_RECORDING;
char nameEntryBuf[NAME_ENTRY_MAX_LEN + 1] = {0}; // editable base name, no extension
int nameEntryLen = 0;         // current length of nameEntryBuf (append/backspace at the end)
int nameEntryKeyRow = 1;      // on-screen keyboard cursor -- deliberately NOT reset per
int nameEntryKeyCol = 0;      // keystroke, only when a new name-entry session begins
char nameEntryError[40] = {0};
char renameOldPath[224] = {0}; // NAME_RENAME only: absolute path being renamed
bool renameIsDir = false;      // NAME_RENAME only
bool renameIsSysEx = false;    // NAME_RENAME only, meaningless if renameIsDir -- .syx vs .mid extension

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
        default:                outputTarget = MIDI_OUT_HARDWARE; break;
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
        browser.enterDir(selectedIndex);
        selectedIndex = 0;
        scrollOffset = 0;
        appState = APP_BROWSE;
        needsRedraw = true;
        return;
    }

    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));

    // nowPlayingName is shared by APP_PLAY and APP_PLAY_SYSEX (Ui::drawPlayer()
    // and Ui::drawSysExPlayer() respectively) -- the two states are mutually
    // exclusive, so there's no risk of one clobbering the other's in-use name.
    strncpy(nowPlayingName, e.name, sizeof(nowPlayingName) - 1);
    nowPlayingName[sizeof(nowPlayingName) - 1] = '\0';

    if (e.isSysEx) {
        sysexPlayer.load(path); // failure surfaces via sysexPlayer.state() on the playback screen
        appState = APP_PLAY_SYSEX;
        needsRedraw = true;
        return;
    }

    stoppedViaNav = false; // a fresh open is never "stopped", see its own comment

    if (player.load(path)) {
        // A genuinely fresh open (as opposed to restarting a just-finished
        // file via PLAY, see handlePlayInput()) starts at the file's own
        // tempo, not whatever was last dialed in, and with a clean synth
        // instrument slate so the previous file's Program Change choices
        // don't bleed into this one before it sends its own.
        player.setTempoScale(1.0f);
        Synth::resetPrograms();
        player.play();
    }
    // On failure, player.state() == STATE_ERROR and drawPlayer() shows
    // player.errorMessage() -- still switch screens so the user sees it.
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
        if (e.isDir || !e.isSysEx) continue;
        if (strncasecmp(e.name, "DUMP", 4) != 0) continue;
        int num = atoi(e.name + 4);
        if (num >= nextNum) nextNum = num + 1;
    }
    snprintf(out, outSize, "DUMP%03d", nextNum);
}

// Loads `initial` (truncated to NAME_ENTRY_MAX_LEN) into the shared
// editor buffer. The on-screen keyboard cursor resets to a fixed start
// point ('Q') for each new session, but -- per the whole point of this
// screen -- is left wherever the user leaves it between keystrokes
// within a session, so picking several nearby letters stays quick.
void beginNameEntry(NamePurpose purpose, const char* initial) {
    namePurpose = purpose;
    nameEntryError[0] = '\0';

    int len = (int)strlen(initial);
    if (len > NAME_ENTRY_MAX_LEN) len = NAME_ENTRY_MAX_LEN;
    memcpy(nameEntryBuf, initial, len);
    nameEntryBuf[len] = '\0';
    nameEntryLen = len;

    nameEntryKeyRow = 1; // top-left letter key ('Q')
    nameEntryKeyCol = 0;

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

void beginRename() {
    if (selectedIndex < 0 || selectedIndex >= browser.entryCount()) return;
    const BrowserEntry& e = browser.entry(selectedIndex);
    browser.buildFullPath(selectedIndex, renameOldPath, sizeof(renameOldPath));
    renameIsDir = e.isDir;
    renameIsSysEx = e.isSysEx;

    char base[NAME_ENTRY_MAX_LEN + 1];
    strncpy(base, e.name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    if (!e.isDir) {
        // Strip the ".mid"/".syx" extension for editing; re-appended on save.
        size_t nlen = strlen(base);
        const char* ext = renameIsSysEx ? ".syx" : ".mid";
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

// ".mid"/".syx" for a recording/capture or a file rename, "" for a folder
// rename/create -- shared by the draw-dispatch (full redraw) and the live
// preview update (partial redraw) so both agree on what's appended after
// the typed name.
const char* currentNameSuffix() {
    if (namePurpose == NAME_RENAME) return renameIsDir ? "" : (renameIsSysEx ? ".syx" : ".mid");
    if (namePurpose == NAME_NEW_RECORDING) return ".mid";
    if (namePurpose == NAME_NEW_SYSEX_CAPTURE) return ".syx";
    return "";
}

// Trims any trailing spaces the user typed via the SPACE key off
// nameEntryBuf and performs whatever namePurpose calls for. Failures
// (name collision, filesystem error) stay on the editor screen with
// `nameEntryError` set rather than navigating away, so the user can just
// change the name and retry.
void finishNameEntry() {
    char trimmed[NAME_ENTRY_MAX_LEN + 1];
    strncpy(trimmed, nameEntryBuf, sizeof(trimmed));
    trimmed[NAME_ENTRY_MAX_LEN] = '\0';
    int len = (int)strlen(trimmed);
    while (len > 0 && trimmed[len - 1] == ' ') trimmed[--len] = '\0';
    if (len == 0) return; // nothing typed -- stay on the naming screen

    if (namePurpose == NAME_NEW_RECORDING) {
        char filename[NAME_ENTRY_MAX_LEN + 5];
        snprintf(filename, sizeof(filename), "%s.mid", trimmed);
        char path[224];
        browser.buildPath(filename, path, sizeof(path));

        strncpy(nowRecordingName, trimmed, sizeof(nowRecordingName) - 1);
        nowRecordingName[sizeof(nowRecordingName) - 1] = '\0';

        recorder.arm(path); // failure surfaces via recorder.state() on the recording screen
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
        appState = APP_CAPTURING_SYSEX;
        needsRedraw = true;
        return;
    }

    if (namePurpose == NAME_NEW_FOLDER) {
        char path[224];
        browser.buildPath(trimmed, path, sizeof(path));
        if (sd.exists(path)) {
            strncpy(nameEntryError, "already exists", sizeof(nameEntryError) - 1);
        } else if (!sd.mkdir(path)) {
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
        snprintf(filename, sizeof(filename), "%s%s", trimmed, renameIsSysEx ? ".syx" : ".mid");
        browser.buildPath(filename, newPath, sizeof(newPath));
    }
    if (strcmp(newPath, renameOldPath) == 0) {
        appState = APP_BROWSE; // unchanged -- nothing to do
    } else if (sd.exists(newPath)) {
        strncpy(nameEntryError, "already exists", sizeof(nameEntryError) - 1);
    } else if (!sd.rename(renameOldPath, newPath)) {
        strncpy(nameEntryError, "rename failed", sizeof(nameEntryError) - 1);
    } else {
        browser.refresh();
        appState = APP_BROWSE;
    }
    if (appState == APP_BROWSE) needsRedraw = true;
    else Ui::updateNameEntryError(nameEntryError);
}

// Builds the contextual action list for whatever's currently selected in
// the browser: Open/Rename/Delete only appear when there's a valid
// selection, New Recording/Capture SysEx Dump/New Folder are always
// offered.
void beginEntryMenu() {
    bool hasSelection = (selectedIndex >= 0 && selectedIndex < browser.entryCount());
    // The browser only ever lists directories, .mid files, and .syx files
    // (see drawBrowser()'s "(no .mid/.syx files here)" case) -- so a
    // non-directory selection is always one of those two.
    bool isFile = hasSelection && !browser.entry(selectedIndex).isDir;
    bool isMidFile = isFile && !browser.entry(selectedIndex).isSysEx;

    menuEntryCount = 0;
    if (hasSelection) {
        menuEntries[menuEntryCount++] = {MENU_OPEN, "Open"};
        menuEntries[menuEntryCount++] = {MENU_RENAME, "Rename"};
        menuEntries[menuEntryCount++] = {MENU_DELETE, "Delete"};
    }
    if (isMidFile) {
        // Not offered for a .syx selection -- the looper works in terms of
        // MIDI events/tracks, which a raw SysEx dump doesn't have.
        menuEntries[menuEntryCount++] = {MENU_LOAD_TO_LOOPER, "Load to Looper"};
    }
    menuEntries[menuEntryCount++] = {MENU_NEW_RECORDING, "New Recording"};
    menuEntries[menuEntryCount++] = {MENU_CAPTURE_SYSEX, "Capture SysEx Dump"};
    menuEntries[menuEntryCount++] = {MENU_NEW_FOLDER, "New Folder"};

    if (hasSelection) {
        strncpy(menuSubtitle, browser.entry(selectedIndex).name, sizeof(menuSubtitle) - 1);
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
            selectedIndex = 0;
            scrollOffset = 0;
            needsRedraw = true;
        } else {
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
    bool playing = (player.state() == MidiPlayer::STATE_PLAYING);
    bool changed = false;

    if (Input::isDown(BTN_UP) &&
        (Input::justPressed(BTN_UP) || now - lastUpStep >= REPEAT_INTERVAL_MS)) {
        player.adjustTempoScale(0.05f);
        lastUpStep = now;
        changed = true;
    }
    if (Input::isDown(BTN_DOWN) &&
        (Input::justPressed(BTN_DOWN) || now - lastDownStep >= REPEAT_INTERVAL_MS)) {
        player.adjustTempoScale(-0.05f);
        lastDownStep = now;
        changed = true;
    }
    if (changed && !playing) {
        // STATE_ERROR uses a completely different screen layout (see
        // handleOpen()), which updatePlayerLive() isn't safe to draw over.
        if (player.state() == MidiPlayer::STATE_ERROR) needsRedraw = true;
        else Ui::updatePlayerLive(player);
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
    MidiPlayer::State st = player.state();
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
            player.seekTo(player.elapsedMs() + (accel ? FAST_STEP_MS : NORMAL_STEP_MS));
            lastRightStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_LEFT)) {
        bool accel = now - leftPressedAtMs >= ACCEL_AFTER_MS;
        uint32_t interval = accel ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) {
            uint32_t step = accel ? FAST_STEP_MS : NORMAL_STEP_MS;
            uint32_t current = player.elapsedMs();
            player.seekTo(current > step ? current - step : 0);
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
        if (player.state() == MidiPlayer::STATE_ERROR) {
            needsRedraw = true;
        } else {
            Ui::updatePlayerState(player.state(), stoppedViaNav);
            Ui::updatePlayerLive(player);
        }
    }
}

// Reloads the currently-open file from the start -- shared by PLAY's
// "restart a just-finished file" and NAV's "stop" (see handlePlayInput()).
// Deliberately does NOT touch tempo scale (a restart keeps whatever speed
// you'd dialed in, unlike a fresh openSelected() open) or auto-play --
// callers that want to keep playing call player.play() themselves
// afterward. Returns true on success (state is now STATE_PAUSED at
// position 0); false means the reload failed and state is now
// STATE_ERROR instead -- callers use this to decide between a targeted
// partial update and a full redraw (STATE_ERROR needs the completely
// different error layout).
bool restartFromTop() {
    char path[192];
    browser.buildFullPath(selectedIndex, path, sizeof(path));
    bool ok = player.load(path);
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
        if (player.state() == MidiPlayer::STATE_PLAYING) {
            player.pause();
            stoppedViaNav = false;
            Ui::updatePlayerState(player.state(), stoppedViaNav);
        } else if (player.state() == MidiPlayer::STATE_PAUSED) {
            player.play();
            stoppedViaNav = false;
            Ui::updatePlayerState(player.state(), stoppedViaNav);
        } else if (player.state() == MidiPlayer::STATE_DONE) {
            // selectedIndex/browser haven't changed since this file was
            // opened (nothing mutates them while in APP_PLAY), so
            // restartFromTop() still points at the same entry.
            if (restartFromTop()) player.play();
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
        player.stop();
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
        if (player.state() == MidiPlayer::STATE_PLAYING ||
            player.state() == MidiPlayer::STATE_PAUSED ||
            player.state() == MidiPlayer::STATE_DONE) {
            MidiOutput::allNotesOffAllChannels();
            if (restartFromTop()) {
                stoppedViaNav = true;
                Ui::updatePlayerState(player.state(), stoppedViaNav);
                Ui::updatePlayerLive(player);
            } else {
                stoppedViaNav = false;
                needsRedraw = true;
            }
        }
    }
    // STATE_ERROR replaces this whole row (and everything below State)
    // with the error message -- see drawPlayer() -- so a partial update
    // isn't safe there; fall back to a full redraw in that case.
    if (Input::justPressed(BTN_ALT)) {
        cycleOutputTarget();
        if (player.state() == MidiPlayer::STATE_ERROR) needsRedraw = true;
        else Ui::updatePlayerOutputTarget(outputTarget);
    }
    // EDIT is a tap-vs-hold modifier here, same disambiguation LooperMode
    // uses for its own Metro/Count-In rows: tap toggles Audio, but only if
    // this press wasn't also used as a hold-modifier for Volume
    // (handleVolumeHold()) or Scrub (handleSeekHold()) below.
    if (Input::justPressed(BTN_EDIT)) {
        editUsedAsModifier = false;
    } else if (Input::justReleased(BTN_EDIT)) {
        if (!editUsedAsModifier) {
            toggleAudio();
            if (player.state() == MidiPlayer::STATE_ERROR) needsRedraw = true;
            else Ui::updatePlayerAudioState(audioOn);
        }
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
        if (menuCursor > 0) moveMenuCursor(menuCursor - 1);
    }
    if (Input::justPressed(BTN_DOWN)) {
        if (menuCursor < menuEntryCount - 1) moveMenuCursor(menuCursor + 1);
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
    });
    MidiOutput::setTarget(outputTarget);
    MidiOutput::setAudioOutput(audioOn);
    Synth::setVolume((uint8_t)volume);

    appState = APP_BROWSE;
    needsRedraw = true;
}

bool update() {
    bool exitRequested = false;

    switch (appState) {
        case APP_BROWSE:
            exitRequested = handleBrowseInput();
            break;

        case APP_PLAY: {
            player.update();
            // Once the file finishes, keep showing the final "Finished"
            // screen until the user presses a button to go back.
            handlePlayInput();

            // Elapsed time and the note-activity strip change continuously
            // while playing. Rather than trigger a full-screen needsRedraw,
            // repaint just those parts directly at a low fixed rate.
            static uint32_t lastPlayTick = 0;
            uint32_t now = millis();
            if (player.state() == MidiPlayer::STATE_PLAYING && now - lastPlayTick >= 50) {
                lastPlayTick = now;
                Ui::updatePlayerLive(player);
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
            }
            break;
        }

        case APP_CONFIRM_CANCEL_SYSEX_CAPTURE:
            handleConfirmCancelSysExCaptureInput();
            break;

        case APP_LOOPER_TRACK_PICK:
            handleLooperTrackPickInput();
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
                Ui::drawPlayer(nowPlayingName, player, outputTarget, audioOn, volume, stoppedViaNav);
                break;

            case APP_PLAY_SYSEX:
                Ui::drawSysExPlayer(nowPlayingName, sysexPlayer);
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

            case APP_CONFIRM_DELETE: {
                bool valid = (selectedIndex >= 0 && selectedIndex < browser.entryCount());
                const BrowserEntry& e = browser.entry(valid ? selectedIndex : 0);
                Ui::drawConfirmDelete(valid ? e.name : "", valid && e.isDir, deleteFailed);
                break;
            }

            case APP_RECORDING:
                Ui::drawRecording(nowRecordingName, recorder);
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
                Ui::drawSysExCapture(nowCapturingSysExName, sysexRecorder);
                break;

            case APP_CONFIRM_CANCEL_SYSEX_CAPTURE:
                // Same reasoning as APP_CONFIRM_CANCEL_RECORDING above.
                Ui::drawMessage("Discard this capture?", "ENTER discard  NAV keep capturing");
                break;

            case APP_LOOPER_TRACK_PICK:
                Ui::drawEntryMenu("Load into Track", LOOPER_TRACK_LABELS, 4, looperTrackPickCursor);
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

} // namespace FilePlayerMode

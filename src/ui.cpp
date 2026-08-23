#include "ui.h"
#include "battery.h"
#include "pins.h"
#include "keyboard_layout.h"
#include "version.h"
#include <TFT_eSPI.h>
#include <string.h>
#include <stdio.h>

namespace {

TFT_eSPI tft = TFT_eSPI();

const uint16_t COLOR_BG        = TFT_BLACK;
const uint16_t COLOR_TEXT      = TFT_WHITE;
const uint16_t COLOR_DIM       = TFT_DARKGREY;
const uint16_t COLOR_HILITE_BG = TFT_NAVY;
const uint16_t COLOR_ACCENT    = TFT_GREEN;
const uint16_t COLOR_ERROR     = TFT_RED;

const int ROW_HEIGHT = 18;
const int HEADER_HEIGHT = 20;
// Tall enough for the browser's two-line button-hint footer; other screens
// just get a bit of empty space below their single hint line.
const int FOOTER_HEIGHT = 34;

// Now-playing screen layout. Short stats are packed two-per-row (a left
// item at x=8, a right one at STAT_COL2_X) rather than one full-width row
// each, since every one of these strings is short enough to leave most of
// a 320px-wide row empty otherwise. That reclaimed height goes straight
// to the note strip, which is the whole point of this screen -- see
// STRIP_HEIGHT below. Tempo and the MIDI/Audio line are long enough that
// pairing them risks running into whatever's on the other side of that
// same row (see updateStatValue()'s comment), so those two keep a full
// row to themselves instead.
//
// Fixed (rather than accumulated inline) so the live-update path can
// repaint just the parts that actually change during playback (elapsed
// time, tempo -- see updatePlayerLive()) without redrawing the rest.
// These offsets only apply to the non-error player state, which is the
// only state the live tick runs in.
const int PLAYER_FILENAME_Y = HEADER_HEIGHT + 8;
const int PLAYER_ROW2_Y = PLAYER_FILENAME_Y + ROW_HEIGHT + 4; // State | Time
const int PLAYER_ROW3_Y = PLAYER_ROW2_Y + ROW_HEIGHT;         // Tempo (alone)
const int PLAYER_ROW4_Y = PLAYER_ROW3_Y + ROW_HEIGHT;         // Tracks | Volume
const int PLAYER_ROW5_Y = PLAYER_ROW4_Y + ROW_HEIGHT;         // MIDI target + Audio on/off (alone)
const int PLAYER_STRIP_Y = PLAYER_ROW5_Y + ROW_HEIGHT + 6;
const int STRIP_HEIGHT      = 74; // was 30 pre-rejigger; the stat-row packing above buys this back
const int STRIP_MIN_BAR_HEIGHT = 4; // floor so even the softest note stays visible

// Fixed x for the second column on paired stat rows (State|Time,
// Tracks|Volume). Left-aligned rather than right-aligned-to-edge like the
// first version of this layout -- a stable, known x is what lets
// updateStatValue() clear/redraw just a value substring without needing
// to also touch (or re-measure) a label right next to it.
const int STAT_COL2_X = 164;

// Note-activity strip range: standard 88-key piano (A0..C8). Notes
// outside this range (rare outside percussion channels) just don't show.
const int PIANO_LO = 21;
const int PIANO_HI = 108;

// Recording screen layout, same fixed-offset approach as the player
// screen above so updateRecordingLive() can repaint just the elapsed/
// event-count rows.
const int REC_FILENAME_Y = HEADER_HEIGHT + 10;
const int REC_STATE_Y    = REC_FILENAME_Y + ROW_HEIGHT + 6;
const int REC_ELAPSED_Y  = REC_STATE_Y + ROW_HEIGHT;
const int REC_EVENTS_Y   = REC_ELAPSED_Y + ROW_HEIGHT;

// SysEx capture/playback screens reuse the REC_* rows above (filename/
// state/elapsed/message-count) for a consistent look with drawRecording()
// -- see drawSysExCapture()/drawSysExPlayer()'s own comments for what
// replaces drawRecording()'s footer-only tail below that.

// Activity dot (recording/capture screens) / progress-bar flash (SysEx
// playback screen): lit/bright while an event/message arrived (or was
// sent) within this long, dim/steady otherwise -- short enough to read as
// a per-event blip rather than a sustained glow, same "recent activity"
// convention LooperMode's track activity dots use (ACTIVITY_FLASH_MS
// there).
const uint32_t RECORDING_ACTIVITY_MS = 250;

// .syx playback's progress bar, filling the same vertical space
// drawPlayer()'s note strip would otherwise occupy.
const int SYSEX_PROGRESS_LABEL_Y = REC_EVENTS_Y + ROW_HEIGHT + 12;
const int SYSEX_PROGRESS_BAR_Y   = SYSEX_PROGRESS_LABEL_Y + ROW_HEIGHT;
const int SYSEX_PROGRESS_BAR_H   = 24;

// On-screen QWERTY keyboard layout (name entry screen). Rows 0-3 (digits,
// QWERTYUIOP, ASDFGHJKL, ZXCVBNM_-) use KB_KEY_W; row 4 (SPACE/DEL/OK)
// uses three wider keys spanning the same total row width (10*30 ==
// 3*100), so every row ends up the same width and just centers itself --
// giving the classic staggered-keyboard look for free.
const int KB_KEY_W = 30;
const int KB_WIDE_KEY_W = 100;
const int KB_KEY_H = 22;
const int KB_ROW_SPACING = 24;
const int NAME_PREVIEW_Y = HEADER_HEIGHT + 8;
const int KB_START_Y = NAME_PREVIEW_Y + ROW_HEIGHT + 6;

int rowsOnScreen() {
    int usable = tft.height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int rows = usable / ROW_HEIGHT;
    return rows < 1 ? 1 : rows;
}

// Draws (or erases) a single browser row at `idx`, given the list is
// currently scrolled to `scrollOffset`. Used both by the full redraw and
// by the selection-only partial redraw so highlight moves don't need to
// repaint the header/footer/whole list.
void drawBrowserRow(const SdBrowser& browser, int idx, int scrollOffset, bool selected) {
    int row = idx - scrollOffset;
    if (row < 0 || row >= rowsOnScreen()) return;

    int y = HEADER_HEIGHT + row * ROW_HEIGHT;
    if (idx < 0 || idx >= browser.entryCount()) {
        tft.fillRect(0, y, tft.width(), ROW_HEIGHT, COLOR_BG);
        return;
    }

    const BrowserEntry& e = browser.entry(idx);
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(0, y, tft.width(), ROW_HEIGHT, bg);

    tft.setTextColor(e.isDir ? COLOR_ACCENT : COLOR_TEXT, bg);
    char line[MAX_FILENAME_LEN + 2];
    snprintf(line, sizeof(line), "%s%s", e.isDir ? "/" : " ", e.name);
    tft.drawString(line, 6, y + 2);
}

// Vertical scrollbar on a row list's right edge, same idea as a
// webpage/document scrollbar -- thumb size shows how much of the whole
// list is visible at once, thumb position shows where in the list that
// is. Only relevant once there's more than one screenful (count > rows);
// callers skip calling this otherwise. Drawn last (after the row loop) so
// it overlays the row area rather than getting overdrawn by it. Shared by
// every scrollable row list (the file browser, the Settings screen) --
// `trackY` is the caller's own first-row Y, since that varies slightly
// screen to screen (e.g. Settings' rows start a few px below the header).
const int SCROLLBAR_W = 3;
const int SCROLLBAR_MIN_THUMB_H = 8; // stays visible/legible even for a very long list

void drawScrollbar(int count, int scrollOffset, int rows, int trackY) {
    int trackX = tft.width() - SCROLLBAR_W;
    int trackH = rows * ROW_HEIGHT;

    tft.fillRect(trackX, trackY, SCROLLBAR_W, trackH, COLOR_DIM);

    int thumbH = (trackH * rows) / count;
    if (thumbH < SCROLLBAR_MIN_THUMB_H) thumbH = SCROLLBAR_MIN_THUMB_H;
    if (thumbH > trackH) thumbH = trackH;

    int maxThumbY = trackH - thumbH;
    int maxScroll = count - rows;
    int thumbY = maxScroll > 0 ? (maxThumbY * scrollOffset) / maxScroll : 0;
    if (thumbY > maxThumbY) thumbY = maxThumbY;

    tft.fillRect(trackX, trackY + thumbY, SCROLLBAR_W, thumbH, COLOR_TEXT);
}

// Redraws the full note-activity strip at `y`: one column per piano key,
// bar height scaled to that note's velocity (max velocity = full height,
// floored at STRIP_MIN_BAR_HEIGHT so soft notes stay visible), anchored
// to the bottom like a keybed/VU meter. Percussion-channel notes (see
// MidiOutput::isNotePercussion()) get a warm red/orange palette instead of
// the melodic green/yellow one, so drum hits read as visually distinct
// from melodic notes at a glance rather than just being more activity in
// the same color. Every column is fully repainted every call -- cheap
// enough (88 small fillRects) to just call at a fixed low rate rather
// than diffing.
void drawNoteStrip(int y) {
    int span = PIANO_HI - PIANO_LO + 1;
    int colWidth = tft.width() / span;
    if (colWidth < 1) colWidth = 1;
    int barWidth = colWidth > 1 ? colWidth - 1 : 1;

    for (int note = PIANO_LO; note <= PIANO_HI; note++) {
        int x = (note - PIANO_LO) * colWidth;
        int barHeight = 0;
        uint16_t color = COLOR_BG;
        if (MidiOutput::isNoteActive((uint8_t)note)) {
            uint8_t vel = MidiOutput::noteVelocity((uint8_t)note);
            barHeight = STRIP_MIN_BAR_HEIGHT + ((STRIP_HEIGHT - STRIP_MIN_BAR_HEIGHT) * vel) / 127;
            if (MidiOutput::isNotePercussion((uint8_t)note)) {
                color = (vel >= 100) ? TFT_RED : (vel >= 64) ? TFT_ORANGE : TFT_MAROON;
            } else {
                color = (vel >= 100) ? TFT_YELLOW : (vel >= 64) ? TFT_GREEN : TFT_DARKGREEN;
            }
        }
        if (barHeight < STRIP_HEIGHT) {
            tft.fillRect(x, y, barWidth, STRIP_HEIGHT - barHeight, COLOR_BG);
        }
        if (barHeight > 0) {
            tft.fillRect(x, y + STRIP_HEIGHT - barHeight, barWidth, barHeight, color);
        }
    }
}

// Draws one stat row: `left` at x=8, and, if non-null, `right` at
// STAT_COL2_X. Used for rows whose content never changes after the
// initial draw (State, Tracks, Volume, MIDI/Sound) -- see updateStatValue()
// below for the two fields that *do* need periodic updating.
void drawStatRow(int y, const char* left, const char* right) {
    tft.setTextDatum(TL_DATUM);
    tft.drawString(left, 8, y);
    if (right && right[0]) tft.drawString(right, STAT_COL2_X, y);
}

// Partial redraw of just the right half of a stat row, for a value that
// changes independently of whatever's on the left (e.g. Volume, which the
// live-update path never touches -- see Ui::updateVolume()). Safe here
// specifically because Volume/MIDI-target strings are short and bounded.
void updateStatRight(int y, const char* text) {
    tft.fillRect(STAT_COL2_X, y, tft.width() - STAT_COL2_X, ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(text, STAT_COL2_X, y);
}

// Cached last-drawn value strings for Time and Tempo, the two fields
// updatePlayerLive() polls every 50ms during playback -- see
// updateStatValue() below. Reset to empty in drawPlayer() so a freshly
// opened file always draws its real starting value on the first tick
// rather than skipping a stale match left over from a previous file.
char g_lastTimeStr[16] = {0};
char g_lastTempoStr[24] = {0};

// Draws (or re-draws) just a field's *value*, immediately after a label
// that's drawn once elsewhere and never touched again (see TIME_LABEL/
// TEMPO_LABEL below). `x` is where the value starts -- i.e. labelX +
// tft.textWidth(label), computed by the caller since only it knows which
// label this value follows.
//
// Two things keep this from ever blinking more than the field actually
// needs to:
//  - No-ops entirely if `value` matches `cache` (this field's own
//    persistent last-drawn-value buffer, e.g. g_lastTempoStr) -- so a
//    value that rarely changes, like Tempo between tempo-meta-events,
//    doesn't get cleared and redrawn on every single poll.
//  - When it does redraw, only clears max(old width, new width) -- not
//    the label, not a neighboring column/row -- using tft.textWidth() to
//    measure exactly rather than assuming a fixed column boundary. That
//    was the bug in an earlier version of this screen: "Tempo: 120 BPM
//    (100%)" could render wider than a fixed half-row clear accounted
//    for, leaving stale pixels behind next to Tracks.
void updateStatValue(int x, int y, const char* value, char* cache, size_t cacheSize) {
    if (strncmp(cache, value, cacheSize) == 0) return;

    int oldWidth = tft.textWidth(cache);
    int newWidth = tft.textWidth(value);
    int clearWidth = (oldWidth > newWidth ? oldWidth : newWidth) + 2;

    tft.fillRect(x, y, clearWidth, ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(value, x, y);

    strncpy(cache, value, cacheSize - 1);
    cache[cacheSize - 1] = '\0';
}

const char* TIME_LABEL = "Time: ";
const char* TEMPO_LABEL = "Tempo: ";
const char* STATE_LABEL = "State: ";

// Cached last-drawn State value, same purpose/pattern as g_lastTimeStr/
// g_lastTempoStr above -- lets Ui::updatePlayerState() no-op on a
// pause/resume that (in principle) didn't actually change anything, and
// only clear+redraw the value substring rather than the whole row.
char g_lastStateStr[16] = {0};

// Partial redraw of just the left half of a stat row -- the mirror image
// of updateStatRight() above, for a field whose *label* lives at x=8 and
// whose value can change independently of whatever's in the right column
// (e.g. MIDI target, which the live-update path never touches). Safe here
// specifically because MIDI-target strings are short and bounded well
// under the STAT_COL2_X boundary.
void updateStatLeft(int y, const char* text) {
    tft.fillRect(8, y, STAT_COL2_X - 8 - 4, ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(text, 8, y);
}

// Right-aligned "midiTracker" branding in the header bar, drawn over
// whatever's already there (header rect + left-aligned title/path must be
// drawn first). Leaves the text datum at TL_DATUM on return.
void drawHeaderBrand() {
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.drawString("midiTracker", tft.width() - 4, 3);
    tft.setTextDatum(TL_DATUM);
}

// Header beat-indicator row (see drawLooperBeatIndicator()) -- BEATS_MAX
// matches TIME_SIG_PRESETS' widest numerator, 7 (7/8), in looper_mode.cpp;
// used to size a fixed clear region wide enough for the largest possible
// row so shrinking from a wider time signature (or turning Metro off)
// never leaves a stray square from the previous draw.
const int BEAT_INDICATOR_MAX_BEATS = 7;
const int BEAT_INDICATOR_SIZE = 8;
const int BEAT_INDICATOR_GAP = 6;
const int BEAT_INDICATOR_MAX_WIDTH =
    BEAT_INDICATOR_MAX_BEATS * BEAT_INDICATOR_SIZE + (BEAT_INDICATOR_MAX_BEATS - 1) * BEAT_INDICATOR_GAP; // 92

// Header beat-indicator row: one small square per beat of the current time
// signature, centered between "MIDI Looper" and "midiTracker" (see
// drawHeaderBrand()). The whole row is hidden (region just blanked back to
// the header background) whenever `visible` is false -- gated purely on
// Metro's own on/off toggle, not on whether anything's actually audible
// right now (see LooperMode's updateMetronome() -- the same distinction it
// draws between "clock running" and "click audible"). Three visual states
// per square: the current beat is lit solid bright (TFT_YELLOW, same
// "active" color as drawActivityDot()); the downbeat (index 0) gets a
// solid-outline resting look even when it isn't current, so the bar's start
// stays visible at a glance; every other beat is a plain dim (COLOR_DIM)
// filled square. Always clears a fixed BEAT_INDICATOR_MAX_WIDTH-wide region
// first regardless of the actual (possibly narrower) row about to be drawn,
// so shrinking timeSigNum (or Metro turning off) can't leave a stray square
// behind from a wider previous draw.
void drawLooperBeatIndicator(bool visible, int currentBeat, int timeSigNum) {
    int clearX = tft.width() / 2 - BEAT_INDICATOR_MAX_WIDTH / 2;
    tft.fillRect(clearX, 0, BEAT_INDICATOR_MAX_WIDTH, HEADER_HEIGHT, COLOR_HILITE_BG);
    if (!visible || timeSigNum < 1) return;

    int rowWidth = timeSigNum * BEAT_INDICATOR_SIZE + (timeSigNum - 1) * BEAT_INDICATOR_GAP;
    int x = tft.width() / 2 - rowWidth / 2;
    int y = (HEADER_HEIGHT - BEAT_INDICATOR_SIZE) / 2;

    for (int i = 0; i < timeSigNum; i++) {
        if (i == currentBeat) {
            tft.fillRect(x, y, BEAT_INDICATOR_SIZE, BEAT_INDICATOR_SIZE, TFT_YELLOW);
        } else if (i == 0) {
            tft.drawRect(x, y, BEAT_INDICATOR_SIZE, BEAT_INDICATOR_SIZE, COLOR_TEXT);
        } else {
            tft.fillRect(x, y, BEAT_INDICATOR_SIZE, BEAT_INDICATOR_SIZE, COLOR_DIM);
        }
        x += BEAT_INDICATOR_SIZE + BEAT_INDICATOR_GAP;
    }
}

void formatDuration(uint32_t ms, char* out, size_t outSize) {
    uint32_t totalSec = ms / 1000;
    uint32_t m = totalSec / 60;
    uint32_t s = totalSec % 60;
    snprintf(out, outSize, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
}

const char* targetLabel(MidiOutTarget t) {
    switch (t) {
        case MIDI_OUT_HARDWARE: return "HW MIDI";
        case MIDI_OUT_USB:      return "USB MIDI";
        default:                return "HW+USB";
    }
}

// `stopped` distinguishes NAV's "reset to the start" from PLAY's plain
// pause -- both leave the player in STATE_PAUSED (there's no separate
// MidiPlayer state for it), so FilePlayerMode tracks which one happened
// itself and passes it through here.
const char* stateLabel(MidiPlayer::State s, bool stopped) {
    switch (s) {
        case MidiPlayer::STATE_PLAYING: return "Playing";
        case MidiPlayer::STATE_PAUSED:  return stopped ? "Stopped" : "Paused";
        case MidiPlayer::STATE_DONE:    return "Finished";
        case MidiPlayer::STATE_ERROR:   return "Error";
        default:                        return "Idle";
    }
}

const char* recordStateLabel(MidiRecorder::State s) {
    switch (s) {
        case MidiRecorder::STATE_ARMED:     return "Waiting for input";
        case MidiRecorder::STATE_RECORDING: return "Recording";
        case MidiRecorder::STATE_ERROR:     return "Error";
        default:                            return "Idle";
    }
}

const char* sysExCaptureStateLabel(SysExRecorder::State s) {
    switch (s) {
        case SysExRecorder::STATE_ARMED:     return "Waiting for SysEx";
        case SysExRecorder::STATE_RECORDING: return "Capturing";
        case SysExRecorder::STATE_ERROR:     return "Error";
        default:                             return "Idle";
    }
}

const char* sysExPlayerStateLabel(SysExPlayer::State s) {
    switch (s) {
        case SysExPlayer::STATE_SENDING: return "Sending";
        case SysExPlayer::STATE_DONE:    return "Finished";
        case SysExPlayer::STATE_ERROR:   return "Error";
        default:                         return "Idle";
    }
}

void drawActivityDot(int x, int y, bool active) {
    tft.fillCircle(x, y, 5, active ? TFT_YELLOW : COLOR_DIM);
}

// Byte-progress bar + numeric readout, filling the space drawPlayer()'s
// note strip would otherwise occupy (meaningless for a raw SysEx dump --
// there are no notes). The filled portion briefly switches from steady
// green to bright yellow immediately after a message is actually sent
// (see RECORDING_ACTIVITY_MS), then settles back -- a static bar alone reads
// as "how far along", the flash is what reads as "actively sending right
// now" per this screen's whole point.
void drawSysExProgress(const SysExPlayer& player) {
    uint32_t total = player.totalBytes();
    uint32_t sent = player.bytesSent();
    int pct = total > 0 ? (int)(((uint64_t)sent * 100) / total) : 0;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu / %lu bytes (%d%%)", (unsigned long)sent, (unsigned long)total, pct);
    tft.fillRect(0, SYSEX_PROGRESS_LABEL_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(buf, 8, SYSEX_PROGRESS_LABEL_Y);

    int barX = 8, barW = tft.width() - 16;
    tft.fillRect(barX, SYSEX_PROGRESS_BAR_Y, barW, SYSEX_PROGRESS_BAR_H, COLOR_DIM);
    int fillW = total > 0 ? (int)(((uint64_t)barW * sent) / total) : 0;
    if (fillW > 0) {
        bool active = player.msSinceLastSend() < RECORDING_ACTIVITY_MS;
        tft.fillRect(barX, SYSEX_PROGRESS_BAR_Y, fillW, SYSEX_PROGRESS_BAR_H, active ? TFT_YELLOW : TFT_GREEN);
    }

    // Nothing else occupies the space below the bar down to the footer.
    int fy = tft.height() - FOOTER_HEIGHT;
    int contentBottom = SYSEX_PROGRESS_BAR_Y + SYSEX_PROGRESS_BAR_H;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
}

} // namespace

namespace Ui {

void begin() {
    pinMode(DISPLAY_BACKLIGHT, OUTPUT);
    digitalWrite(DISPLAY_BACKLIGHT, HIGH);

    tft.init();
    tft.setRotation(3);
    tft.fillScreen(COLOR_BG);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
}

int visibleRows() { return rowsOnScreen(); }

void drawSplash() {
    tft.fillScreen(COLOR_BG);
    tft.setTextDatum(MC_DATUM);
    int cx = tft.width() / 2;
    int cy = tft.height() / 2;

    tft.setTextColor(COLOR_ACCENT, COLOR_BG);
    tft.setTextFont(4);
    tft.drawString("midiTracker", cx, cy - 14);

    tft.setTextFont(2);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("play. record. loop.", cx, cy + 18);

    // Bottom middle, same dim/gray styling as the subtext above.
    char verBuf[24];
    snprintf(verBuf, sizeof(verBuf), "v%s", VERSIONNUMBER);
    tft.drawString(verBuf, cx, tft.height() - 12);

    tft.setTextDatum(TL_DATUM);
}

void drawBrowser(const SdBrowser& browser, int selectedIndex, int scrollOffset) {
    // Header: current path
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(browser.currentPath(), 4, 3);
    drawHeaderBrand();

    int rows = rowsOnScreen();
    int count = browser.entryCount();

    // Every row slot gets painted, not just the ones with an entry --
    // drawBrowserRow() blanks slots past entryCount to COLOR_BG -- so a
    // directory with fewer files than the last one doesn't leave stale
    // rows behind. That's what lets this skip a blanket fillScreen()
    // before redrawing, which used to cause a visible full-screen black
    // flash on every directory change (openSelected()/goUp() and
    // moveSelection()'s page-scroll path all funnel through here).
    for (int row = 0; row < rows; row++) {
        int idx = scrollOffset + row;
        drawBrowserRow(browser, idx, scrollOffset, idx == selectedIndex);
    }

    // Row slots only cover a whole multiple of ROW_HEIGHT, which can leave
    // a sliver of the body area between the last row and the footer --
    // clear it explicitly rather than relying on the (now-removed)
    // fillScreen() to have done it.
    int bodyBottom = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = HEADER_HEIGHT + rows * ROW_HEIGHT;
    if (rowsBottom < bodyBottom) {
        tft.fillRect(0, rowsBottom, tft.width(), bodyBottom - rowsBottom, COLOR_BG);
    }

    if (count == 0) {
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString("(no .mid/.syx/.wav/.mod/.s3m files here)", 8, HEADER_HEIGHT + 8);
    } else if (count > rows) {
        drawScrollbar(count, scrollOffset, rows, HEADER_HEIGHT);
    }

    // Footer: button hints
    int fy = tft.height() - FOOTER_HEIGHT;
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // Grouped by physical button row (see pins.h's layout diagram): line 1
    // is the row nearest the screen (UP/PLAY/EDIT), line 2 the two rows
    // below it (LEFT/DOWN/RIGHT/ENTER, then ALT/NAV) -- same convention
    // every other footer in this file follows. PLAY/RIGHT also open a
    // file here (same as ENTER) but aren't separately called out, same
    // as everywhere else redundant alternate triggers go unmentioned.
    tft.drawString("UD move/ALT page  EDIT menu", 4, fy + 1);
    tft.drawString("ENTER open  LEFT/NAV back  ALT output", 4, fy + 17);
    drawBatteryMeter();
}

void updateBrowserSelection(const SdBrowser& browser, int prevIndex, int newIndex, int scrollOffset) {
    drawBrowserRow(browser, prevIndex, scrollOffset, false);
    drawBrowserRow(browser, newIndex, scrollOffset, true);

    // drawBrowserRow() fills the full row width, including the scrollbar's
    // 3px column at the right edge -- so the two rows just redrawn need
    // the scrollbar re-layered on top of them, same as drawBrowser()'s own
    // "drawn last so it overlays the row area" ordering.
    int rows = rowsOnScreen();
    int count = browser.entryCount();
    if (count > rows) {
        drawScrollbar(count, scrollOffset, rows, HEADER_HEIGHT);
    }
}

void drawPlayer(const char* filename, const MidiPlayer& player, MidiOutTarget target,
                 bool audioOn, int volume, bool stopped) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Now Playing", 4, 3);
    drawHeaderBrand();

    // None of these rows fill their own background the way e.g.
    // drawBrowserRow() does -- they're short strings on an otherwise-bare
    // row, previously left to a blanket fillScreen() to establish a clean
    // canvas. Now that that's removed (it caused a visible full-screen
    // black flash on every redraw, same fix as drawBrowser()'s/
    // drawLooper()'s), each row clears its own exact width immediately
    // before drawing into it instead, so the transition is a quick
    // sequence of small per-row flashes rather than one big one -- same
    // technique, just applied per-row here instead of via each row
    // drawing its own content.
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), PLAYER_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);

    // Clears through to PLAYER_ROW2_Y, not just ROW_HEIGHT -- the two are
    // PLAYER_ROW2_Y - PLAYER_FILENAME_Y apart (ROW_HEIGHT + a 4px gap), so
    // clearing only ROW_HEIGHT left that trailing 4px strip uncleared,
    // showing leftover file-browser text there.
    tft.fillRect(0, PLAYER_FILENAME_Y, tft.width(), PLAYER_ROW2_Y - PLAYER_FILENAME_Y, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(filename, 8, PLAYER_FILENAME_Y);

    char buf[48];
    tft.fillRect(0, PLAYER_ROW2_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString(STATE_LABEL, 8, PLAYER_ROW2_Y);

    // A fresh file: none of the cached values can still be valid (they
    // may belong to whatever was playing before), so force every field
    // below to draw for real on the first tick rather than skipping a
    // stale match -- see updateStatValue().
    g_lastTimeStr[0] = '\0';
    g_lastTempoStr[0] = '\0';
    g_lastStateStr[0] = '\0';
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     stateLabel(player.state(), stopped), g_lastStateStr, sizeof(g_lastStateStr));

    bool isError = player.state() == MidiPlayer::STATE_ERROR;
    if (isError) {
        // Error replaces everything below State -- there's no elapsed
        // time/tempo/tracks/output to show for a file that never started.
        tft.fillRect(0, PLAYER_ROW3_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(player.errorMessage(), 8, PLAYER_ROW3_Y);
    } else {
        tft.drawString(TIME_LABEL, STAT_COL2_X, PLAYER_ROW2_Y);
        char timeStr[16];
        formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
        updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                         timeStr, g_lastTimeStr, sizeof(g_lastTimeStr));

        tft.fillRect(0, PLAYER_ROW3_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        tft.drawString(TEMPO_LABEL, 8, PLAYER_ROW3_Y);
        snprintf(buf, sizeof(buf), "%u BPM (%d%%)", player.currentBPM(),
                 (int)(player.tempoScale() * 100.0f + 0.5f));
        updateStatValue(8 + tft.textWidth(TEMPO_LABEL), PLAYER_ROW3_Y,
                         buf, g_lastTempoStr, sizeof(g_lastTempoStr));

        tft.fillRect(0, PLAYER_ROW4_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        snprintf(buf, sizeof(buf), "Tracks: %u", player.trackCount());
        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", volume);
        drawStatRow(PLAYER_ROW4_Y, buf, volBuf);

        // Two fixed columns, like Tracks|Volume above, rather than one
        // concatenated string -- lets updatePlayerOutputTarget()/
        // updatePlayerAudioState() repaint just their own half.
        tft.fillRect(0, PLAYER_ROW5_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        char midiBuf[24];
        snprintf(midiBuf, sizeof(midiBuf), "MIDI: %s", targetLabel(target));
        char audioBuf[16];
        snprintf(audioBuf, sizeof(audioBuf), "Audio: %s", audioOn ? "On" : "Off");
        drawStatRow(PLAYER_ROW5_Y, midiBuf, audioBuf);
    }

    // Everything from here down to the footer is either drawNoteStrip()'s
    // own area (which fully repaints its own covered width every call
    // regardless of note state -- see its comment) or otherwise unused in
    // this state. Cleared as one span down to the footer, covering the
    // full width (so the strip's own right-edge remainder, from 320px not
    // dividing evenly by its 88 columns, is included too) -- simpler and
    // more robust than tracking each branch's own exact row count, with
    // no risk of a gap slipping through between them.
    int contentBottom = isError ? (PLAYER_ROW3_Y + ROW_HEIGHT) : (PLAYER_ROW5_Y + ROW_HEIGHT);
    int fy = tft.height() - FOOTER_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    drawNoteStrip(PLAYER_STRIP_Y);
    // This footer was never explicitly cleared even before the
    // fillScreen() removal above -- it relied on that same blanket clear
    // too, unlike drawBrowser()'s/drawLooper()'s own footers which
    // already fillRect their own area.
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // Row-grouped (see pins.h): line 1 = UP/PLAY/EDIT, with EDIT+UP/DOWN
    // (volume) woven into UD's own hint since EDIT is a modifier there
    // (as opposed to its own tap, which toggles Audio) -- same reasoning
    // ALT gets woven into hints elsewhere instead of a separate mention
    // wherever it's a modifier rather than its own tap action. Line 2
    // similarly weaves in ENTER+LEFT/RIGHT (scrub).
    tft.drawString("UD tempo/EDIT+UD vol  PLAY tog  EDIT aud", 4, fy + 1);
    tft.drawString("LEFT back/ENT+LR seek  ALT midi  NAV stop", 4, fy + 17);
    drawBatteryMeter();
}

// Repaints only the volume half of its row (right side of ROW4, next to
// Tracks) -- see the comment on Ui::updateVolume().
void updateVolume(int volume) {
    char buf[24];
    snprintf(buf, sizeof(buf), "Volume: %d%%", volume);
    updateStatRight(PLAYER_ROW4_Y, buf);
}

// Partial redraw of just the State value, e.g. after PLAY toggles
// pause/resume or NAV stops. Caller must not use this for a transition
// into or out of STATE_ERROR -- that swaps the whole row layout (see
// drawPlayer()) and needs a full redraw instead.
void updatePlayerState(MidiPlayer::State state, bool stopped) {
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     stateLabel(state, stopped), g_lastStateStr, sizeof(g_lastStateStr));
}

// Partial redraw of just the MIDI-target half of its row (left side of
// ROW5, next to Audio), e.g. after ALT cycles it. Does not touch Audio.
void updatePlayerOutputTarget(MidiOutTarget target) {
    char buf[24];
    snprintf(buf, sizeof(buf), "MIDI: %s", targetLabel(target));
    updateStatLeft(PLAYER_ROW5_Y, buf);
}

// Partial redraw of just the Audio half of its row (right side of ROW5,
// next to MIDI target), e.g. after EDIT toggles it. Does not touch MIDI.
void updatePlayerAudioState(bool audioOn) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Audio: %s", audioOn ? "On" : "Off");
    updateStatRight(PLAYER_ROW5_Y, buf);
}

void updatePlayerLive(const MidiPlayer& player) {
    // Callers must not use this while STATE_ERROR -- that swaps the whole
    // row layout (see drawPlayer()) and needs a full redraw instead; every
    // other state shares the same non-error row offsets, so this applies
    // unconditionally otherwise (the periodic tick only calls it while
    // STATE_PLAYING, but a seek can also change Time/Tempo while PAUSED or
    // DONE -- see handleSeekHold()). State/Tracks/MIDI/Sound/Volume don't
    // change on their own during playback, so they're not touched here at
    // all -- only Time and Tempo are, and each via updateStatValue() so a
    // poll that finds no actual change (Tempo between tempo-meta-events;
    // Time within the same displayed second) doesn't redraw anything.
    char timeStr[16];
    formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
    updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                     timeStr, g_lastTimeStr, sizeof(g_lastTimeStr));

    char buf[24];
    snprintf(buf, sizeof(buf), "%u BPM (%d%%)", player.currentBPM(),
             (int)(player.tempoScale() * 100.0f + 0.5f));
    updateStatValue(8 + tft.textWidth(TEMPO_LABEL), PLAYER_ROW3_Y,
                     buf, g_lastTempoStr, sizeof(g_lastTempoStr));

    drawNoteStrip(PLAYER_STRIP_Y);
}

// -- WAV player screen -------------------------------------------------
//
// Reuses drawPlayer()'s row Y constants (PLAYER_FILENAME_Y/ROW2_Y/ROW4_Y/
// ROW5_Y) and Ui::updateVolume() unchanged -- Volume sits at ROW4_Y's
// right column, the same position MIDI's Tracks | Volume occupies,
// specifically so handleVolumeHold() (shared verbatim between both
// screens, see file_player_mode.cpp) can redraw Volume through the exact
// same call without a WAV-specific variant; ROW4_Y's left column is left
// blank rather than sharing the row with format info, which would
// otherwise run into Volume's column on a long format string ("44100 Hz
// 16-bit Stereo" is wider than the left column leaves room for). ROW3_Y
// (Tempo, for MIDI) is unused here -- WAV has no per-file tempo. A steady
// elapsed/total progress bar occupies ROW5_Y downward in place of the
// note-activity strip (meaningless for audio) and MIDI-target/Audio rows
// (neither applies to WAV), with the format info line below the bar,
// where it has the full row width to itself.

char g_lastWavTimeStr[24] = {0};
char g_lastWavStateStr[16] = {0};

const int WAV_BAR_Y = PLAYER_ROW5_Y + 4;
const int WAV_BAR_H = 24;
const int WAV_FORMAT_Y = WAV_BAR_Y + WAV_BAR_H + 6;

const char* wavStateLabel(WavPlayer::State s, bool stopped) {
    switch (s) {
        case WavPlayer::STATE_PLAYING: return "Playing";
        case WavPlayer::STATE_PAUSED:  return stopped ? "Stopped" : "Paused";
        case WavPlayer::STATE_DONE:    return "Finished";
        case WavPlayer::STATE_ERROR:   return "Error";
        default:                       return "Idle";
    }
}

// Steady elapsed/total fill -- no activity flash the way
// drawSysExProgress() has, since WAV streaming isn't message-bursty the
// way SysEx sending is.
void drawWavProgressBar(const WavPlayer& player) {
    uint32_t total = player.totalMs();
    uint32_t elapsed = player.elapsedMs();
    if (elapsed > total) elapsed = total;

    int barX = 8, barW = tft.width() - 16;
    tft.fillRect(barX, WAV_BAR_Y, barW, WAV_BAR_H, COLOR_DIM);
    int fillW = total > 0 ? (int)(((uint64_t)barW * elapsed) / total) : 0;
    if (fillW > 0) tft.fillRect(barX, WAV_BAR_Y, fillW, WAV_BAR_H, TFT_GREEN);
}

void drawWavPlayer(const char* filename, const WavPlayer& player, int volume, bool stopped) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Now Playing", 4, 3);
    drawHeaderBrand();

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), PLAYER_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);
    tft.fillRect(0, PLAYER_FILENAME_Y, tft.width(), PLAYER_ROW2_Y - PLAYER_FILENAME_Y, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(filename, 8, PLAYER_FILENAME_Y);

    tft.fillRect(0, PLAYER_ROW2_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString(STATE_LABEL, 8, PLAYER_ROW2_Y);

    // A fresh file: force every field below to draw for real on the
    // first tick rather than skipping a stale match -- see drawPlayer()'s
    // identical reasoning for g_lastTimeStr/g_lastStateStr.
    g_lastWavTimeStr[0] = '\0';
    g_lastWavStateStr[0] = '\0';
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     wavStateLabel(player.state(), stopped), g_lastWavStateStr, sizeof(g_lastWavStateStr));

    bool isError = player.state() == WavPlayer::STATE_ERROR;
    tft.fillRect(0, PLAYER_ROW3_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (isError) {
        // Same "error replaces everything below State" convention as
        // drawPlayer() -- there's no Time/Volume/format to show for a
        // file that never loaded.
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(player.errorMessage(), 8, PLAYER_ROW3_Y);
    } else {
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(TIME_LABEL, STAT_COL2_X, PLAYER_ROW2_Y);
        char timeStr[24];
        char elapsedBuf[16], totalBuf[16];
        formatDuration(player.elapsedMs(), elapsedBuf, sizeof(elapsedBuf));
        formatDuration(player.totalMs(), totalBuf, sizeof(totalBuf));
        snprintf(timeStr, sizeof(timeStr), "%s / %s", elapsedBuf, totalBuf);
        updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                         timeStr, g_lastWavTimeStr, sizeof(g_lastWavTimeStr));

        // Clears through to WAV_BAR_Y, not just ROW_HEIGHT -- the two are
        // a few px apart (a small gap before the bar), so clearing only
        // ROW_HEIGHT left that gap uncleared, showing leftover browser
        // text there. Same "clear through to the next row's Y, not just
        // this row's own height" fix drawPlayer() already uses for its
        // filename-to-State gap.
        tft.fillRect(0, PLAYER_ROW4_Y, tft.width(), WAV_BAR_Y - PLAYER_ROW4_Y, COLOR_BG);
        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", volume);
        drawStatRow(PLAYER_ROW4_Y, "", volBuf);

        drawWavProgressBar(player);

        // Same reasoning as the fillRect above -- clears from right after
        // the bar's own bottom edge (not just this row's own height), so
        // the small gap between the bar and this text doesn't go uncleared.
        tft.fillRect(0, WAV_BAR_Y + WAV_BAR_H, tft.width(), (WAV_FORMAT_Y + ROW_HEIGHT) - (WAV_BAR_Y + WAV_BAR_H), COLOR_BG);
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        char formatBuf[32];
        snprintf(formatBuf, sizeof(formatBuf), "%lu Hz  %u-bit  %s",
                 (unsigned long)player.sampleRate(), player.bitsPerSample(),
                 player.channels() == 2 ? "Stereo" : "Mono");
        tft.drawString(formatBuf, 8, WAV_FORMAT_Y);
    }

    int contentBottom = isError ? (PLAYER_ROW3_Y + ROW_HEIGHT) : (WAV_FORMAT_Y + ROW_HEIGHT);
    int fy = tft.height() - FOOTER_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // No ALT (no MIDI output target -- WAV never goes out a MIDI port)
    // and no bare-EDIT-tap action (WAV playback is always audible, no
    // separate on/off toggle) -- EDIT only ever acts as a hold-modifier
    // here, same weaving convention drawPlayer()'s footer uses.
    tft.drawString("EDIT+UD vol  PLAY tog", 4, fy + 1);
    tft.drawString("LEFT back  ENT+LR seek  NAV stop", 4, fy + 17);
    drawBatteryMeter();
}

void updateWavPlayerState(WavPlayer::State state, bool stopped) {
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     wavStateLabel(state, stopped), g_lastWavStateStr, sizeof(g_lastWavStateStr));
}

void updateWavPlayerLive(const WavPlayer& player) {
    // Same STATE_ERROR caveat as updatePlayerLive() -- callers must not
    // use this across a transition into/out of it.
    char timeStr[24];
    char elapsedBuf[16], totalBuf[16];
    formatDuration(player.elapsedMs(), elapsedBuf, sizeof(elapsedBuf));
    formatDuration(player.totalMs(), totalBuf, sizeof(totalBuf));
    snprintf(timeStr, sizeof(timeStr), "%s / %s", elapsedBuf, totalBuf);
    updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                     timeStr, g_lastWavTimeStr, sizeof(g_lastWavTimeStr));

    drawWavProgressBar(player);
}

// -- MOD player screen ---------------------------------------------------
//
// Same row-reuse convention as the WAV screen (PLAYER_FILENAME_Y/ROW2_Y/
// ROW4_Y), and Volume again sits at ROW4_Y's right column so
// handleVolumeHold() redraws it through the unchanged Ui::updateVolume()
// call. No progress bar -- a tracker has no fixed total duration (it can
// loop indefinitely, see ModPlayer's header comment) -- so Time is
// elapsed-only, and pattern/row position plus format info fill the space
// the WAV screen's bar/format-line would otherwise occupy.

char g_lastModTimeStr[16] = {0};
char g_lastModStateStr[16] = {0};
char g_lastModPosStr[24] = {0};

const int MOD_POS_Y = PLAYER_ROW5_Y;                 // "Pattern: N/M  Row: R"
const int MOD_FORMAT_Y = MOD_POS_Y + ROW_HEIGHT + 6; // format info line

const char* modStateLabel(ModPlayer::State s, bool stopped) {
    switch (s) {
        case ModPlayer::STATE_PLAYING: return "Playing";
        case ModPlayer::STATE_PAUSED:  return stopped ? "Stopped" : "Paused";
        case ModPlayer::STATE_DONE:    return "Finished";
        case ModPlayer::STATE_ERROR:   return "Error";
        default:                       return "Idle";
    }
}

void drawModPlayer(const char* filename, const ModPlayer& player, int volume, bool stopped) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Now Playing", 4, 3);
    drawHeaderBrand();

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), PLAYER_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);
    tft.fillRect(0, PLAYER_FILENAME_Y, tft.width(), PLAYER_ROW2_Y - PLAYER_FILENAME_Y, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(filename, 8, PLAYER_FILENAME_Y);

    tft.fillRect(0, PLAYER_ROW2_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString(STATE_LABEL, 8, PLAYER_ROW2_Y);

    // A fresh file: force every field below to draw for real on the
    // first tick -- same reasoning as drawWavPlayer()'s identical resets.
    g_lastModTimeStr[0] = '\0';
    g_lastModStateStr[0] = '\0';
    g_lastModPosStr[0] = '\0';
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     modStateLabel(player.state(), stopped), g_lastModStateStr, sizeof(g_lastModStateStr));

    bool isError = player.state() == ModPlayer::STATE_ERROR;
    tft.fillRect(0, PLAYER_ROW3_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (isError) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(player.errorMessage(), 8, PLAYER_ROW3_Y);
    } else {
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(TIME_LABEL, STAT_COL2_X, PLAYER_ROW2_Y);
        char timeStr[16];
        formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
        updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                         timeStr, g_lastModTimeStr, sizeof(g_lastModTimeStr));

        // Clears through to MOD_POS_Y, not just ROW_HEIGHT -- same "avoid
        // an uncleared gap" fix drawWavPlayer() needed for its own
        // Volume-row-to-bar gap.
        tft.fillRect(0, PLAYER_ROW4_Y, tft.width(), MOD_POS_Y - PLAYER_ROW4_Y, COLOR_BG);
        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", volume);
        drawStatRow(PLAYER_ROW4_Y, "", volBuf);

        tft.fillRect(0, MOD_POS_Y, tft.width(), (MOD_FORMAT_Y + ROW_HEIGHT) - MOD_POS_Y, COLOR_BG);
        char posBuf[24];
        snprintf(posBuf, sizeof(posBuf), "Pattern: %d/%d  Row: %d",
                 player.patternOrderPosition(), player.songLength(), player.currentRow());
        updateStatValue(8, MOD_POS_Y, posBuf, g_lastModPosStr, sizeof(g_lastModPosStr));

        char formatBuf[32];
        snprintf(formatBuf, sizeof(formatBuf), "%dch  %d instruments", player.channelCount(), player.instrumentCount());
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(formatBuf, 8, MOD_FORMAT_Y);
    }

    int contentBottom = isError ? (PLAYER_ROW3_Y + ROW_HEIGHT) : (MOD_FORMAT_Y + ROW_HEIGHT);
    int fy = tft.height() - FOOTER_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // No ALT (no MIDI target), no seek (ModPlayer doesn't support it, see
    // its header comment), no bare-EDIT-tap action -- same reasoning the
    // WAV screen's footer already documents.
    tft.drawString("EDIT+UD vol  PLAY tog", 4, fy + 1);
    tft.drawString("LEFT back  NAV stop", 4, fy + 17);
    drawBatteryMeter();
}

void updateModPlayerState(ModPlayer::State state, bool stopped) {
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     modStateLabel(state, stopped), g_lastModStateStr, sizeof(g_lastModStateStr));
}

void updateModPlayerLive(const ModPlayer& player) {
    char timeStr[16];
    formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
    updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                     timeStr, g_lastModTimeStr, sizeof(g_lastModTimeStr));

    char posBuf[24];
    snprintf(posBuf, sizeof(posBuf), "Pattern: %d/%d  Row: %d",
             player.patternOrderPosition(), player.songLength(), player.currentRow());
    updateStatValue(8, MOD_POS_Y, posBuf, g_lastModPosStr, sizeof(g_lastModPosStr));
}

// -- S3M player screen ---------------------------------------------------
//
// Identical layout/row-reuse convention to the MOD screen above (S3M is
// also a tracker format with no fixed total duration, see S3mPlayer's
// header comment) -- only the format info line's contents differ.

char g_lastS3mTimeStr[16] = {0};
char g_lastS3mStateStr[16] = {0};
char g_lastS3mPosStr[24] = {0};

const int S3M_POS_Y = PLAYER_ROW5_Y;                 // "Pattern: N/M  Row: R"
const int S3M_FORMAT_Y = S3M_POS_Y + ROW_HEIGHT + 6; // format info line

const char* s3mStateLabel(S3mPlayer::State s, bool stopped) {
    switch (s) {
        case S3mPlayer::STATE_PLAYING: return "Playing";
        case S3mPlayer::STATE_PAUSED:  return stopped ? "Stopped" : "Paused";
        case S3mPlayer::STATE_DONE:    return "Finished";
        case S3mPlayer::STATE_ERROR:   return "Error";
        default:                       return "Idle";
    }
}

void drawS3mPlayer(const char* filename, const S3mPlayer& player, int volume, bool stopped) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Now Playing", 4, 3);
    drawHeaderBrand();

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), PLAYER_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);
    tft.fillRect(0, PLAYER_FILENAME_Y, tft.width(), PLAYER_ROW2_Y - PLAYER_FILENAME_Y, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(filename, 8, PLAYER_FILENAME_Y);

    tft.fillRect(0, PLAYER_ROW2_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString(STATE_LABEL, 8, PLAYER_ROW2_Y);

    // A fresh file: force every field below to draw for real on the
    // first tick -- same reasoning as drawModPlayer()'s identical resets.
    g_lastS3mTimeStr[0] = '\0';
    g_lastS3mStateStr[0] = '\0';
    g_lastS3mPosStr[0] = '\0';
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     s3mStateLabel(player.state(), stopped), g_lastS3mStateStr, sizeof(g_lastS3mStateStr));

    bool isError = player.state() == S3mPlayer::STATE_ERROR;
    tft.fillRect(0, PLAYER_ROW3_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (isError) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(player.errorMessage(), 8, PLAYER_ROW3_Y);
    } else {
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(TIME_LABEL, STAT_COL2_X, PLAYER_ROW2_Y);
        char timeStr[16];
        formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
        updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                         timeStr, g_lastS3mTimeStr, sizeof(g_lastS3mTimeStr));

        // Same "clear through to the next content's Y" gap-fix as
        // drawModPlayer()/drawWavPlayer().
        tft.fillRect(0, PLAYER_ROW4_Y, tft.width(), S3M_POS_Y - PLAYER_ROW4_Y, COLOR_BG);
        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", volume);
        drawStatRow(PLAYER_ROW4_Y, "", volBuf);

        tft.fillRect(0, S3M_POS_Y, tft.width(), (S3M_FORMAT_Y + ROW_HEIGHT) - S3M_POS_Y, COLOR_BG);
        char posBuf[24];
        snprintf(posBuf, sizeof(posBuf), "Pattern: %d/%d  Row: %d",
                 player.patternOrderPosition(), player.songLength(), player.currentRow());
        updateStatValue(8, S3M_POS_Y, posBuf, g_lastS3mPosStr, sizeof(g_lastS3mPosStr));

        char formatBuf[32];
        snprintf(formatBuf, sizeof(formatBuf), "%dch  %d instruments", player.channelCount(), player.instrumentCount());
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(formatBuf, 8, S3M_FORMAT_Y);
    }

    int contentBottom = isError ? (PLAYER_ROW3_Y + ROW_HEIGHT) : (S3M_FORMAT_Y + ROW_HEIGHT);
    int fy = tft.height() - FOOTER_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // Same footer reasoning as drawModPlayer() -- no ALT, no seek, no
    // bare-EDIT-tap action.
    tft.drawString("EDIT+UD vol  PLAY tog", 4, fy + 1);
    tft.drawString("LEFT back  NAV stop", 4, fy + 17);
    drawBatteryMeter();
}

void updateS3mPlayerState(S3mPlayer::State state, bool stopped) {
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     s3mStateLabel(state, stopped), g_lastS3mStateStr, sizeof(g_lastS3mStateStr));
}

void updateS3mPlayerLive(const S3mPlayer& player) {
    char timeStr[16];
    formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
    updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                     timeStr, g_lastS3mTimeStr, sizeof(g_lastS3mTimeStr));

    char posBuf[24];
    snprintf(posBuf, sizeof(posBuf), "Pattern: %d/%d  Row: %d",
             player.patternOrderPosition(), player.songLength(), player.currentRow());
    updateStatValue(8, S3M_POS_Y, posBuf, g_lastS3mPosStr, sizeof(g_lastS3mPosStr));
}

// -- XM player screen -----------------------------------------------------
//
// Identical layout/row-reuse convention to the S3M screen above (XM is
// also a tracker format with no fixed total duration, see XmPlayer's
// header comment) -- only the format info line's contents differ.

char g_lastXmTimeStr[16] = {0};
char g_lastXmStateStr[16] = {0};
char g_lastXmPosStr[24] = {0};

const int XM_POS_Y = PLAYER_ROW5_Y;                // "Pattern: N/M  Row: R"
const int XM_FORMAT_Y = XM_POS_Y + ROW_HEIGHT + 6; // format info line

const char* xmStateLabel(XmPlayer::State s, bool stopped) {
    switch (s) {
        case XmPlayer::STATE_PLAYING: return "Playing";
        case XmPlayer::STATE_PAUSED:  return stopped ? "Stopped" : "Paused";
        case XmPlayer::STATE_DONE:    return "Finished";
        case XmPlayer::STATE_ERROR:   return "Error";
        default:                      return "Idle";
    }
}

void drawXmPlayer(const char* filename, const XmPlayer& player, int volume, bool stopped) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Now Playing", 4, 3);
    drawHeaderBrand();

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), PLAYER_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);
    tft.fillRect(0, PLAYER_FILENAME_Y, tft.width(), PLAYER_ROW2_Y - PLAYER_FILENAME_Y, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(filename, 8, PLAYER_FILENAME_Y);

    tft.fillRect(0, PLAYER_ROW2_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString(STATE_LABEL, 8, PLAYER_ROW2_Y);

    // A fresh file: force every field below to draw for real on the
    // first tick -- same reasoning as drawS3mPlayer()'s identical resets.
    g_lastXmTimeStr[0] = '\0';
    g_lastXmStateStr[0] = '\0';
    g_lastXmPosStr[0] = '\0';
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     xmStateLabel(player.state(), stopped), g_lastXmStateStr, sizeof(g_lastXmStateStr));

    bool isError = player.state() == XmPlayer::STATE_ERROR;
    tft.fillRect(0, PLAYER_ROW3_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (isError) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(player.errorMessage(), 8, PLAYER_ROW3_Y);
    } else {
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(TIME_LABEL, STAT_COL2_X, PLAYER_ROW2_Y);
        char timeStr[16];
        formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
        updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                         timeStr, g_lastXmTimeStr, sizeof(g_lastXmTimeStr));

        // Same "clear through to the next content's Y" gap-fix as
        // drawS3mPlayer()/drawModPlayer().
        tft.fillRect(0, PLAYER_ROW4_Y, tft.width(), XM_POS_Y - PLAYER_ROW4_Y, COLOR_BG);
        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", volume);
        drawStatRow(PLAYER_ROW4_Y, "", volBuf);

        tft.fillRect(0, XM_POS_Y, tft.width(), (XM_FORMAT_Y + ROW_HEIGHT) - XM_POS_Y, COLOR_BG);
        char posBuf[24];
        snprintf(posBuf, sizeof(posBuf), "Pattern: %d/%d  Row: %d",
                 player.patternOrderPosition(), player.songLength(), player.currentRow());
        updateStatValue(8, XM_POS_Y, posBuf, g_lastXmPosStr, sizeof(g_lastXmPosStr));

        // XM channel counts legitimately run up to 32 (vs S3M's ~16 in
        // practice) -- still just 2 digits, fits this row exactly like
        // S3M's identical format line does.
        char formatBuf[32];
        snprintf(formatBuf, sizeof(formatBuf), "%dch  %d instruments", player.channelCount(), player.instrumentCount());
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(formatBuf, 8, XM_FORMAT_Y);
    }

    int contentBottom = isError ? (PLAYER_ROW3_Y + ROW_HEIGHT) : (XM_FORMAT_Y + ROW_HEIGHT);
    int fy = tft.height() - FOOTER_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // Same footer reasoning as drawS3mPlayer() -- no ALT, no seek, no
    // bare-EDIT-tap action.
    tft.drawString("EDIT+UD vol  PLAY tog", 4, fy + 1);
    tft.drawString("LEFT back  NAV stop", 4, fy + 17);
    drawBatteryMeter();
}

void updateXmPlayerState(XmPlayer::State state, bool stopped) {
    updateStatValue(8 + tft.textWidth(STATE_LABEL), PLAYER_ROW2_Y,
                     xmStateLabel(state, stopped), g_lastXmStateStr, sizeof(g_lastXmStateStr));
}

void updateXmPlayerLive(const XmPlayer& player) {
    char timeStr[16];
    formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
    updateStatValue(STAT_COL2_X + tft.textWidth(TIME_LABEL), PLAYER_ROW2_Y,
                     timeStr, g_lastXmTimeStr, sizeof(g_lastXmTimeStr));

    char posBuf[24];
    snprintf(posBuf, sizeof(posBuf), "Pattern: %d/%d  Row: %d",
             player.patternOrderPosition(), player.songLength(), player.currentRow());
    updateStatValue(8, XM_POS_Y, posBuf, g_lastXmPosStr, sizeof(g_lastXmPosStr));
}

// Draws a single keyboard key at its grid position, highlighted or not.
// Used by both the full redraw and the cursor-move partial redraw.
void drawKeyboardKey(int row, int col, bool selected) {
    int cols = KEYBOARD_ROW_LENS[row];
    bool wideRow = (row == KEYBOARD_ROW_COUNT - 1);
    int cellW = wideRow ? KB_WIDE_KEY_W : KB_KEY_W;
    int rowWidth = cols * cellW;
    int startX = (tft.width() - rowWidth) / 2;
    int y = KB_START_Y + row * KB_ROW_SPACING;
    int x = startX + col * cellW;

    const KeyDef& key = KEYBOARD_ROWS[row][col];
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    uint16_t border = selected ? COLOR_ACCENT : COLOR_DIM;

    tft.fillRect(x, y, cellW - 2, KB_KEY_H, bg);
    tft.drawRect(x, y, cellW - 2, KB_KEY_H, border);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(key.kind == KEY_CHAR ? COLOR_TEXT : COLOR_ACCENT, bg);
    tft.drawString(key.label, x + (cellW - 2) / 2, y + KB_KEY_H / 2);
    tft.setTextDatum(TL_DATUM);
}

// Draws (or clears) the name-entry footer's second line: an error message
// if given, otherwise the DEL/OK/cancel hint. Used by both the full
// redraw and the standalone error-only partial redraw.
void drawNameEntryFooterLine2(const char* error) {
    int fy = tft.height() - FOOTER_HEIGHT;
    tft.fillRect(0, fy + 16, tft.width(), FOOTER_HEIGHT - 16, COLOR_BG);
    tft.setTextColor(error ? COLOR_ERROR : COLOR_DIM, COLOR_BG);
    tft.drawString(error ? error : "DEL/OK on keyboard  NAV cancel", 4, fy + 17);
    drawBatteryMeter();
}

void drawNameEntry(const char* title, const char* name, const char* suffix,
                    const char* error, int keyRow, int keyCol) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(title, 4, 3);

    // Header-to-preview gap: nothing else covers it now that the blanket
    // fillScreen() is gone (it caused a visible full-screen black flash
    // on every redraw, same fix as drawBrowser()'s/drawLooper()'s).
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), NAME_PREVIEW_Y - HEADER_HEIGHT, COLOR_BG);

    // Text preview: what's been typed so far (a trailing "_" stands in
    // for a text cursor -- entry is always append/backspace-at-the-end,
    // there's no mid-string insertion point to show).
    tft.fillRect(0, NAME_PREVIEW_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    char preview[MAX_FILENAME_LEN + 8];
    snprintf(preview, sizeof(preview), "%s%s_", name, (suffix && suffix[0]) ? suffix : "");
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(preview, 8, NAME_PREVIEW_Y);

    int keyboardTop = NAME_PREVIEW_Y + ROW_HEIGHT;
    if (KB_START_Y > keyboardTop) {
        tft.fillRect(0, keyboardTop, tft.width(), KB_START_Y - keyboardTop, COLOR_BG);
    }
    for (int row = 0; row < KEYBOARD_ROW_COUNT; row++) {
        // Individual keys are centered and narrower than the screen, with
        // small gaps around and between them -- clear the row's full
        // width first (covers the margins and inter-key gaps in one go)
        // rather than trying to enumerate each sliver separately.
        int rowY = KB_START_Y + row * KB_ROW_SPACING;
        tft.fillRect(0, rowY, tft.width(), KB_ROW_SPACING, COLOR_BG);
        for (int col = 0; col < KEYBOARD_ROW_LENS[row]; col++) {
            drawKeyboardKey(row, col, row == keyRow && col == keyCol);
        }
    }

    int fy = tft.height() - FOOTER_HEIGHT;
    int keyboardBottom = KB_START_Y + KEYBOARD_ROW_COUNT * KB_ROW_SPACING;
    if (fy > keyboardBottom) {
        tft.fillRect(0, keyboardBottom, tft.width(), fy - keyboardBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), 16, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("ARROWS move  ENTER select", 4, fy + 1);
    drawNameEntryFooterLine2(error);
}

void updateNameEntryKey(int prevRow, int prevCol, int newRow, int newCol) {
    drawKeyboardKey(prevRow, prevCol, false);
    drawKeyboardKey(newRow, newCol, true);
}

void updateNameEntryPreview(const char* name, const char* suffix) {
    char preview[MAX_FILENAME_LEN + 8];
    snprintf(preview, sizeof(preview), "%s%s_", name, (suffix && suffix[0]) ? suffix : "");
    tft.fillRect(0, NAME_PREVIEW_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(preview, 8, NAME_PREVIEW_Y);
}

void updateNameEntryError(const char* error) {
    drawNameEntryFooterLine2(error);
}

// Draws a single entry-menu row, highlighted or not. Used by both the
// full redraw and the cursor-move partial redraw.
void drawMenuRow(const char* label, int row, bool selected) {
    int y = HEADER_HEIGHT + 6 + row * ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(0, y, tft.width(), ROW_HEIGHT, bg);
    tft.setTextColor(COLOR_TEXT, bg);
    tft.drawString(label, 12, y + 2);
}

void drawModeSelect(const char* const* labels, int count, int cursor) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("midiTracker", 4, 3);

    // Header-to-first-row gap, and everything below the last item down to
    // the footer, aren't covered by any row's own draw -- cleared
    // explicitly rather than relying on a blanket fillScreen() (removed:
    // it caused a visible full-screen black flash on every redraw, same
    // fix as drawBrowser()'s/drawLooper()'s).
    int rowsTop = HEADER_HEIGHT + 6;
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), 6, COLOR_BG);
    int fy = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = rowsTop + count * ROW_HEIGHT;
    if (rowsBottom < fy) {
        tft.fillRect(0, rowsBottom, tft.width(), fy - rowsBottom, COLOR_BG);
    }

    for (int i = 0; i < count; i++) {
        drawMenuRow(labels[i], i, i == cursor);
    }

    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("UP/DN move  ENTER select", 4, fy + 1);
    drawBatteryMeter();
}

void updateModeSelectCursor(const char* const* labels, int count, int prevCursor, int newCursor) {
    if (prevCursor >= 0 && prevCursor < count) drawMenuRow(labels[prevCursor], prevCursor, false);
    if (newCursor >= 0 && newCursor < count) drawMenuRow(labels[newCursor], newCursor, true);
}

void drawEntryMenu(const char* subtitle, const char* const* labels, int count, int cursor) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(subtitle, 4, 3);

    // Same gaps as drawModeSelect() -- see its comment.
    int rowsTop = HEADER_HEIGHT + 6;
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), 6, COLOR_BG);
    int fy = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = rowsTop + count * ROW_HEIGHT;
    if (rowsBottom < fy) {
        tft.fillRect(0, rowsBottom, tft.width(), fy - rowsBottom, COLOR_BG);
    }

    for (int i = 0; i < count; i++) {
        drawMenuRow(labels[i], i, i == cursor);
    }

    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("UP/DN move  ENTER select  NAV cancel", 4, fy + 1);
    drawBatteryMeter();
}

void updateEntryMenuSelection(const char* const* labels, int count, int prevCursor, int newCursor) {
    if (prevCursor >= 0 && prevCursor < count) drawMenuRow(labels[prevCursor], prevCursor, false);
    if (newCursor >= 0 && newCursor < count) drawMenuRow(labels[newCursor], newCursor, true);
}

void drawConfirmDelete(const char* name, bool isDir, bool failed) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Delete", 4, 3);

    // Only two short lines of text on an otherwise-empty body -- clear
    // each row (and the header-to-first-row gap) right before drawing
    // into it rather than relying on a blanket fillScreen() (removed: it
    // caused a visible full-screen black flash on every redraw, same fix
    // as drawBrowser()'s/drawLooper()'s).
    int y = HEADER_HEIGHT + 16;
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), y - HEADER_HEIGHT, COLOR_BG);

    char line[MAX_FILENAME_LEN + 16];
    tft.fillRect(0, y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (failed) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        snprintf(line, sizeof(line), "Could not delete %s", name);
        tft.drawString(line, 8, y);
    } else {
        tft.setTextColor(COLOR_TEXT, COLOR_BG);
        snprintf(line, sizeof(line), "Delete %s %s?", isDir ? "folder" : "file", name);
        tft.drawString(line, 8, y);
    }
    y += ROW_HEIGHT;

    tft.fillRect(0, y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (failed) {
        tft.setTextColor(COLOR_DIM, COLOR_BG);
        tft.drawString(isDir ? "(folder not empty?)" : "", 8, y);
    } else {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString("This cannot be undone.", 8, y);
    }
    y += ROW_HEIGHT;

    int fy = tft.height() - FOOTER_HEIGHT;
    if (fy > y) {
        tft.fillRect(0, y, tft.width(), fy - y, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString(failed ? "NAV/ENTER back" : "ENTER delete  NAV cancel", 4, fy + 1);
    drawBatteryMeter();
}

void drawRecording(const char* filename, const MidiRecorder& recorder) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Recording", 4, 3);

    // Same reasoning as drawPlayer()/drawConfirmDelete(): clear each row
    // (and the gaps between them) right before drawing into it, rather
    // than relying on a blanket fillScreen() (removed: it caused a
    // visible full-screen black flash on every redraw).
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), REC_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);

    tft.fillRect(0, REC_FILENAME_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    char nameBuf[MAX_FILENAME_LEN + 4];
    snprintf(nameBuf, sizeof(nameBuf), "%s.mid", filename);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(nameBuf, 8, REC_FILENAME_Y);

    int gapTop = REC_FILENAME_Y + ROW_HEIGHT;
    if (REC_STATE_Y > gapTop) {
        tft.fillRect(0, gapTop, tft.width(), REC_STATE_Y - gapTop, COLOR_BG);
    }

    tft.fillRect(0, REC_STATE_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    char buf[48];
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    snprintf(buf, sizeof(buf), "State: %s", recordStateLabel(recorder.state()));
    tft.drawString(buf, 8, REC_STATE_Y);

    tft.fillRect(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (recorder.state() == MidiRecorder::STATE_ERROR) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(recorder.errorMessage(), 8, REC_ELAPSED_Y);
        tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    } else {
        char timeStr[16];
        formatDuration(recorder.elapsedMs(), timeStr, sizeof(timeStr));
        snprintf(buf, sizeof(buf), "Elapsed: %s", timeStr);
        tft.drawString(buf, 8, REC_ELAPSED_Y);

        tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        snprintf(buf, sizeof(buf), "Events: %lu", (unsigned long)recorder.eventCount());
        tft.drawString(buf, 8, REC_EVENTS_Y);
        drawActivityDot(tft.width() - 16, REC_EVENTS_Y + ROW_HEIGHT / 2,
                         recorder.msSinceLastEvent() < RECORDING_ACTIVITY_MS);
    }

    int fy = tft.height() - FOOTER_HEIGHT;
    int contentBottom = REC_EVENTS_Y + ROW_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("NAV/LEFT stop & save  EDIT discard", 4, fy + 1);
    drawBatteryMeter();
}

void updateRecordingLive(const MidiRecorder& recorder) {
    // Only meaningful (and only called) while STATE_RECORDING, so the
    // fixed non-error layout offsets from drawRecording() apply here.
    char buf[48];
    tft.setTextColor(COLOR_DIM, COLOR_BG);

    char timeStr[16];
    formatDuration(recorder.elapsedMs(), timeStr, sizeof(timeStr));
    snprintf(buf, sizeof(buf), "Elapsed: %s", timeStr);
    tft.fillRect(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.drawString(buf, 8, REC_ELAPSED_Y);

    snprintf(buf, sizeof(buf), "Events: %lu", (unsigned long)recorder.eventCount());
    tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.drawString(buf, 8, REC_EVENTS_Y);
    drawActivityDot(tft.width() - 16, REC_EVENTS_Y + ROW_HEIGHT / 2,
                     recorder.msSinceLastEvent() < RECORDING_ACTIVITY_MS);
}

void drawSysExCapture(const char* filename, const SysExRecorder& recorder) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Capture SysEx", 4, 3);
    drawHeaderBrand();

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), REC_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);

    tft.fillRect(0, REC_FILENAME_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    char nameBuf[MAX_FILENAME_LEN + 4];
    snprintf(nameBuf, sizeof(nameBuf), "%s.syx", filename);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(nameBuf, 8, REC_FILENAME_Y);

    int gapTop = REC_FILENAME_Y + ROW_HEIGHT;
    if (REC_STATE_Y > gapTop) {
        tft.fillRect(0, gapTop, tft.width(), REC_STATE_Y - gapTop, COLOR_BG);
    }

    tft.fillRect(0, REC_STATE_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    char buf[48];
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    snprintf(buf, sizeof(buf), "State: %s", sysExCaptureStateLabel(recorder.state()));
    tft.drawString(buf, 8, REC_STATE_Y);

    tft.fillRect(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (recorder.state() == SysExRecorder::STATE_ERROR) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(recorder.errorMessage(), 8, REC_ELAPSED_Y);
        tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    } else {
        char timeStr[16];
        formatDuration(recorder.elapsedMs(), timeStr, sizeof(timeStr));
        snprintf(buf, sizeof(buf), "Elapsed: %s", timeStr);
        tft.drawString(buf, 8, REC_ELAPSED_Y);

        tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        snprintf(buf, sizeof(buf), "Messages: %lu", (unsigned long)recorder.messageCount());
        tft.drawString(buf, 8, REC_EVENTS_Y);
        drawActivityDot(tft.width() - 16, REC_EVENTS_Y + ROW_HEIGHT / 2,
                              recorder.msSinceLastMessage() < RECORDING_ACTIVITY_MS);
    }

    int fy = tft.height() - FOOTER_HEIGHT;
    int contentBottom = REC_EVENTS_Y + ROW_HEIGHT;
    if (fy > contentBottom) {
        tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
    }
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("NAV/LEFT stop & save  EDIT discard", 4, fy + 1);
    drawBatteryMeter();
}

void updateSysExCaptureLive(const SysExRecorder& recorder) {
    // Only meaningful (and only called) while STATE_RECORDING, so the
    // fixed non-error layout offsets from drawSysExCapture() apply here.
    char buf[48];
    tft.setTextColor(COLOR_DIM, COLOR_BG);

    char timeStr[16];
    formatDuration(recorder.elapsedMs(), timeStr, sizeof(timeStr));
    snprintf(buf, sizeof(buf), "Elapsed: %s", timeStr);
    tft.fillRect(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.drawString(buf, 8, REC_ELAPSED_Y);

    snprintf(buf, sizeof(buf), "Messages: %lu", (unsigned long)recorder.messageCount());
    tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.drawString(buf, 8, REC_EVENTS_Y);
    drawActivityDot(tft.width() - 16, REC_EVENTS_Y + ROW_HEIGHT / 2,
                          recorder.msSinceLastMessage() < RECORDING_ACTIVITY_MS);
}

void drawSysExPlayer(const char* filename, const SysExPlayer& player) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("SysEx Playback", 4, 3);
    drawHeaderBrand();

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), REC_FILENAME_Y - HEADER_HEIGHT, COLOR_BG);

    tft.fillRect(0, REC_FILENAME_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(filename, 8, REC_FILENAME_Y);

    int gapTop = REC_FILENAME_Y + ROW_HEIGHT;
    if (REC_STATE_Y > gapTop) {
        tft.fillRect(0, gapTop, tft.width(), REC_STATE_Y - gapTop, COLOR_BG);
    }

    tft.fillRect(0, REC_STATE_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    char buf[48];
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    snprintf(buf, sizeof(buf), "State: %s", sysExPlayerStateLabel(player.state()));
    tft.drawString(buf, 8, REC_STATE_Y);

    tft.fillRect(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    if (player.state() == SysExPlayer::STATE_ERROR) {
        tft.setTextColor(COLOR_ERROR, COLOR_BG);
        tft.drawString(player.errorMessage(), 8, REC_ELAPSED_Y);
        tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        int fy = tft.height() - FOOTER_HEIGHT;
        int contentBottom = REC_EVENTS_Y + ROW_HEIGHT;
        if (fy > contentBottom) {
            tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
        }
    } else {
        char timeStr[16];
        formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
        snprintf(buf, sizeof(buf), "Elapsed: %s", timeStr);
        tft.drawString(buf, 8, REC_ELAPSED_Y);

        tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
        snprintf(buf, sizeof(buf), "Messages: %lu", (unsigned long)player.messagesSent());
        tft.drawString(buf, 8, REC_EVENTS_Y);

        drawSysExProgress(player);
    }

    int fy = tft.height() - FOOTER_HEIGHT;
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("NAV/LEFT stop", 4, fy + 1);
    drawBatteryMeter();
}

void updateSysExPlayerLive(const SysExPlayer& player) {
    // Only meaningful (and only called) while STATE_SENDING, so the fixed
    // non-error layout offsets from drawSysExPlayer() apply here.
    char buf[48];
    tft.setTextColor(COLOR_DIM, COLOR_BG);

    char timeStr[16];
    formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
    snprintf(buf, sizeof(buf), "Elapsed: %s", timeStr);
    tft.fillRect(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.drawString(buf, 8, REC_ELAPSED_Y);

    snprintf(buf, sizeof(buf), "Messages: %lu", (unsigned long)player.messagesSent());
    tft.fillRect(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, COLOR_BG);
    tft.drawString(buf, 8, REC_EVENTS_Y);

    drawSysExProgress(player);
}

// Looper screen layout: header, then 4 fixed-height track rows (label
// line + a thin progress bar), then a status line, then footer. The label
// line uses fixed x columns (track number / channel / state) rather than
// one concatenated string, so the channel field can be redrawn on its own
// (see drawLooperTrackChannel()) without needing to know how wide the
// surrounding text renders in this proportional font.
const int LOOPER_ROW_HEIGHT = 40;
const int LOOPER_FIRST_ROW_Y = HEADER_HEIGHT + 4;
const int LOOPER_BAR_HEIGHT = 6;
const int LOOPER_BAR_Y_OFFSET = 22; // bar sits below the label line within a row
const int LOOPER_STATUS_Y = LOOPER_FIRST_ROW_Y + 4 * LOOPER_ROW_HEIGHT + 2;
const int LOOPER_TRACK_COL_X = 8;
const int LOOPER_CHANNEL_COL_X = 32;   // wide enough for "T4" plus a gap
const int LOOPER_STATE_COL_X = 88;     // wide enough for "Ch16"/"OMNI" plus a gap
const int LOOPER_BARLEN_COL_X = 165;   // wide enough for "RECORDING!" (longest state + overflow marker) plus a gap
const int LOOPER_ACTIVITY_X = 222;     // wide enough for "128BAR" (longest bar-length value) plus a gap
const int LOOPER_ACTIVITY_SPACING = 14; // gap between the record and play activity dots

const char* loopStateLabel(LoopTrackState s) {
    switch (s) {
        case LOOP_TRACK_ARMED:     return "ARMED";
        case LOOP_TRACK_RECORDING: return "RECORDING";
        case LOOP_TRACK_PLAYING:   return "PLAYING";
        case LOOP_TRACK_MUTED:     return "MUTED";
        case LOOP_TRACK_STOPPED:   return "STOPPED";
        case LOOP_TRACK_PAUSED:    return "PAUSED";
        default:                   return "EMPTY";
    }
}

uint16_t loopStateColor(LoopTrackState s) {
    switch (s) {
        case LOOP_TRACK_ARMED:     return TFT_YELLOW;
        case LOOP_TRACK_RECORDING: return TFT_RED;
        case LOOP_TRACK_PLAYING:   return TFT_GREEN;
        case LOOP_TRACK_MUTED:     return COLOR_DIM;
        case LOOP_TRACK_STOPPED:   return TFT_ORANGE;
        case LOOP_TRACK_PAUSED:    return TFT_CYAN;
        default:                   return COLOR_DIM;
    }
}

// "ChNN" (1-16) or "OMNI" -- shared by drawLooperTrackLabel() (full row)
// and drawLooperTrackChannel() (channel-only partial update).
void looperChannelText(char* buf, size_t bufSize, const LoopTrackView& t) {
    if (t.channel >= 16) snprintf(buf, bufSize, "OMNI");
    else snprintf(buf, bufSize, "Ch%02d", t.channel + 1);
}

// "FREE" or "NBAR" (e.g. "1BAR", "128BAR") -- shared by drawLooperTrackLabel()
// (full row) and drawLooperTrackBarLength() (bar-length-only partial update).
void looperBarLenText(char* buf, size_t bufSize, const LoopTrackView& t) {
    if (t.barLength == 0) snprintf(buf, bufSize, "FREE");
    else snprintf(buf, bufSize, "%dBAR", t.barLength);
}

// Redraws just the length field of track idx's row (e.g. after a BPM
// change rescales a bar-quantized track's effective length) -- clears a
// fixed-width box at the row's right edge, same approach as
// drawLooperTrackChannel()/drawLooperTrackBarLength(), rather than
// needing to know how wide the value renders.
void drawLooperTrackLength(const LoopTrackView& t, int idx, bool selected) {
    int y = LOOPER_FIRST_ROW_Y + idx * LOOPER_ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    int boxX = tft.width() - 72; // generous width for "NNN.Ns"
    tft.fillRect(boxX, y, tft.width() - boxX, ROW_HEIGHT, bg);

    if (t.lengthMs == 0) return;
    char lenBuf[16];
    snprintf(lenBuf, sizeof(lenBuf), "%lu.%01lus", (unsigned long)(t.lengthMs / 1000),
             (unsigned long)((t.lengthMs / 100) % 10));
    tft.setTextColor(loopStateColor(t.state), bg);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(lenBuf, tft.width() - 8, y + 2);
    tft.setTextDatum(TL_DATUM);
}

// Draws one track's label line (row background, "T1 / Ch03 / PLAYING" in
// their fixed columns, selection highlight) -- everything in the row
// except the progress bar and activity dot, which updateLooperPositions()
// owns separately so they can be refreshed on their own at a faster tick
// without re-drawing this text.
void drawLooperTrackLabel(const LoopTrackView& t, int idx, bool selected) {
    int y = LOOPER_FIRST_ROW_Y + idx * LOOPER_ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(0, y, tft.width(), LOOPER_ROW_HEIGHT - 4, bg);
    // The trailing 4px between this row and the next is a deliberate gap
    // (breathing room, and where a selected row's highlight visibly ends
    // before the next row starts) -- always background regardless of
    // `selected`, and cleared here explicitly (not left to a caller's
    // fillScreen()) so this row is fully self-contained for both the full
    // screen redraw and a standalone partial update.
    tft.fillRect(0, y + LOOPER_ROW_HEIGHT - 4, tft.width(), 4, COLOR_BG);

    tft.setTextColor(loopStateColor(t.state), bg);
    tft.setTextDatum(TL_DATUM);

    char trackBuf[4];
    snprintf(trackBuf, sizeof(trackBuf), "T%d", idx + 1);
    tft.drawString(trackBuf, LOOPER_TRACK_COL_X, y + 2);

    char chBuf[8];
    looperChannelText(chBuf, sizeof(chBuf), t);
    tft.drawString(chBuf, LOOPER_CHANNEL_COL_X, y + 2);

    // "!" appended when the event buffer filled up and further events are
    // being silently dropped -- surfaced rather than left invisible.
    char stateBuf[12];
    snprintf(stateBuf, sizeof(stateBuf), t.bufferFull ? "%s!" : "%s", loopStateLabel(t.state));
    tft.drawString(stateBuf, LOOPER_STATE_COL_X, y + 2);

    char barBuf[10];
    looperBarLenText(barBuf, sizeof(barBuf), t);
    tft.drawString(barBuf, LOOPER_BARLEN_COL_X, y + 2);

    drawLooperTrackLength(t, idx, selected);
}

// Redraws just the channel field of track idx's row (e.g. after ALT+UP/
// DOWN changes it) -- clears a fixed-width box at LOOPER_CHANNEL_COL_X,
// comfortably wide enough for any "ChNN"/"OMNI" value, rather than the
// whole row. Safe to call on its own since the channel column never
// overlaps the track number or state text next to it.
void drawLooperTrackChannel(const LoopTrackView& t, int idx, bool selected) {
    int y = LOOPER_FIRST_ROW_Y + idx * LOOPER_ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(LOOPER_CHANNEL_COL_X, y, LOOPER_STATE_COL_X - LOOPER_CHANNEL_COL_X - 4, ROW_HEIGHT, bg);

    char chBuf[8];
    looperChannelText(chBuf, sizeof(chBuf), t);
    tft.setTextColor(loopStateColor(t.state), bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(chBuf, LOOPER_CHANNEL_COL_X, y + 2);
}

// Redraws just the bar-length field of track idx's row (e.g. after ALT+
// LEFT/RIGHT changes it) -- same approach as drawLooperTrackChannel(): a
// fixed-width box at LOOPER_BARLEN_COL_X, comfortably wide enough for any
// "FREE"/"NBAR" value, so it never collides with the state text or
// activity dots on either side.
void drawLooperTrackBarLength(const LoopTrackView& t, int idx, bool selected) {
    int y = LOOPER_FIRST_ROW_Y + idx * LOOPER_ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(LOOPER_BARLEN_COL_X, y, LOOPER_ACTIVITY_X - LOOPER_BARLEN_COL_X - 4, ROW_HEIGHT, bg);

    char barBuf[10];
    looperBarLenText(barBuf, sizeof(barBuf), t);
    tft.setTextColor(loopStateColor(t.state), bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(barBuf, LOOPER_BARLEN_COL_X, y + 2);
}

// Draws just track idx's progress bar at its fixed position within the
// row -- a dim background track the full row width, filled proportionally
// to positionFrac. Always safe to call standalone (e.g. every ~100ms)
// since it never touches the label line above it.
void drawLooperTrackBar(const LoopTrackView& t, int idx) {
    int y = LOOPER_FIRST_ROW_Y + idx * LOOPER_ROW_HEIGHT + LOOPER_BAR_Y_OFFSET;
    int barW = tft.width() - 16;
    tft.fillRect(8, y, barW, LOOPER_BAR_HEIGHT, COLOR_DIM);
    // No lengthMs==0 guard here: positionFrac() already reports 0 for
    // every state where there's genuinely nothing to show (empty, armed,
    // freeform recording with no target yet). But a bar-quantized track's
    // open-ended first pass also has lengthMs==0 right up until it
    // auto-commits, and positionFrac() *does* compute real progress
    // toward its target length in that case -- bailing out here on
    // lengthMs alone would silently discard it.
    int fillW = (int)(barW * t.positionFrac);
    if (fillW > 0) tft.fillRect(8, y, fillW, LOOPER_BAR_HEIGHT, loopStateColor(t.state));
}

// Draws (or clears) track idx's two MIDI-activity dots -- record (red)
// and play (green), each lit briefly whenever the track actually
// captures/sends an event, matching the same colors as the RECORDING/
// PLAYING state text. Separate dots because a track mid-overdub does
// both at once (see LoopTrackView::recordActive/playActive) and one
// shared indicator couldn't show that. `selected` must match whatever
// drawLooperTrackLabel() last used for this row, so the "off" color
// matches the row's background.
void drawLooperTrackActivity(const LoopTrackView& t, int idx, bool selected) {
    int y = LOOPER_FIRST_ROW_Y + idx * LOOPER_ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillCircle(LOOPER_ACTIVITY_X, y + 8, 4, t.recordActive ? TFT_RED : bg);
    tft.fillCircle(LOOPER_ACTIVITY_X + LOOPER_ACTIVITY_SPACING, y + 8, 4, t.playActive ? TFT_GREEN : bg);
}

// The BPM row: a 5th, single-line item below the 4 track rows, selectable
// by the same row cursor (see LooperMode's onBpmRow) so BPM/Time
// Signature/Sync/Metronome/Count In -- shared, global settings, not
// per-track ones -- live directly in the main list instead of a separate
// menu screen. Highlighted the same way a selected track row is.
//
// Metro and Count In sit between the time signature and Sync -- their own
// text color is the on/off indicator (dim = off, full-bright = on, same
// convention dim-vs-bright text uses everywhere else in this app); Sync
// (right-aligned) uses the same coloring rather than swapping its label
// text to "Independent" when off -- a fixed "Sync" label dim vs. bright
// reads at a glance the same way every other on/off field on this row
// already does, without needing the row to make room for a second, longer
// word. Whichever of the five fields currently has focus
// (RIGHT/LEFT step through BPM -> Time Sig -> Metro -> Count In -> Sync
// and back -- see LooperMode's handleBpmRowFocus()) is shown inverted
// instead: a small filled box in the field's own on/off color with the
// row's own background color as the text, so it's unambiguous which one
// ALT+UP/DOWN (Metro/Count In/Sync) or EDIT+UP/DOWN/LEFT/RIGHT (BPM
// value/Time Sig) currently targets, without needing a third color whose
// contrast against dim/bright text can't be verified without hardware in
// hand. BPM value and Time Sig (focusIndex 0/1) get the same
// inverted-box treatment in COLOR_TEXT (neither has an on/off state of
// its own to color by) -- besides matching the other three fields, it's
// what makes landing on the BPM row from a track row above/below read as
// "now editing something" rather than just "row selected", since without
// it the row-level highlight alone looks identical to a plain selected
// track row.
//
// BPM value/Time Sig/Metro/Count In are laid out left-to-right from a
// running x cursor (each draw call below returns its own rendered width)
// rather than fixed pixel columns -- the row is always redrawn as a whole
// on any change (never just one field in place), so there's no benefit to
// fixed columns, and it means the layout self-adjusts if a field's text
// ever changes length (e.g. BPM gaining/losing a digit) instead of
// needing hand-tuned constants re-verified on hardware. Sync stays
// right-aligned, independent of that run.
const char* LOOPER_METRONOME_LABEL = "Metro";
const char* LOOPER_COUNTIN_LABEL = "Count";
const int LOOPER_FIELD_GAP = 10;

// A "value" field (not on/off) at x: BPM's own numeric readout, or the
// time signature -- always shown in COLOR_TEXT (there's no dim/off state
// for either), same inverted-box focus treatment as the on/off fields
// below. Returns the field's rendered width (text + padding) so the
// caller can lay out the next field immediately after it.
int drawLooperBpmValueField(int x, const char* text, bool focused, uint16_t rowBg) {
    uint16_t fg = focused ? rowBg : COLOR_TEXT;
    uint16_t fieldBg = focused ? COLOR_TEXT : rowBg;
    int w = tft.textWidth(text) + 6;
    if (focused) {
        tft.fillRect(x - 3, LOOPER_STATUS_Y, w, ROW_HEIGHT, fieldBg);
    }
    tft.setTextColor(fg, fieldBg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(text, x, LOOPER_STATUS_Y);
    return w;
}

// Shared by the Metro and Count In fields (both left-aligned, on/off
// style) -- see drawLooperBpmRow()'s comment for the inversion scheme.
// Returns the field's rendered width, same reason as
// drawLooperBpmValueField()'s.
int drawLooperBpmField(int x, const char* label, bool on, bool focused, uint16_t rowBg) {
    uint16_t color = on ? COLOR_TEXT : COLOR_DIM;
    uint16_t fg = focused ? rowBg : color;
    uint16_t fieldBg = focused ? color : rowBg;
    int w = tft.textWidth(label) + 6;
    if (focused) {
        tft.fillRect(x - 3, LOOPER_STATUS_Y, w, ROW_HEIGHT, fieldBg);
    }
    tft.setTextColor(fg, fieldBg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(label, x, LOOPER_STATUS_Y);
    return w;
}

// focusIndex: 0 = BPM value, 1 = Time Sig, 2 = Metro, 3 = Count In,
// 4 = Sync -- see drawLooper()'s doc comment. Meaningless unless
// `selected` is also true. `showMetronomeVolume`/`showCountInBars` swap
// Metro's/Count In's own label for `metronomeVolumePercent`/`countInBars`
// while EDIT is held with that field focused, so adjusting it
// (handleMetronomeVolumeAdjust()/handleCountInBarsAdjust()) shows what's
// actually changing instead of the field just sitting there unreadably
// static under a value the user can't see -- meaningless unless
// focusIndex is 2 or 3 respectively (that field already has to have
// focus for this to make sense).
void drawLooperBpmRow(float bpm, int timeSigNum, int timeSigDen, bool syncMode, bool metronomeOn,
                       int metronomeVolumePercent, bool showMetronomeVolume, bool countInEnabled,
                       int countInBars, bool showCountInBars, int focusIndex, bool selected) {
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(0, LOOPER_STATUS_Y, tft.width(), ROW_HEIGHT, bg);

    int x = 8;
    char bpmBuf[16];
    snprintf(bpmBuf, sizeof(bpmBuf), "BPM: %d", (int)(bpm + 0.5f));
    x += drawLooperBpmValueField(x, bpmBuf, selected && focusIndex == 0, bg) + LOOPER_FIELD_GAP;

    char sigBuf[8];
    snprintf(sigBuf, sizeof(sigBuf), "%d/%d", timeSigNum, timeSigDen);
    x += drawLooperBpmValueField(x, sigBuf, selected && focusIndex == 1, bg) + LOOPER_FIELD_GAP;

    char metroVolBuf[8];
    const char* metroLabel = LOOPER_METRONOME_LABEL;
    if (showMetronomeVolume) {
        snprintf(metroVolBuf, sizeof(metroVolBuf), "%d%%", metronomeVolumePercent);
        metroLabel = metroVolBuf;
    }
    x += drawLooperBpmField(x, metroLabel, metronomeOn, selected && focusIndex == 2, bg) + LOOPER_FIELD_GAP;

    char countInBarsBuf[8];
    const char* countInLabel = LOOPER_COUNTIN_LABEL;
    if (showCountInBars) {
        snprintf(countInBarsBuf, sizeof(countInBarsBuf), "%d bar%s", countInBars, countInBars == 1 ? "" : "s");
        countInLabel = countInBarsBuf;
    }
    drawLooperBpmField(x, countInLabel, countInEnabled, selected && focusIndex == 3, bg);

    const char* syncLabel = "Sync";
    uint16_t syncColor = syncMode ? COLOR_TEXT : COLOR_DIM;
    bool syncFocused = selected && focusIndex == 4;
    uint16_t syncFg = syncFocused ? bg : syncColor;
    uint16_t syncBg = syncFocused ? syncColor : bg;
    if (syncFocused) {
        int w = tft.textWidth(syncLabel) + 6;
        tft.fillRect(tft.width() - 8 - w, LOOPER_STATUS_Y, w, ROW_HEIGHT, syncBg);
    }
    tft.setTextColor(syncFg, syncBg);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(syncLabel, tft.width() - 8, LOOPER_STATUS_Y);
    tft.setTextDatum(TL_DATUM);
}

void drawLooper(const LoopTrackView tracksArg[4], int selectedTrack, bool onBpmRow, bool syncMode, float bpm,
                 int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                 bool showMetronomeVolume, bool countInEnabled, int countInBars, bool showCountInBars,
                 int focusIndex, int metronomeCurrentBeat) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("MIDI Looper", 4, 3);
    drawHeaderBrand();
    drawLooperBeatIndicator(metronomeOn, metronomeCurrentBeat, timeSigNum);

    // Small gaps nothing else here covers -- above the first track row,
    // between the last track row and the BPM row, and between the BPM
    // row and the footer -- cleared explicitly rather than relying on a
    // blanket fillScreen() (removed: it caused a visible full-screen
    // black flash on every redraw, same fix as drawBrowser()'s). Each
    // row's own internal trailing gap is drawLooperTrackLabel()'s job.
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), LOOPER_FIRST_ROW_Y - HEADER_HEIGHT, COLOR_BG);
    tft.fillRect(0, LOOPER_FIRST_ROW_Y + 4 * LOOPER_ROW_HEIGHT, tft.width(),
                 LOOPER_STATUS_Y - (LOOPER_FIRST_ROW_Y + 4 * LOOPER_ROW_HEIGHT), COLOR_BG);
    tft.fillRect(0, LOOPER_STATUS_Y + ROW_HEIGHT, tft.width(),
                 (tft.height() - FOOTER_HEIGHT) - (LOOPER_STATUS_Y + ROW_HEIGHT), COLOR_BG);

    for (int i = 0; i < 4; i++) {
        bool selected = !onBpmRow && (i == selectedTrack);
        drawLooperTrackLabel(tracksArg[i], i, selected);
        drawLooperTrackBar(tracksArg[i], i);
        drawLooperTrackActivity(tracksArg[i], i, selected);
    }
    drawLooperBpmRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                      showMetronomeVolume, countInEnabled, countInBars, showCountInBars, focusIndex, onBpmRow);

    int fy = tft.height() - FOOTER_HEIGHT;
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    // Row-grouped (see pins.h): line 1 = UP/PLAY/EDIT, line 2 = LEFT/
    // DOWN/RIGHT/ENTER then ALT/NAV. ALT is woven into UD's and LR's own
    // hints (channel/bar-length on a track row, Metro/Count In/Sync on the
    // BPM row) rather than getting a separate mention, since it's purely a
    // modifier here, never a standalone action of its own -- same
    // reasoning applies wherever else ALT shows up in this app. The BPM
    // value and Time Sig don't share ALT's hint -- they're EDIT-held
    // instead (handleBpmValueAdjust()/handleTimeSigAdjust()), woven into
    // EDIT's own hint instead ("rec/bpm": record on a track row, adjust
    // BPM/Time Sig when held on the BPM row). PLAY/RIGHT's track-row
    // meanings (Pause, Mute) are each a single word here since both are
    // toggles -- pressing the same button again does the opposite, not a
    // separate action.
    // "L"/"R" (rather than spelled-out LEFT/RIGHT) is this line's own
    // concession to fitting five distinct hints on one row -- the same
    // idea as "UD" above, just naming two buttons that do different
    // things instead of one pair doing the same thing.
    tft.drawString("UD row/ALT chan  PLAY pause  EDIT rec/bpm", 4, fy + 1);
    tft.drawString("L back/R mute/ALT bar  ENT menu  NAV stop", 4, fy + 17);
    drawBatteryMeter();
}

void updateLooperPositions(const LoopTrackView tracksArg[4], int selectedTrack) {
    for (int i = 0; i < 4; i++) {
        drawLooperTrackBar(tracksArg[i], i);
        drawLooperTrackActivity(tracksArg[i], i, i == selectedTrack);
    }
}

// Cheap partial redraw of just the selected track's channel field (see
// drawLooperTrackChannel()), e.g. after ALT+UP/DOWN changes it. Channel
// changes only ever apply to the currently selected track, so this is
// always the highlighted row.
void updateLooperChannel(const LoopTrackView tracksArg[4], int selectedTrack) {
    drawLooperTrackChannel(tracksArg[selectedTrack], selectedTrack, true);
}

void updateLooperBarLength(const LoopTrackView tracksArg[4], int selectedTrack) {
    drawLooperTrackBarLength(tracksArg[selectedTrack], selectedTrack, true);
}

// Cheap partial redraw of just track trackIdx's length field -- e.g. after
// a BPM change rescales it. Unlike the other per-field update functions,
// this can be called for any track, not just the selected one (a BPM
// change can rescale several bar-quantized tracks at once), hence the
// explicit `selected` rather than always assuming true.
void updateLooperLength(const LoopTrackView tracksArg[4], int trackIdx, bool selected) {
    drawLooperTrackLength(tracksArg[trackIdx], trackIdx, selected);
}

void updateLooperBpm(float bpm, int timeSigNum, int timeSigDen, bool syncMode, bool metronomeOn,
                      int metronomeVolumePercent, bool showMetronomeVolume, bool countInEnabled,
                      int countInBars, bool showCountInBars, int focusIndex, bool selected) {
    drawLooperBpmRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                      showMetronomeVolume, countInEnabled, countInBars, showCountInBars, focusIndex, selected);
}

// -- count-in overlay -----------------------------------------------------

// Redraws just the big countdown number + caption, both cleared and
// redrawn together every call -- cheap enough (one big glyph, one short
// string) at the once-per-beat rate this is actually called, so there's
// no need to diff which part changed the way updateStatValue() does for
// the constantly-polled player screen.
void drawLooperCountInBeat(int beatsRemaining) {
    int cx = tft.width() / 2;
    int cy = tft.height() / 2;

    tft.fillRect(cx - 60, cy - 30, 120, 76, COLOR_BG);

    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%d", beatsRemaining);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COLOR_ACCENT, COLOR_BG);
    tft.setTextFont(4);
    tft.drawString(numBuf, cx, cy);

    tft.setTextFont(2);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("beats remaining", cx, cy + 38);

    tft.setTextDatum(TL_DATUM);
}

void drawLooperCountIn(int beatsRemaining) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("MIDI Looper", 4, 3);
    drawHeaderBrand();

    int cx = tft.width() / 2;
    int cy = tft.height() / 2;
    int fy = tft.height() - FOOTER_HEIGHT;

    // Everything else on this screen is either the header, the footer, or
    // drawLooperCountInBeat()'s own small centered box (cy-30 to cy+46,
    // matching its own fillRect below) -- clear the otherwise-empty space
    // above and below that box explicitly rather than relying on a
    // blanket fillScreen() (removed: it caused a visible full-screen
    // black flash on every redraw, same fix as
    // drawBrowser()'s/drawLooper()'s).
    int boxTop = cy - 30;
    int boxBottom = cy + 46;
    if (boxTop > HEADER_HEIGHT) {
        tft.fillRect(0, HEADER_HEIGHT, tft.width(), boxTop - HEADER_HEIGHT, COLOR_BG);
    }
    if (fy > boxBottom) {
        tft.fillRect(0, boxBottom, tft.width(), fy - boxBottom, COLOR_BG);
    }

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextFont(2);
    tft.drawString("Count In", cx, cy - 45);
    tft.setTextDatum(TL_DATUM);

    drawLooperCountInBeat(beatsRemaining);

    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("Recording starts after the count-in", 4, fy + 1);
    drawBatteryMeter();
}

void updateLooperCountInBeat(int beatsRemaining) {
    drawLooperCountInBeat(beatsRemaining);
}

void updateLooperBeatIndicator(bool visible, int currentBeat, int timeSigNum) {
    drawLooperBeatIndicator(visible, currentBeat, timeSigNum);
}

// -- Settings screen --------------------------------------------------------

const int SETTINGS_ROW_Y0 = HEADER_HEIGHT + 4;
const int SETTINGS_VALUE_X = 180; // left column stays well clear of the longest label ("Count In Bars")

// Draws (or erases) a single settings row at `index`, given the list is
// currently scrolled to `scrollOffset` -- same idea as drawBrowserRow(),
// see its comment. Used both by the full redraw and by the
// selection/value-only partial redraws.
void drawSettingsRow(const char* label, const char* value, int index, int scrollOffset, bool selected) {
    int row = index - scrollOffset;
    if (row < 0 || row >= visibleRows()) return;
    int y = SETTINGS_ROW_Y0 + row * ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    tft.fillRect(0, y, tft.width(), ROW_HEIGHT, bg);
    tft.setTextColor(COLOR_TEXT, bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(label, 8, y + 1);
    tft.drawString(value, SETTINGS_VALUE_X, y + 1);
}

void drawSettings(const char* const* labels, const char* const* values, int count, int cursor,
                   int scrollOffset, const char* pageTitle) {
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, COLOR_HILITE_BG);
    tft.setTextColor(COLOR_TEXT, COLOR_HILITE_BG);
    tft.setTextDatum(TL_DATUM);
    char titleBuf[32];
    snprintf(titleBuf, sizeof(titleBuf), "Settings: %s", pageTitle);
    tft.drawString(titleBuf, 4, 3);
    drawHeaderBrand();

    // The header-to-first-row gap isn't covered by any row's own draw --
    // cleared explicitly rather than relying on a blanket fillScreen()
    // (removed: it caused a visible full-screen black flash on every
    // redraw, same fix as drawBrowser()'s/drawLooper()'s).
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), SETTINGS_ROW_Y0 - HEADER_HEIGHT, COLOR_BG);

    int rows = visibleRows();
    // Every row slot gets painted, not just the ones with an item -- past
    // `count`, blank it -- same "avoids a black flash, handles a shorter
    // list than before" reasoning as drawBrowser()'s row loop.
    for (int row = 0; row < rows; row++) {
        int idx = scrollOffset + row;
        if (idx < count) {
            drawSettingsRow(labels[idx], values[idx], idx, scrollOffset, idx == cursor);
        } else {
            tft.fillRect(0, SETTINGS_ROW_Y0 + row * ROW_HEIGHT, tft.width(), ROW_HEIGHT, COLOR_BG);
        }
    }

    // Row slots only cover a whole multiple of ROW_HEIGHT, which can leave
    // a sliver of the body area between the last row and the footer.
    int bodyBottom = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = SETTINGS_ROW_Y0 + rows * ROW_HEIGHT;
    if (rowsBottom < bodyBottom) {
        tft.fillRect(0, rowsBottom, tft.width(), bodyBottom - rowsBottom, COLOR_BG);
    }

    if (count > rows) drawScrollbar(count, scrollOffset, rows, SETTINGS_ROW_Y0);

    int fy = tft.height() - FOOTER_HEIGHT;
    tft.fillRect(0, fy, tft.width(), FOOTER_HEIGHT, COLOR_BG);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    // Row-grouped (see pins.h): PLAY (row 1) does nothing on this screen,
    // so line 1 covers UP/DOWN plus plain LEFT/RIGHT (page navigation);
    // line 2 covers EDIT+LEFT/RIGHT (value adjust, EDIT woven in since
    // it's a pure modifier here) plus NAV (unconditional exit).
    tft.drawString("UD move  LR page", 4, fy + 1);
    tft.drawString("EDIT+LR change  NAV back", 4, fy + 17);
    drawBatteryMeter();
}

void updateSettingsSelection(const char* const* labels, const char* const* values, int count,
                              int prevCursor, int newCursor, int scrollOffset) {
    if (prevCursor >= 0 && prevCursor < count) drawSettingsRow(labels[prevCursor], values[prevCursor], prevCursor, scrollOffset, false);
    if (newCursor >= 0 && newCursor < count) drawSettingsRow(labels[newCursor], values[newCursor], newCursor, scrollOffset, true);

    // drawSettingsRow() fills the full row width, including the
    // scrollbar's 3px column -- re-layer it on top, same reasoning as
    // updateBrowserSelection().
    int rows = visibleRows();
    if (count > rows) drawScrollbar(count, scrollOffset, rows, SETTINGS_ROW_Y0);
}

void updateSettingsValue(const char* label, const char* value, int index, int scrollOffset) {
    drawSettingsRow(label, value, index, scrollOffset, true);
}

// Cheap partial redraw for moving the looper's row cursor: repaints only
// the previously and newly selected rows instead of the whole screen.
// Covers all 5 rows (4 tracks + BPM), since the cursor can land on any of
// them -- prevOnBpm/newOnBpm say whether that endpoint is the BPM row
// rather than prevSelected/newSelected being a track index. Track rows
// redraw label *and* bar together -- the label's background fill spans
// the bar's y-range too, so drawing the label alone would wipe the bar
// without redrawing it.
// Cheap partial redraw of trackIdx's whole row (label text + bar +
// activity dots) -- e.g. after its state changes from a button press,
// without touching any other row, the BPM row, header, or footer.
void updateLooperTrackRow(const LoopTrackView tracksArg[4], int trackIdx, bool selected) {
    drawLooperTrackLabel(tracksArg[trackIdx], trackIdx, selected);
    drawLooperTrackBar(tracksArg[trackIdx], trackIdx);
    drawLooperTrackActivity(tracksArg[trackIdx], trackIdx, selected);
}

void updateLooperSelection(const LoopTrackView tracksArg[4], int prevSelected, bool prevOnBpm,
                            int newSelected, bool newOnBpm, bool syncMode, float bpm,
                            int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                            bool showMetronomeVolume, bool countInEnabled, int countInBars,
                            bool showCountInBars, int focusIndex) {
    if (prevOnBpm) {
        drawLooperBpmRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                          showMetronomeVolume, countInEnabled, countInBars, showCountInBars, focusIndex, false);
    } else if (prevSelected >= 0 && prevSelected < 4) {
        updateLooperTrackRow(tracksArg, prevSelected, false);
    }
    if (newOnBpm) {
        drawLooperBpmRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                          showMetronomeVolume, countInEnabled, countInBars, showCountInBars, focusIndex, true);
    } else if (newSelected >= 0 && newSelected < 4) {
        updateLooperTrackRow(tracksArg, newSelected, true);
    }
}

void drawMessage(const char* line1, const char* line2) {
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.setTextDatum(MC_DATUM);
    int cx = tft.width() / 2;
    int cy = tft.height() / 2;

    // No header/footer bars to anchor on here (this is a bare centered
    // dialog, used for many transient confirm screens) -- split the
    // clear into three bands around the text instead of one blanket
    // fillScreen() (removed: it caused a visible full-screen black flash
    // on every redraw, same fix as drawBrowser()'s/drawLooper()'s), so no
    // single clear covers more than roughly half the screen.
    int textTop = cy - 20;
    int textBottom = cy + 20;
    if (textTop > 0) tft.fillRect(0, 0, tft.width(), textTop, COLOR_BG);
    tft.fillRect(0, textTop, tft.width(), textBottom - textTop, COLOR_BG);
    if (textBottom < tft.height()) {
        tft.fillRect(0, textBottom, tft.width(), tft.height() - textBottom, COLOR_BG);
    }

    tft.drawString(line1, cx, line2 ? cy - 10 : cy);
    if (line2) tft.drawString(line2, cx, cy + 10);
    tft.setTextDatum(TL_DATUM);
    drawBatteryMeter();
}

// Small vertical battery gauge in the bottom-right corner. Footer
// button-hint text is left-aligned starting at x=4 and, even at its
// widest ("LEFT back/ENT+LR seek  ALT midi  NAV stop" in drawWavPlayer()),
// stays well clear of this corner, so it's free across every screen.
// Called on main.cpp's own ~1s timer (not tied to Battery::update()'s
// much slower ~15s actual sample rate) so the icon reappears quickly
// after any screen transition wipes the footer, without paying that
// cost anywhere near once per loop() iteration -- see main.cpp.
// Redrawn from scratch every call (clear + outline + fill) rather than
// diffed against the previous state: at a ~1s cadence and this size,
// there's no meaningful cost to just always repainting it outright.
void drawBatteryMeter() {
    const int bodyW = 10, bodyH = 16;
    const int termW = 4, termH = 2;
    const int marginRight = 6;

    int bodyX = tft.width() - marginRight - bodyW;
    int termX = bodyX + (bodyW - termW) / 2;
    int fy = tft.height() - FOOTER_HEIGHT;
    int topY = fy + (FOOTER_HEIGHT - (bodyH + termH)) / 2;
    int termY = topY;
    int bodyY = topY + termH;

    uint8_t pct = Battery::percentage();
    bool charging = Battery::isCharging();

    tft.fillRect(bodyX - 1, topY - 1, bodyW + 2, bodyH + termH + 2, COLOR_BG);

    uint16_t fillColor = COLOR_TEXT;
    if (charging) {
        fillColor = COLOR_ACCENT;
    } else if (pct <= 15) {
        fillColor = COLOR_ERROR;
    } else if (pct <= 30) {
        fillColor = COLOR_DIM;
    }

    tft.drawRect(bodyX, bodyY, bodyW, bodyH, COLOR_DIM);
    tft.fillRect(termX, termY, termW, termH, COLOR_DIM);

    // Fill grows from the bottom, matching a physical battery's usual
    // orientation with the terminal on top.
    const int interiorH = bodyH - 2;
    int filledH = (interiorH * pct) / 100;
    if (pct > 0 && filledH == 0) filledH = 1; // always show *something* above 0%
    if (filledH > 0) {
        tft.fillRect(bodyX + 1, bodyY + 1 + (interiorH - filledH), bodyW - 2, filledH, fillColor);
    }
}

} // namespace Ui

#include "ui.h"
#include "battery.h"
#include "pins.h"
#include "keyboard_layout.h"
#include "version.h"
#include "synth.h" // pariSynth play screen -- see drawPariSynthPlay()
#include "ui_strip.h"
#include "ui_look.h"
#include "ui_player.h"
#include "ui_modes.h"
#include <ctype.h>
#include <TFT_eSPI.h>
#include <hardware/clocks.h>
#include <string.h>
#include <stdio.h>

namespace {

using Ui::stripBegin;
using Ui::stripMaxRows;
using Ui::stripSend;
using Ui::sliceAllows;
using Ui::SliceTimer;
using Ui::DRAW_METER;

TFT_eSPI tft = TFT_eSPI();

// ---- theme palette --------------------------------------------------
// The colours live in ui_look.cpp. Each one is a theme entry the Theme page
// edits as R/G/B and .thm files save and load by label, so labels must
// never be renamed.
using Look::COLOR_BG;
using Look::COLOR_SURFACE;
using Look::COLOR_RULE;
using Look::COLOR_TEXT;
using Look::COLOR_DIM;
using Look::COLOR_HILITE_BG;
using Look::COLOR_SELECT;
using Look::COLOR_ACCENT;
using Look::COLOR_ERROR;
using Look::COLOR_MIDI;
using Look::COLOR_WAV;
using Look::COLOR_TRACK_ARMED;
using Look::COLOR_TRACK_RECORDING;
using Look::COLOR_TRACK_PLAYING;
using Look::COLOR_TRACK_STOPPED;
using Look::COLOR_TRACK_PAUSED;
using Look::COLOR_PULSE;
using Look::COLOR_DOWNBEAT;
using Look::COLOR_NOTE_HIGH;
using Look::COLOR_NOTE_MID;
using Look::COLOR_NOTE_LOW;
using Look::COLOR_PERC_HIGH;
using Look::COLOR_PERC_MID;
using Look::COLOR_PERC_LOW;
using Look::rgb;
using Look::Icon;

// `label` is the .thm key, `def` what resetThemeColorsToDefault() restores.
struct ThemeColorEntry {
    const char* label;
    uint16_t* value;
    uint16_t def;
};
ThemeColorEntry THEME_COLORS[] = {
    {"Background", &COLOR_BG,        rgb(0x0A0E14)},
    {"Surface",    &COLOR_SURFACE,   rgb(0x141A22)},
    {"Rule",       &COLOR_RULE,      rgb(0x2A3340)},
    {"Text",       &COLOR_TEXT,      rgb(0xE6E9EE)},
    {"Dim",        &COLOR_DIM,       rgb(0x8A95A5)},
    {"Highlight",  &COLOR_HILITE_BG, rgb(0x122028)},
    {"Select",     &COLOR_SELECT,    rgb(0x2FD4E6)},
    {"Accent",     &COLOR_ACCENT,    rgb(0xFFB000)},
    {"Error",      &COLOR_ERROR,     rgb(0xFF3B30)},
    {"MIDI",       &COLOR_MIDI,      rgb(0xB08CFF)},
    {"WAV",        &COLOR_WAV,       rgb(0x7FD67F)},
    {"Armed",      &COLOR_TRACK_ARMED,     rgb(0xFF8A82)},
    {"Recording",  &COLOR_TRACK_RECORDING, rgb(0xFF3B30)},
    {"Playing",    &COLOR_TRACK_PLAYING,   rgb(0xFFB000)},
    {"Stopped",    &COLOR_TRACK_STOPPED,   rgb(0x8A95A5)},
    {"Paused",     &COLOR_TRACK_PAUSED,    rgb(0x2FD4E6)},
    {"Pulse",      &COLOR_PULSE,    rgb(0x2FD4E6)},
    {"Downbeat",   &COLOR_DOWNBEAT, rgb(0xFFB000)},
    {"Note Hi",    &COLOR_NOTE_HIGH, rgb(0xFFB000)},
    {"Note Mid",   &COLOR_NOTE_MID,  rgb(0x2FD4E6)},
    {"Note Lo",    &COLOR_NOTE_LOW,  rgb(0x1F6E78)},
    {"Perc Hi",    &COLOR_PERC_HIGH, rgb(0xFF3B30)},
    {"Perc Mid",   &COLOR_PERC_MID,  rgb(0xB08CFF)},
    {"Perc Lo",    &COLOR_PERC_LOW,  rgb(0x5A4A80)},
};
const int THEME_COLOR_COUNT = sizeof(THEME_COLORS) / sizeof(THEME_COLORS[0]);

// RGB565 (TFT_eSPI's native 16-bit color format: 5 bits red, 6 green, 5
// blue) packing/unpacking -- the Theme page edits each channel as a plain
// 0-255 byte (familiar, and matches what a .thm file stores), quantized
// down to whatever the panel can actually display. unpackRGB565()
// replicates each channel's top bits into its own low bits (rather than
// just left-shifting and leaving the low bits zero) so e.g. full-scale
// blue (0-31) unpacks to 255, not 248 -- round-tripping a color through
// edit-and-store-back doesn't quietly darken it.
uint16_t packRGB565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
void unpackRGB565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    r = (uint8_t)((r5 << 3) | (r5 >> 2));
    g = (uint8_t)((g6 << 2) | (g6 >> 4));
    b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

const int ROW_HEIGHT = 18;
// The status strip and its rule, and the two-row keypad footer.
const int HEADER_HEIGHT = Look::HEADER_H;
const int FOOTER_HEIGHT = Look::FOOTER_H;


// Recording screen layout, same fixed-offset approach as the player
// screen above so updateRecordingLive() can repaint just the elapsed/
// event-count rows.
const int REC_FILENAME_Y = HEADER_HEIGHT + 10;
const int REC_STATE_Y    = REC_FILENAME_Y + ROW_HEIGHT + 6;
const int REC_ELAPSED_Y  = REC_STATE_Y + ROW_HEIGHT;
const int REC_EVENTS_Y   = REC_ELAPSED_Y + ROW_HEIGHT;

// Scrolling MIDI-in log (see updateRecordingLog()), filling the otherwise-
// empty stretch between the Events row and the footer. Row count isn't a
// fixed constant -- it's however many ROW_HEIGHT rows actually fit between
// REC_LOG_Y and the footer, computed at draw time the same way drawPlayer()
// et al. compute fy from tft.height() rather than assuming a screen size.
// Same ROW_HEIGHT/font as the rest of this screen rather than a smaller
// font to pack in more lines -- shrinking the font to fit more on screen
// was tried for a different layout and rejected on real hardware as
// illegible (see embedded-ui-change-approach memory).
const int REC_LOG_Y = REC_EVENTS_Y + ROW_HEIGHT + 8;

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

// Draws (or blanks) the browser row for entry idx at scroll offset.
void drawBrowserRow(const SdBrowser& browser, int idx, int scrollOffset, bool selected) {
    int row = idx - scrollOffset;
    if (row < 0 || row >= rowsOnScreen()) return;
    int y = Look::LIST_TOP + row * ROW_HEIGHT;
    if (idx < 0 || idx >= browser.entryCount()) {
        Look::drawFileRow(y, nullptr, false);
        return;
    }
    const BrowserEntry& e = browser.entry(idx);
    Look::FileRow r;
    r.name = e.name;
    r.size = e.size;
    r.typeLabel = nullptr;
    if (e.isDir) {
        r.type = Look::ROW_DIR;
    } else {
        switch (e.kind) {
            case FILE_MID: r.type = Look::ROW_MIDI;    r.typeLabel = "MID"; break;
            case FILE_SYX: r.type = Look::ROW_SYX;     r.typeLabel = "SYX"; break;
            case FILE_WAV: r.type = Look::ROW_WAV;     r.typeLabel = "WAV"; break;
            case FILE_MOD: r.type = Look::ROW_TRACKER; r.typeLabel = "MOD"; break;
            case FILE_S3M: r.type = Look::ROW_TRACKER; r.typeLabel = "S3M"; break;
            case FILE_XM:  r.type = Look::ROW_TRACKER; r.typeLabel = "XM"; break;
            default:       r.type = Look::ROW_TRACKER; r.typeLabel = "IT"; break;
        }
    }
    Look::drawFileRow(y, &r, selected);
}

// Scrollbar beside a row list whose first row is at trackY. Blanks the
// track when everything fits.
void drawScrollbar(int count, int scrollOffset, int rows, int trackY) {
    Look::drawScrollbar(trackY, rows, count, scrollOffset);
}

// Clears a w x h box at x, y to bg and draws `text` in font 2 at tx along
// from x, top aligned, through the strip canvas. A box taller than one strip
// at that width goes out in bands, the text clipped to each.
void stripText(int x, int y, int w, int h, const char* text, int tx, uint16_t fg, uint16_t bg) {
    if (x + w > tft.width()) w = tft.width() - x;
    if (w <= 0 || h <= 0) return;
    int rows = stripMaxRows(w);
    for (int by = 0; by < h; by += rows) {
        int bh = (h - by < rows) ? h - by : rows;
        TFT_eSprite& s = stripBegin(x, y + by, w, bh, bg);
        if (text && text[0]) {
            s.setTextColor(fg, bg);
            s.drawString(text, tx, -by);
        }
        stripSend();
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
        case MIDI_OUT_OFF:      return "Off";
        case MIDI_OUT_HARDWARE: return "HW MIDI";
        case MIDI_OUT_USB:      return "USB MIDI";
        default:                return "HW+USB";
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

// Writes note number 60 as "C4" (scientific pitch notation, middle C = C4)
// into a caller-supplied 5-char buffer (worst case "C#-1" + NUL, for the
// sharps down at note 1/3/6/8/10) -- just for the MIDI-in log below, so no
// claim is made that this matches any particular DAW/tracker's own octave
// numbering.
void midiNoteName(uint8_t note, char* out) {
    static const char* const NAMES[12] = { "C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B" };
    int octave = note / 12 - 1;
    snprintf(out, 5, "%s%d", NAMES[note % 12], octave);
}

// Formats one MidiLogEntry (see updateRecordingLog()) into a single short
// line, e.g. "Ch1  Note On   C4  v100" or "Ch3  CC 74  = 90". `out` should
// be sized generously (see the callers) -- proportional-font width, not
// character count, is what actually has to fit the row.
void formatMidiLogLine(const Ui::MidiLogEntry& e, char* out, size_t outSize) {
    if (e.isSysEx) {
        snprintf(out, outSize, "SysEx  %u bytes", (unsigned)e.sysExLen);
        return;
    }
    uint8_t type = e.status & 0xF0;
    int channel = (e.status & 0x0F) + 1;
    char note[5];
    switch (type) {
        case 0x80:
            midiNoteName(e.data1, note);
            snprintf(out, outSize, "Ch%-2d Note Off  %-3s v%d", channel, note, e.data2);
            break;
        case 0x90:
            midiNoteName(e.data1, note);
            snprintf(out, outSize, "Ch%-2d Note On   %-3s v%d", channel, note, e.data2);
            break;
        case 0xA0:
            midiNoteName(e.data1, note);
            snprintf(out, outSize, "Ch%-2d Poly AT   %-3s %d", channel, note, e.data2);
            break;
        case 0xB0:
            snprintf(out, outSize, "Ch%-2d CC %-3d = %d", channel, e.data1, e.data2);
            break;
        case 0xC0:
            snprintf(out, outSize, "Ch%-2d Prog Chg  %d", channel, e.data1);
            break;
        case 0xD0:
            snprintf(out, outSize, "Ch%-2d Chan AT   %d", channel, e.data1);
            break;
        case 0xE0: {
            int bend = (int)(((uint16_t)e.data2 << 7) | e.data1) - 8192;
            snprintf(out, outSize, "Ch%-2d Bend      %+d", channel, bend);
            break;
        }
        default:
            snprintf(out, outSize, "Ch%-2d %02X %02X %02X", channel, e.status, e.data1, e.data2);
            break;
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
    tft.fillCircle(x, y, 5, active ? COLOR_PULSE : COLOR_DIM);
}

// Paints a progress bar's change from `lastFill` to `fillW` pixels (the bar
// is `barW` wide at x, y, `h` high, COLOR_DIM behind the fill). lastFill < 0
// paints the whole bar; colorChanged repaints the whole fill.
void paintBarDelta(int x, int y, int h, int barW, int fillW, int lastFill, uint16_t color, bool colorChanged) {
    if (lastFill < 0) {
        tft.fillRect(x, y, barW, h, COLOR_DIM);
        if (fillW > 0) tft.fillRect(x, y, fillW, h, color);
        return;
    }
    if (fillW < lastFill) tft.fillRect(x + fillW, y, lastFill - fillW, h, COLOR_DIM);
    if (colorChanged) {
        if (fillW > 0) tft.fillRect(x, y, fillW, h, color);
    } else if (fillW > lastFill) {
        tft.fillRect(x + lastFill, y, fillW - lastFill, h, color);
    }
}

// Byte-progress bar + numeric readout, filling the space drawPlayer()'s
// note strip would otherwise occupy (meaningless for a raw SysEx dump --
// there are no notes). The filled portion briefly switches from steady
// green to bright yellow immediately after a message is actually sent
// (see RECORDING_ACTIVITY_MS), then settles back -- a static bar alone reads
// as "how far along", the flash is what reads as "actively sending right
// now" per this screen's whole point.
char g_sysexLabel[32] = {0};
int g_sysexFill = -1;
uint16_t g_sysexColor = 0;

// `full` (the screen's entry draw) clears the whole progress area and
// repaints it; live ticks only repaint the label if its text changed and
// the part of the bar that changed.
void drawSysExProgress(const SysExPlayer& player, bool full) {
    uint32_t total = player.totalBytes();
    uint32_t sent = player.bytesSent();
    int pct = total > 0 ? (int)(((uint64_t)sent * 100) / total) : 0;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu / %lu bytes (%d%%)", (unsigned long)sent, (unsigned long)total, pct);
    if (full) {
        g_sysexLabel[0] = '\0';
        g_sysexFill = -1;
        // The bar is inset 8px each side; clear its margins once.
        tft.fillRect(0, SYSEX_PROGRESS_BAR_Y, tft.width(), SYSEX_PROGRESS_BAR_H, COLOR_BG);
        // Nothing else occupies the space below the bar down to the footer.
        int fy = tft.height() - FOOTER_HEIGHT;
        int contentBottom = SYSEX_PROGRESS_BAR_Y + SYSEX_PROGRESS_BAR_H;
        if (fy > contentBottom) {
            tft.fillRect(0, contentBottom, tft.width(), fy - contentBottom, COLOR_BG);
        }
    }
    if (strcmp(buf, g_sysexLabel) != 0) {
        stripText(0, SYSEX_PROGRESS_LABEL_Y, tft.width(), ROW_HEIGHT, buf, 8, COLOR_DIM, COLOR_BG);
        strcpy(g_sysexLabel, buf);
    }

    int barX = 8, barW = tft.width() - 16;
    int fillW = total > 0 ? (int)(((uint64_t)barW * sent) / total) : 0;
    uint16_t color = (fillW > 0 && player.msSinceLastSend() < RECORDING_ACTIVITY_MS) ? COLOR_PULSE : COLOR_ACCENT;
    if (fillW == g_sysexFill && (fillW == 0 || color == g_sysexColor)) return;
    paintBarDelta(barX, SYSEX_PROGRESS_BAR_Y, SYSEX_PROGRESS_BAR_H, barW, fillW, g_sysexFill,
                  color, color != g_sysexColor);
    g_sysexFill = fillW;
    g_sysexColor = color;
}

// Shared keypad footer from up to two hint lines, see Look::drawHintFooter().
void drawFooter(const char* line1, const char* line2 = nullptr) {
    Look::drawHintFooter(line1, line2);
}

// The status strip for a screen: title in Silkscreen, an optional icon, and
// optional VT323 text after it, dim after a title and bright without one.
void statusBar(const char* title, const Icon* icon, uint16_t iconColor, const char* text = nullptr) {
    Look::Status st;
    st.icon = icon;
    st.iconColor = iconColor;
    st.title = title;
    char buf[64];
    if (text) {
        Look::fitText(Look::FONT_TEXT, text, Look::statusTextRoom(st), buf, sizeof(buf));
        if (title) st.text = buf;
        else st.textLast = buf;
    }
    Look::drawStatus(st);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
}

} // namespace

namespace Ui {

// 0-100 percent -> a PWM duty cycle on DISPLAY_BACKLIGHT. analogWrite()
// (Arduino-Pico core, RP2040 PWM hardware) rather than the plain
// digitalWrite(HIGH) this used to be -- there's no other PWM output
// anywhere else in this firmware to match a convention against, so this
// just uses the framework's own default PWM frequency/resolution (8-bit,
// 0-255), which is already how the framework's other analogWrite() users
// (e.g. LED dimming) typically leave it.
void setBacklightBrightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    analogWrite(DISPLAY_BACKLIGHT, (percent * 255) / 100);
}

void begin() {
    pinMode(DISPLAY_BACKLIGHT, OUTPUT);
    setBacklightBrightness(0); // stay dark until the screen is cleared, so GRAM garbage never reaches the eye

    // SPI clock: the SPI block divides clk_peri by an even prescale (2 or
    // more) times a whole number. The core's set_sys_clock_khz(200 MHz)
    // leaves clk_peri on the 48 MHz USB PLL, so without DISPLAY_FAST_PERI
    // any SPI_FREQUENCY over 24 MHz gives 24 MHz (48 / 2). With it,
    // clk_peri runs from clk_sys (200 MHz), switched here before anything
    // opens SPI or a UART, and the 50 MHz request gives exactly 200 / 4 =
    // 50 MHz, the fastest clean step under the ST7789's 62.5 MHz write
    // limit (16 ns cycle). The next steps down are 33.3 and 25 MHz.
    // Serial1 (MIDI) starts after this and works out its baud from
    // clk_peri, which divides exactly either way: 400 at 200 MHz, 96 at 48.
#if defined(DISPLAY_FAST_PERI) && DISPLAY_FAST_PERI
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                              clock_get_hz(clk_sys));
#endif

    tft.init();
    stripInit(tft);
    Look::setScreen(&tft);
    tft.setRotation(3);
    tft.fillScreen(COLOR_BG);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);

    setBacklightBrightness(100); // full brightness until SettingsMode::begin() applies the saved value
}

int visibleRows() { return rowsOnScreen(); }

void drawSplash() {
    Look::drawSplash(VERSIONNUMBER);
}

const char* shortTargetLabel(MidiOutTarget t) {
    switch (t) {
        case MIDI_OUT_HARDWARE: return "HW";
        case MIDI_OUT_USB:      return "USB";
        case MIDI_OUT_BOTH:     return "HW+USB";
        default:                return "OFF";
    }
}

// Status strip shared by drawBrowser() and drawLoopBrowser(): the folder
// as "/ music / mods" with the last part bright, plus the truncation note
// (see SdBrowser::truncationNote()) when the last scan didn't list the
// whole folder. A long path keeps its end. The file browser shows the MIDI
// output target on the right, which ALT changes there.
void drawBrowserHeaderPath(const SdBrowser& browser, const char* title, bool showTarget) {
    char pathBuf[192];
    browser.displayPath(pathBuf, sizeof(pathBuf));
    char note[16];
    browser.truncationNote(note, sizeof(note));

    // "/a/b" to "/ a / " and "b".
    char spaced[256];
    size_t n = 0;
    const char* last = pathBuf;
    for (const char* p = pathBuf; *p; p++)
        if (*p == '/') last = p + 1;
    for (const char* p = pathBuf; p < last && n + 4 < sizeof(spaced); p++) {
        if (*p == '/') {
            if (n > 0) spaced[n++] = ' ';
            spaced[n++] = '/';
            spaced[n++] = ' ';
        } else {
            spaced[n++] = *p;
        }
    }
    spaced[n] = '\0';
    char lastBuf[MAX_FILENAME_LEN + 20];
    snprintf(lastBuf, sizeof(lastBuf), "%s%s", last, note);

    Look::Status st;
    st.icon = &Look::ICON_FOLDER;
    st.iconColor = COLOR_ACCENT;
    st.title = title;
    if (showTarget) {
        st.rightLabel = "OUT";
        st.rightValue = shortTargetLabel(MidiOutput::getTarget());
    }
    int room = Look::statusTextRoom(st);
    char headBuf[256], lastFit[MAX_FILENAME_LEN + 24];
    int lastW = Look::textWidth(Look::FONT_TEXT, lastBuf);
    if (lastW > room) {
        Look::fitText(Look::FONT_TEXT, lastBuf, room, lastFit, sizeof(lastFit));
        headBuf[0] = '\0';
    } else {
        snprintf(lastFit, sizeof(lastFit), "%s", lastBuf);
        Look::fitText(Look::FONT_TEXT, spaced, room - lastW, headBuf, sizeof(headBuf), true);
    }
    st.text = headBuf;
    st.textLast = lastFit;
    Look::drawStatus(st);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
}

// Rows, the gap under them and the scrollbar, shared by both browsers.
void drawBrowserList(const SdBrowser& browser, int selectedIndex, int scrollOffset, const char* emptyText) {
    int rows = rowsOnScreen();
    int count = browser.entryCount();
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), Look::LIST_TOP - HEADER_HEIGHT, COLOR_BG);
    // Every slot is painted, blank ones too, so no full-screen clear.
    for (int row = 0; row < rows; row++) {
        int idx = scrollOffset + row;
        drawBrowserRow(browser, idx, scrollOffset, idx == selectedIndex);
    }
    int bodyBottom = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = Look::LIST_TOP + rows * ROW_HEIGHT;
    if (rowsBottom < bodyBottom) tft.fillRect(0, rowsBottom, tft.width(), bodyBottom - rowsBottom, COLOR_BG);
    drawScrollbar(count, scrollOffset, rows, Look::LIST_TOP);
    if (count == 0) {
        Look::band(0, Look::LIST_TOP, Look::LIST_W, ROW_HEIGHT, COLOR_BG, [&](TFT_eSPI& g) {
            Look::drawText(g, Look::FONT_TEXT, emptyText, 10, Look::rowTextY(Look::LIST_TOP), COLOR_DIM);
        });
    }
}

void updateBrowserHeader(const SdBrowser& browser) {
    drawBrowserHeaderPath(browser, "FILES", true);
}

void drawBrowser(const SdBrowser& browser, int selectedIndex, int scrollOffset) {
    drawBrowserHeaderPath(browser, "FILES", true);
    drawBrowserList(browser, selectedIndex, scrollOffset, "no .mid .syx .wav .mod .s3m .xm .it files");
    // PLAY and RIGHT open too. ALT held with UP/DOWN pages.
    static const Look::Key row1[] = {
        {"UP", "up", Look::TONE_TEXT},
        {"PLAY", "play", Look::TONE_PLAY},
        {"EDIT", "menu", Look::TONE_TEXT},
        {"ALT", "out", Look::TONE_TEXT},
        {"NAV", "back", Look::TONE_TEXT},
    };
    static const Look::Key row2[] = {
        {"LEFT", "parent", Look::TONE_TEXT},
        {"DOWN", "down", Look::TONE_TEXT},
        {"RIGHT", "open", Look::TONE_TEXT},
        {"ENT", "open", Look::TONE_SELECT},
    };
    Look::drawKeyFooter(row1, 5, row2, 4);
}

void drawLoopBrowser(const SdBrowser& browser, int selectedIndex, int scrollOffset) {
    drawBrowserHeaderPath(browser, "LOOPS", false);
    drawBrowserList(browser, selectedIndex, scrollOffset, "no saved loops here");
    // Select, delete or back only.
    drawFooter("UD move  ENTER select", "HOLD ENTER delete");
}

void updateBrowserSelection(const SdBrowser& browser, int prevIndex, int newIndex, int scrollOffset) {
    // Rows stop short of the scrollbar, so it needs no redraw.
    drawBrowserRow(browser, prevIndex, scrollOffset, false);
    drawBrowserRow(browser, newIndex, scrollOffset, true);
}

// The player screens are in ui_player.cpp; these fill its info structs.

UiPlayer::PlayState playState(MidiPlayer::State s, bool stopped) {
    switch (s) {
        case MidiPlayer::STATE_PLAYING: return UiPlayer::PS_PLAYING;
        case MidiPlayer::STATE_PAUSED:  return stopped ? UiPlayer::PS_STOPPED : UiPlayer::PS_PAUSED;
        case MidiPlayer::STATE_DONE:    return UiPlayer::PS_DONE;
        case MidiPlayer::STATE_ERROR:   return UiPlayer::PS_ERROR;
        default:                        return UiPlayer::PS_IDLE;
    }
}

// Remembered for the live updates, which don't get them.
MidiOutTarget g_playerTarget = MIDI_OUT_BOTH;
bool g_playerAudio = false;
int g_playerVolume = 0;
bool g_playerStopped = false;

UiPlayer::MidiInfo midiInfo(const char* filename, const MidiPlayer& player) {
    UiPlayer::MidiInfo m;
    m.name = filename;
    m.format = player.format();
    m.tracks = player.trackCount();
    m.elapsedMs = player.elapsedMs();
    m.bpm = player.currentBPM();
    m.tempoPct = (int)(player.tempoScale() * 100.0f + 0.5f);
    m.state = playState(player.state(), g_playerStopped);
    m.error = player.errorMessage();
    m.volume = g_playerVolume;
    m.out = targetLabel(g_playerTarget);
    m.audioOn = g_playerAudio;
    return m;
}

void drawPlayer(const char* filename, const MidiPlayer& player, MidiOutTarget target,
                 bool audioOn, int volume, bool stopped) {
    g_playerTarget = target;
    g_playerAudio = audioOn;
    g_playerVolume = volume;
    g_playerStopped = stopped;
    UiPlayer::drawMidi(midiInfo(filename, player));
}

void updateVolume(int volume) {
    g_playerVolume = volume;
    UiPlayer::setVolume(volume);
}

void updatePlayerState(MidiPlayer::State state, bool stopped) {
    g_playerStopped = stopped;
    UiPlayer::midiState(playState(state, stopped));
}

void updatePlayerOutputTarget(MidiOutTarget target) {
    g_playerTarget = target;
    UiPlayer::midiOutput(targetLabel(target), g_playerAudio);
}

void updatePlayerAudioState(bool audioOn) {
    g_playerAudio = audioOn;
    UiPlayer::midiOutput(targetLabel(g_playerTarget), audioOn);
}

void updatePlayerLive(const MidiPlayer& player) {
    UiPlayer::midiLive(midiInfo(nullptr, player));
}

// -- pariSynth play screen (ui_modes.cpp) --------------------------------

static UiModes::SynthCell pariCell(int channel) {
    UiModes::SynthCell c;
    if (channel == 9) {
        c.name = "Drums";
        c.family = 16;
        return c;
    }
    uint8_t family = (Synth::getChannelProgram((uint8_t)channel) >> 3) & 0x0F;
    c.name = Synth::instrumentFamilyName(family);
    c.family = family;
    return c;
}

void drawPariSynthPlay(int selectedChannel, uint16_t activeMask) {
    UiModes::SynthCell cells[16];
    for (int ch = 0; ch < 16; ch++) cells[ch] = pariCell(ch);
    UiModes::synthDraw(cells, selectedChannel, activeMask);
}

uint16_t pariSynthDrawnActiveMask() { return UiModes::synthDrawnActiveMask(); }

void updatePariSynthPlayLive() { UiModes::synthLive(); }

void updatePariSynthChannelCell(int channel, bool selected, bool active) {
    UiModes::synthCell(channel, pariCell(channel), selected, active);
}

void updatePariSynthSelection(int prevChannel, int newChannel, bool prevActive, bool newActive) {
    updatePariSynthChannelCell(prevChannel, false, prevActive);
    updatePariSynthChannelCell(newChannel, true, newActive);
}

void updatePariSynthVolumeOverlay(int volume) { UiModes::synthVolume(volume); }

// -- WAV and tracker player screens (ui_player.cpp) ----------------------

UiPlayer::PlayState playState(WavPlayer::State s, bool stopped) {
    switch (s) {
        case WavPlayer::STATE_PLAYING: return UiPlayer::PS_PLAYING;
        case WavPlayer::STATE_PAUSED:  return stopped ? UiPlayer::PS_STOPPED : UiPlayer::PS_PAUSED;
        case WavPlayer::STATE_DONE:    return UiPlayer::PS_DONE;
        case WavPlayer::STATE_ERROR:   return UiPlayer::PS_ERROR;
        default:                       return UiPlayer::PS_IDLE;
    }
}

UiPlayer::PlayState playState(TrackerPlayer::State s, bool stopped) {
    switch (s) {
        case TrackerPlayer::STATE_PLAYING: return UiPlayer::PS_PLAYING;
        case TrackerPlayer::STATE_PAUSED:  return stopped ? UiPlayer::PS_STOPPED : UiPlayer::PS_PAUSED;
        case TrackerPlayer::STATE_DONE:    return UiPlayer::PS_DONE;
        case TrackerPlayer::STATE_ERROR:   return UiPlayer::PS_ERROR;
        default:                           return UiPlayer::PS_IDLE;
    }
}

UiPlayer::WavInfo wavInfo(const char* filename, const WavPlayer& player) {
    UiPlayer::WavInfo w;
    w.name = filename;
    w.fileKey = player.pathKey();
    w.elapsedMs = player.elapsedMs();
    w.totalMs = player.totalMs();
    w.sampleRate = player.sampleRate();
    w.bits = player.bitsPerSample();
    w.channels = player.channels();
    w.state = playState(player.state(), g_playerStopped);
    w.error = player.errorMessage();
    w.volume = g_playerVolume;
    return w;
}

void drawWavPlayer(const char* filename, const WavPlayer& player, int volume, bool stopped) {
    g_playerVolume = volume;
    g_playerStopped = stopped;
    UiPlayer::drawWav(wavInfo(filename, player));
}

void updateWavPlayerState(WavPlayer::State state, bool stopped) {
    g_playerStopped = stopped;
    UiPlayer::wavState(playState(state, stopped));
}

void updateWavPlayerLive(const WavPlayer& player) {
    UiPlayer::wavLive(wavInfo(nullptr, player));
}

// The format from the file name: "XM".
void trackerFormat(const char* filename, char* out, size_t outSize) {
    const char* dot = filename ? strrchr(filename, '.') : nullptr;
    size_t n = 0;
    if (dot) {
        for (const char* p = dot + 1; *p && n + 1 < outSize; p++) out[n++] = (char)toupper((unsigned char)*p);
    }
    out[n] = '\0';
}

UiPlayer::TrackerInfo trackerInfo(const char* filename, const TrackerPlayer& player, const char* format) {
    UiPlayer::TrackerInfo t;
    t.name = filename;
    t.format = format;
    t.channels = player.channelCount();
    t.instruments = player.instrumentCount();
    t.songLength = player.songLength();
    t.order = player.patternOrderPosition();
    t.row = player.currentRow();
    t.speed = player.speed();
    t.tempo = player.tempo();
    t.elapsedMs = player.elapsedMs();
    t.state = playState(player.state(), g_playerStopped);
    t.error = player.errorMessage();
    t.volume = g_playerVolume;
    return t;
}

void drawTrackerPlayer(const char* filename, const TrackerPlayer& player, int volume, bool stopped) {
    g_playerVolume = volume;
    g_playerStopped = stopped;
    char format[6];
    trackerFormat(filename, format, sizeof(format));
    UiPlayer::drawTracker(trackerInfo(filename, player, format));
}

void updateTrackerPlayerState(TrackerPlayer::State state, bool stopped) {
    g_playerStopped = stopped;
    UiPlayer::trackerState(playState(state, stopped));
}

bool playerLiveSettled() { return UiPlayer::liveSettled(); }

void updateTrackerPlayerLive(const TrackerPlayer& player) {
    UiPlayer::trackerLive(trackerInfo(nullptr, player, nullptr));
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
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_SURFACE;
    uint16_t border = selected ? COLOR_SELECT : COLOR_RULE;
    int w = cellW - 2;
    Look::band(x, y, w, KB_KEY_H, bg, [&](TFT_eSPI& g) {
        g.drawRect(x, y, w, KB_KEY_H, border);
        int tw = Look::textWidth(Look::FONT_TEXT, key.label);
        uint16_t fg = key.kind == KEY_CHAR ? COLOR_TEXT : COLOR_ACCENT;
        Look::drawText(g, Look::FONT_TEXT, key.label, x + (w - tw) / 2, y + (KB_KEY_H - 7) / 2, fg);
    });
}

// The footer, with the second line red when it's an error.
void drawNameEntryFooter(const char* error) {
    Look::drawHintFooter("ARROWS move  ENTER select", error ? error : "DEL/OK on keyboard",
                         error ? COLOR_ERROR : COLOR_DIM);
}

void drawNameEntryPreview(const char* name, const char* suffix) {
    char preview[MAX_FILENAME_LEN + 8];
    snprintf(preview, sizeof(preview), "%s%s_", name, (suffix && suffix[0]) ? suffix : "");
    Look::band(0, NAME_PREVIEW_Y, tft.width(), ROW_HEIGHT, COLOR_BG, [&](TFT_eSPI& g) {
        Look::drawText(g, Look::FONT_TEXT, preview, 10, Look::rowTextY(NAME_PREVIEW_Y), COLOR_TEXT, 1);
    });
}

void drawNameEntry(const char* title, const char* name, const char* suffix,
                    const char* error, int keyRow, int keyCol) {
    statusBar(nullptr, nullptr, 0, title);

    tft.fillRect(0, HEADER_HEIGHT, tft.width(), NAME_PREVIEW_Y - HEADER_HEIGHT, COLOR_BG);
    drawNameEntryPreview(name, suffix);

    int keyboardTop = NAME_PREVIEW_Y + ROW_HEIGHT;
    if (KB_START_Y > keyboardTop) {
        tft.fillRect(0, keyboardTop, tft.width(), KB_START_Y - keyboardTop, COLOR_BG);
    }
    for (int row = 0; row < KEYBOARD_ROW_COUNT; row++) {
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
    drawNameEntryFooter(error);
}

void updateNameEntryKey(int prevRow, int prevCol, int newRow, int newCol) {
    drawKeyboardKey(prevRow, prevCol, false);
    drawKeyboardKey(newRow, newCol, true);
}

void updateNameEntryPreview(const char* name, const char* suffix) {
    drawNameEntryPreview(name, suffix);
}

void updateNameEntryError(const char* error) {
    drawNameEntryFooter(error);
}

// A menu row in the list under the status strip.
void drawMenuRow(const char* label, int idx, int scrollOffset, bool selected) {
    int row = idx - scrollOffset;
    if (row < 0 || row >= rowsOnScreen()) return;
    Look::drawTextRow(Look::LIST_TOP + row * ROW_HEIGHT, label, nullptr, 0, selected);
}

// One line under each mode name, by position.
const char* const MODE_NOTES[] = {
    "play and record",
    "4 tracks",
    "16 channels",
    "midi \x7f synth \x7f theme",
};

void drawModeRowAt(const char* const* labels, int count, int i, bool selected) {
    if (i < 0 || i >= count || i >= 4) return;
    Look::drawModeRow(i, labels[i], MODE_NOTES[i], selected);
}

void drawModeSelect(const char* const* labels, int count, int cursor) {
    Look::Status st;
    st.title = "MIDITRACKER";
    st.titleColor = COLOR_ACCENT;
    st.note = VERSIONNUMBER;
    Look::drawStatus(st);
    Look::clearModeList();
    for (int i = 0; i < count && i < 4; i++) drawModeRowAt(labels, count, i, i == cursor);
    static const Look::Key keys[] = {
        {"UD", "move", Look::TONE_TEXT},
        {"RIGHT/ENT", "open", Look::TONE_SELECT},
    };
    Look::drawKeyFooter(keys, 2);
}

void updateModeSelectCursor(const char* const* labels, int count, int prevCursor, int newCursor) {
    drawModeRowAt(labels, count, prevCursor, false);
    drawModeRowAt(labels, count, newCursor, true);
}

void drawEntryMenu(const char* subtitle, const char* const* labels, int count, int cursor, int scrollOffset) {
    statusBar(nullptr, nullptr, 0, subtitle);
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), Look::LIST_TOP - HEADER_HEIGHT, COLOR_BG);

    int rows = rowsOnScreen();
    for (int row = 0; row < rows; row++) {
        int idx = scrollOffset + row;
        drawMenuRow(idx < count ? labels[idx] : nullptr, idx, scrollOffset, idx == cursor);
    }

    int fy = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = Look::LIST_TOP + rows * ROW_HEIGHT;
    if (rowsBottom < fy) {
        tft.fillRect(0, rowsBottom, tft.width(), fy - rowsBottom, COLOR_BG);
    }
    drawScrollbar(count, scrollOffset, rows, Look::LIST_TOP);

    drawFooter("UP/DN move  ENTER select");
}

void updateEntryMenuSelection(const char* const* labels, int count, int prevCursor, int newCursor, int scrollOffset) {
    if (prevCursor >= 0 && prevCursor < count) drawMenuRow(labels[prevCursor], prevCursor, scrollOffset, false);
    if (newCursor >= 0 && newCursor < count) drawMenuRow(labels[newCursor], newCursor, scrollOffset, true);
}

void drawConfirmDelete(const char* name, bool isDir, bool failed) {
    Look::Card c;
    c.icon = &Look::ICON_WARNING;
    c.border = COLOR_ERROR;
    c.line1 = name;
    static const Look::Key ask[] = {
        {"NAV", "cancel", Look::TONE_TEXT},
        {"ENT", "delete", Look::TONE_ERROR},
    };
    static const Look::Key back[] = {
        {"NAV", "back", Look::TONE_TEXT},
    };
    if (failed) {
        c.title = "COULD NOT DELETE";
        c.line2 = isDir ? "folder not empty?" : nullptr;
        c.buttons = back;
        c.buttonCount = 1;
    } else {
        c.title = isDir ? "DELETE FOLDER" : "DELETE FILE";
        c.line2 = "this can't be undone";
        c.buttons = ask;
        c.buttonCount = 2;
    }
    Look::dimBody();
    Look::drawCard(c);
}

void drawConfirmOverwrite(const char* name, bool isDir) {
    Look::Card c;
    c.icon = &Look::ICON_WARNING;
    c.border = COLOR_ACCENT;
    c.title = isDir ? "OVERWRITE FOLDER" : "OVERWRITE FILE";
    c.line1 = name;
    c.line2 = "this can't be undone";
    static const Look::Key keys[] = {
        {"NAV", "cancel", Look::TONE_TEXT},
        {"ENT", "overwrite", Look::TONE_PLAY},
    };
    c.buttons = keys;
    c.buttonCount = 2;
    Look::dimBody();
    Look::drawCard(c);
}

char g_liveElapsed[24] = {0};
char g_liveCount[24] = {0};
int8_t g_liveDot = -1;
uint32_t g_logHash[12] = {0};

void resetLiveRows() {
    g_liveElapsed[0] = '\0';
    g_liveCount[0] = '\0';
    g_liveDot = -1;
    for (uint32_t& h : g_logHash) h = 0;
}

// dot < 0: this screen has no activity dot.
void drawLiveRows(const char* elapsed, const char* count, int dot) {
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    if (strcmp(elapsed, g_liveElapsed) != 0) {
        stripText(0, REC_ELAPSED_Y, tft.width(), ROW_HEIGHT, elapsed, 8, COLOR_DIM, COLOR_BG);
        strlcpy(g_liveElapsed, elapsed, sizeof(g_liveElapsed));
    }
    bool countChanged = strcmp(count, g_liveCount) != 0;
    if (countChanged) {
        stripText(0, REC_EVENTS_Y, tft.width(), ROW_HEIGHT, count, 8, COLOR_DIM, COLOR_BG);
        strlcpy(g_liveCount, count, sizeof(g_liveCount));
    }
    if (dot >= 0 && (countChanged || g_liveDot != (int8_t)dot)) {
        drawActivityDot(tft.width() - 16, REC_EVENTS_Y + ROW_HEIGHT / 2, dot != 0);
        g_liveDot = (int8_t)dot;
    }
}

void drawRecording(const char* filename, const MidiRecorder& recorder,
                    const MidiLogEntry* log, int logCount) {
    resetLiveRows();
    statusBar("RECORD", &Look::ICON_REC, COLOR_ERROR);

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
    if (recorder.state() != MidiRecorder::STATE_ERROR) {
        updateRecordingLog(log, logCount);
    }
    drawFooter("NAV/LEFT stop & save  EDIT discard");
}

void updateRecordingLive(const MidiRecorder& recorder) {
    // Only meaningful (and only called) while STATE_RECORDING, so the
    // fixed non-error layout offsets from drawRecording() apply here.
    char timeStr[16], elapsed[24], count[24];
    formatDuration(recorder.elapsedMs(), timeStr, sizeof(timeStr));
    snprintf(elapsed, sizeof(elapsed), "Elapsed: %s", timeStr);
    snprintf(count, sizeof(count), "Events: %lu", (unsigned long)recorder.eventCount());
    drawLiveRows(elapsed, count, recorder.msSinceLastEvent() < RECORDING_ACTIVITY_MS ? 1 : 0);
}

void updateRecordingLog(const MidiLogEntry* entries, int count) {
    int fy = tft.height() - FOOTER_HEIGHT;
    int rows = (fy - REC_LOG_Y) / ROW_HEIGHT;
    if (rows <= 0) return;
    int maxRows = (int)(sizeof(g_logHash) / sizeof(g_logHash[0]));
    if (rows > maxRows) rows = maxRows;

    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    // entries[] is oldest-first; show only the most recent `shown`, newest
    // at the bottom, so it reads like a scrolling log rather than a
    // shrinking one. Each row is repainted only if its text changed
    // (g_logHash holds a hash of what it shows, 0 for blank).
    int shown = count < 0 ? 0 : (count < rows ? count : rows);
    int startIdx = count - shown;
    for (int i = 0; i < rows; i++) {
        char buf[40];
        buf[0] = '\0';
        uint32_t h = 0;
        if (i < shown) {
            formatMidiLogLine(entries[startIdx + i], buf, sizeof(buf));
            h = 2166136261u;
            for (const char* c = buf; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
            if (h == 0) h = 1;
        }
        if (h == g_logHash[i]) continue;
        g_logHash[i] = h;
        tft.fillRect(0, REC_LOG_Y + i * ROW_HEIGHT, tft.width(), ROW_HEIGHT, COLOR_BG);
        if (h) tft.drawString(buf, 8, REC_LOG_Y + i * ROW_HEIGHT);
    }
}

void drawSysExCapture(const char* filename, const SysExRecorder& recorder,
                       const MidiLogEntry* log, int logCount) {
    resetLiveRows();
    statusBar("SYSEX CAPTURE", &Look::ICON_REC, COLOR_ERROR);

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
    if (recorder.state() != SysExRecorder::STATE_ERROR) {
        updateRecordingLog(log, logCount);
    }
    drawFooter("NAV/LEFT stop & save  EDIT discard");
}

void updateSysExCaptureLive(const SysExRecorder& recorder) {
    // Only meaningful (and only called) while STATE_RECORDING, so the
    // fixed non-error layout offsets from drawSysExCapture() apply here.
    char timeStr[16], elapsed[24], count[24];
    formatDuration(recorder.elapsedMs(), timeStr, sizeof(timeStr));
    snprintf(elapsed, sizeof(elapsed), "Elapsed: %s", timeStr);
    snprintf(count, sizeof(count), "Messages: %lu", (unsigned long)recorder.messageCount());
    drawLiveRows(elapsed, count, recorder.msSinceLastMessage() < RECORDING_ACTIVITY_MS ? 1 : 0);
}

void drawSysExPlayer(const char* filename, const SysExPlayer& player) {
    resetLiveRows();
    statusBar("SYSEX", &Look::ICON_NOTE, COLOR_DIM);

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

        drawSysExProgress(player, true);
    }

    // No footer hint line -- NAV/LEFT (stop & back) is this screen's only
    // input, and that's standard back-navigation, not called out here.
    drawFooter(nullptr);
}

void updateSysExPlayerLive(const SysExPlayer& player) {
    // Only meaningful (and only called) while STATE_SENDING, so the fixed
    // non-error layout offsets from drawSysExPlayer() apply here.
    char timeStr[16], elapsed[24], count[24];
    formatDuration(player.elapsedMs(), timeStr, sizeof(timeStr));
    snprintf(elapsed, sizeof(elapsed), "Elapsed: %s", timeStr);
    snprintf(count, sizeof(count), "Messages: %lu", (unsigned long)player.messagesSent());
    drawLiveRows(elapsed, count, -1);

    drawSysExProgress(player, false);
}

// -- Looper screen (ui_modes.cpp) -----------------------------------------

static UiModes::LoopBpmRow looperRow(float bpm, int timeSigNum, int timeSigDen, bool syncMode, bool metronomeOn,
                              int metronomeVolumePercent, bool showMetronomeVolume, bool countInEnabled,
                              int countInBars, bool showCountInBars, int focusIndex, bool selected) {
    UiModes::LoopBpmRow r;
    r.bpm = (int)(bpm + 0.5f);
    r.sigNum = timeSigNum;
    r.sigDen = timeSigDen;
    r.sync = syncMode;
    r.metroOn = metronomeOn;
    r.metroVolume = metronomeVolumePercent;
    r.showMetroVolume = showMetronomeVolume;
    r.countIn = countInEnabled;
    r.countInBars = countInBars;
    r.showCountInBars = showCountInBars;
    r.focus = focusIndex;
    r.selected = selected;
    // One bar in ms, as LooperMode's barsToMs(1).
    float beatMs = bpm > 0.0f ? 60000.0f / bpm * (4.0f / (timeSigDen > 0 ? timeSigDen : 4)) : 500.0f;
    r.barMs = (uint32_t)(timeSigNum * beatMs);
    return r;
}

void drawLooper(const LoopTrackView tracks[4], int selectedTrack, bool onBpmRow, bool syncMode, float bpm,
                 int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                 bool showMetronomeVolume, bool countInEnabled, int countInBars, bool showCountInBars,
                 int focusIndex, bool showBeatIndicator, int metronomeCurrentBeat) {
    UiModes::LoopBpmRow r = looperRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                      showMetronomeVolume, countInEnabled, countInBars, showCountInBars,
                                      focusIndex, onBpmRow);
    UiModes::looperDraw(tracks, selectedTrack, r, showBeatIndicator, metronomeCurrentBeat);
}

void updateLooperPositions(const LoopTrackView tracks[4], int selectedTrack) {
    UiModes::looperLive(tracks, selectedTrack);
}

void updateLooperSelection(const LoopTrackView tracks[4], int prevSelected, bool prevOnBpm,
                            int newSelected, bool newOnBpm, bool syncMode, float bpm,
                            int timeSigNum, int timeSigDen, bool metronomeOn, int metronomeVolumePercent,
                            bool showMetronomeVolume, bool countInEnabled, int countInBars,
                            bool showCountInBars, int focusIndex) {
    UiModes::LoopBpmRow r = looperRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                      showMetronomeVolume, countInEnabled, countInBars, showCountInBars,
                                      focusIndex, newOnBpm);
    UiModes::looperSelection(tracks, prevSelected, prevOnBpm, newSelected, newOnBpm, r);
}

void updateLooperTrackRow(const LoopTrackView tracks[4], int trackIdx, bool selected) {
    if (trackIdx < 0 || trackIdx > 3) return;
    UiModes::looperTrackRow(tracks[trackIdx], trackIdx, selected);
}

void updateLooperChannel(const LoopTrackView tracks[4], int selectedTrack) {
    if (selectedTrack < 0 || selectedTrack > 3) return;
    UiModes::looperTrackInfo(tracks[selectedTrack], selectedTrack, true);
}

void updateLooperBarLength(const LoopTrackView tracks[4], int selectedTrack) {
    if (selectedTrack < 0 || selectedTrack > 3) return;
    UiModes::looperTrackRow(tracks[selectedTrack], selectedTrack, true);
}

void updateLooperLength(const LoopTrackView tracks[4], int trackIdx, bool selected) {
    if (trackIdx < 0 || trackIdx > 3) return;
    UiModes::looperTrackLength(tracks[trackIdx], trackIdx, selected);
}

void updateLooperBpm(float bpm, int timeSigNum, int timeSigDen, bool syncMode, bool metronomeOn,
                      int metronomeVolumePercent, bool showMetronomeVolume, bool countInEnabled,
                      int countInBars, bool showCountInBars, int focusIndex, bool selected) {
    UiModes::looperBpm(looperRow(bpm, timeSigNum, timeSigDen, syncMode, metronomeOn, metronomeVolumePercent,
                                 showMetronomeVolume, countInEnabled, countInBars, showCountInBars,
                                 focusIndex, selected));
}

void updateLooperBpmRowBeatFlash(bool active, bool downbeat, bool selected) {
    (void)selected;
    UiModes::looperBeatFlash(active, downbeat);
}

void updateLooperBarBeat(int bar, int beat) { UiModes::looperBarBeat(bar, beat); }

void drawLooperCountIn(int beatsRemaining) { UiModes::looperCountIn(beatsRemaining); }

void updateLooperCountInBeat(int beatsRemaining) { UiModes::looperCountInBeat(beatsRemaining); }

void updateLooperBeatIndicator(bool visible, int currentBeat, int timeSigNum) {
    UiModes::looperBeats(visible, currentBeat, timeSigNum);
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
    Look::drawTextRow(SETTINGS_ROW_Y0 + row * ROW_HEIGHT, label, value, SETTINGS_VALUE_X, selected);
}

void settingsStatus(const char* pageTitle) {
    Look::Status st;
    st.icon = &Look::ICON_SLIDERS;
    st.iconColor = COLOR_DIM;
    st.title = "SETTINGS";
    st.text = pageTitle;
    Look::drawStatus(st);
    Look::drawIcon(tft, Look::ICON_KNOBS, 6, (Look::STATUS_H - 12) / 2, COLOR_TEXT);
}

// Icons for Settings' pages, by position.
void drawSettingsPageRow(const char* const* labels, const char* const* notes, int count, int i, bool selected) {
    if (i < 0 || i >= count || i >= 4) return;
    switch (i) {
        case 0: Look::drawIconRow(i, Look::ICON_WAVE, COLOR_SELECT, labels[i], notes[i], selected); break;
        case 1: Look::drawIconRow(i, Look::ICON_LOOPER, COLOR_ACCENT, labels[i], notes[i], selected); break;
        case 2: Look::drawIconRow(i, Look::ICON_METRONOME, COLOR_TEXT, labels[i], notes[i], selected); break;
        default: Look::drawIconRow(i, Look::ICON_NOTE, COLOR_MIDI, labels[i], notes[i], selected); break;
    }
}

void drawSettingsPageList(const char* const* labels, const char* const* notes, int count, int cursor) {
    settingsStatus(nullptr);
    Look::clearModeList();
    for (int i = 0; i < count && i < 4; i++) drawSettingsPageRow(labels, notes, count, i, i == cursor);
    static const Look::Key keys[] = {
        {"UD", "move", Look::TONE_TEXT},
        {"RIGHT/ENT", "open", Look::TONE_SELECT},
    };
    Look::drawKeyFooter(keys, 2);
}

void updateSettingsPageListCursor(const char* const* labels, const char* const* notes, int count, int prevCursor, int newCursor) {
    drawSettingsPageRow(labels, notes, count, prevCursor, false);
    drawSettingsPageRow(labels, notes, count, newCursor, true);
}

void drawSettings(const char* const* labels, const char* const* values, int count, int cursor,
                   int scrollOffset, const char* pageTitle,
                   const char* footerLine1, const char* footerLine2) {
    settingsStatus(pageTitle);
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), SETTINGS_ROW_Y0 - HEADER_HEIGHT, COLOR_BG);

    int rows = visibleRows();
    for (int row = 0; row < rows; row++) {
        int idx = scrollOffset + row;
        if (idx < count) {
            drawSettingsRow(labels[idx], values[idx], idx, scrollOffset, idx == cursor);
        } else {
            Look::drawTextRow(SETTINGS_ROW_Y0 + row * ROW_HEIGHT, nullptr, nullptr, 0, false);
        }
    }

    int bodyBottom = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = SETTINGS_ROW_Y0 + rows * ROW_HEIGHT;
    if (rowsBottom < bodyBottom) {
        tft.fillRect(0, rowsBottom, tft.width(), bodyBottom - rowsBottom, COLOR_BG);
    }
    drawScrollbar(count, scrollOffset, rows, SETTINGS_ROW_Y0);

    drawFooter(footerLine1, footerLine2);
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

// -- Theme page ---------------------------------------------------------

const int THEME_R_X = 130;
const int THEME_G_X = 178;
const int THEME_B_X = 226;
const int THEME_SWATCH_X = 278;
const int THEME_SWATCH_W = 30;
const int THEME_SWATCH_H = ROW_HEIGHT - 4;

// Draws (or erases) a single Theme page row: label, three R/G/B fields
// (the `channelCursor`-focused one drawn with foreground/background
// swapped, same inverted-field technique drawLooperBpmRow()'s Sync field
// uses for its own focus highlight), and a small solid swatch previewing
// the actual resulting color next to them. Used by both the full page
// redraw and the per-row partial updates below.
void drawThemeRow(int index, int channelCursor, bool selected, int scrollOffset) {
    int row = index - scrollOffset;
    if (row < 0 || row >= visibleRows()) return;
    int y = SETTINGS_ROW_Y0 + row * ROW_HEIGHT;

    uint8_t r, g, b;
    unpackRGB565(*THEME_COLORS[index].value, r, g, b);
    const uint8_t channels[3] = {r, g, b};
    const char* prefixes[3] = {"R", "G", "B"};
    const int xs[3] = {THEME_R_X, THEME_G_X, THEME_B_X};
    uint16_t swatch = *THEME_COLORS[index].value;

    Look::listRow(y, Look::LIST_W, selected, [&](TFT_eSPI& gfx, uint16_t bg) {
        int ty = Look::rowTextY(y);
        Look::drawText(gfx, Look::FONT_TEXT, THEME_COLORS[index].label, 10, ty, COLOR_TEXT);
        for (int ch = 0; ch < 3; ch++) {
            bool focused = selected && channelCursor == ch;
            char buf[6];
            snprintf(buf, sizeof(buf), "%s%3d", prefixes[ch], channels[ch]);
            uint16_t fg = COLOR_DIM;
            if (focused) {
                int w = Look::textWidth(Look::FONT_TEXT, buf) + 4;
                gfx.fillRect(xs[ch] - 2, y + 2, w, ROW_HEIGHT - 4, COLOR_SELECT);
                fg = COLOR_BG;
            } else if (selected) {
                fg = COLOR_TEXT;
            }
            Look::drawText(gfx, Look::FONT_TEXT, buf, xs[ch], ty, fg);
        }
        gfx.fillRect(THEME_SWATCH_X, y + 3, THEME_SWATCH_W, THEME_SWATCH_H, swatch);
        gfx.drawRect(THEME_SWATCH_X, y + 3, THEME_SWATCH_W, THEME_SWATCH_H, COLOR_RULE);
    });
}

void drawThemePage(int colorCursor, int channelCursor, int scrollOffset) {
    settingsStatus("Theme");
    tft.fillRect(0, HEADER_HEIGHT, tft.width(), SETTINGS_ROW_Y0 - HEADER_HEIGHT, COLOR_BG);

    int rows = visibleRows();
    for (int row = 0; row < rows; row++) {
        int idx = scrollOffset + row;
        if (idx < THEME_COLOR_COUNT) {
            drawThemeRow(idx, channelCursor, idx == colorCursor, scrollOffset);
        } else {
            Look::drawTextRow(SETTINGS_ROW_Y0 + row * ROW_HEIGHT, nullptr, nullptr, 0, false);
        }
    }

    int bodyBottom = tft.height() - FOOTER_HEIGHT;
    int rowsBottom = SETTINGS_ROW_Y0 + rows * ROW_HEIGHT;
    if (rowsBottom < bodyBottom) {
        tft.fillRect(0, rowsBottom, tft.width(), bodyBottom - rowsBottom, COLOR_BG);
    }
    drawScrollbar(THEME_COLOR_COUNT, scrollOffset, rows, SETTINGS_ROW_Y0);

    drawFooter("UD color  LR field  ENT menu", "EDIT+UD/LR value");
}

void updateThemeRow(int index, int channelCursor, bool selected, int scrollOffset) {
    drawThemeRow(index, channelCursor, selected, scrollOffset);
}

void updateThemeSelection(int prevCursor, int newCursor, int channelCursor, int scrollOffset) {
    if (prevCursor >= 0 && prevCursor < THEME_COLOR_COUNT) drawThemeRow(prevCursor, channelCursor, false, scrollOffset);
    if (newCursor >= 0 && newCursor < THEME_COLOR_COUNT) drawThemeRow(newCursor, channelCursor, true, scrollOffset);
    int rows = visibleRows();
    if (THEME_COLOR_COUNT > rows) drawScrollbar(THEME_COLOR_COUNT, scrollOffset, rows, SETTINGS_ROW_Y0);
}

int themeColorCount() { return THEME_COLOR_COUNT; }
const char* themeColorLabel(int index) { return THEME_COLORS[index].label; }

void getThemeColorRGB(int index, uint8_t& r, uint8_t& g, uint8_t& b) {
    unpackRGB565(*THEME_COLORS[index].value, r, g, b);
}

// Entries set since themeLoadBegin(), one bit each.
static uint32_t g_themeLoaded = 0;
static_assert(sizeof(THEME_COLORS) / sizeof(THEME_COLORS[0]) <= 32, "g_themeLoaded holds one bit per entry");

static int themeIndex(const char* label) {
    for (int i = 0; i < THEME_COLOR_COUNT; i++)
        if (strcmp(THEME_COLORS[i].label, label) == 0) return i;
    return -1;
}

void setThemeColorRGB(int index, uint8_t r, uint8_t g, uint8_t b) {
    *THEME_COLORS[index].value = packRGB565(r, g, b);
    g_themeLoaded |= 1u << index;
}

void resetThemeColorsToDefault() {
    for (int i = 0; i < THEME_COLOR_COUNT; i++) *THEME_COLORS[i].value = THEME_COLORS[i].def;
}

// The palette before a load, put back if the file had nothing usable.
static uint16_t g_themeBackup[THEME_COLOR_COUNT];

void themeLoadBegin() {
    for (int i = 0; i < THEME_COLOR_COUNT; i++) g_themeBackup[i] = *THEME_COLORS[i].value;
    resetThemeColorsToDefault();
    g_themeLoaded = 0;
}

// A theme saved before Surface, Rule and Select existed gets them from the
// entries that drew those parts then: Highlight was the header bar, Dim the
// lines and Accent the marked items.
void themeLoadEnd(bool ok) {
    if (!ok) {
        for (int i = 0; i < THEME_COLOR_COUNT; i++) *THEME_COLORS[i].value = g_themeBackup[i];
        return;
    }
    struct { const char* added; const char* from; } derive[] = {
        {"Surface", "Highlight"},
        {"Rule", "Dim"},
        {"Select", "Accent"},
    };
    for (auto& d : derive) {
        int to = themeIndex(d.added), from = themeIndex(d.from);
        if (to < 0 || from < 0) continue;
        if (!(g_themeLoaded & (1u << to)) && (g_themeLoaded & (1u << from)))
            *THEME_COLORS[to].value = *THEME_COLORS[from].value;
    }
}

// A card over the current screen. line2 in key form ("ENTER confirm  NAV
// cancel") becomes buttons, anything else a dim second line.
void drawMessage(const char* line1, const char* line2) {
    Look::Card c;
    c.line1 = line1;
    char buf[96];
    Look::Key keys[3];
    int n = line2 ? Look::parseHints(line2, buf, sizeof(buf), keys, 3) : 0;
    bool allKeys = n > 0;
    for (int i = 0; i < n; i++)
        if (!keys[i].key) allKeys = false;
    if (allKeys) {
        for (int i = 0; i < n; i++)
            if (strcmp(keys[i].key, "ENTER") == 0 || strcmp(keys[i].key, "ENT") == 0) keys[i].tone = Look::TONE_SELECT;
        c.buttons = keys;
        c.buttonCount = n;
    } else {
        c.line2 = line2;
    }
    // At boot there's no screen under it, so it goes on a plain one.
    if (Look::statusShown()) Look::dimBody();
    else tft.fillScreen(COLOR_BG);
    Look::drawCard(c);
}

// The battery and MIDI LEDs live in the status strip. Every screen draws
// the strip, so this only redraws them.
void drawBatteryMeter() {
    Look::statusBattery(Battery::percentage(), Battery::isCharging());
}

// The LEDs light on any MIDI in or out and hold for LED_HOLD_MS, so a single
// note shows. They change at most every LED_STEP_MS.
void updateBatteryMeter() {
    const uint32_t LED_HOLD_MS = 60;
    const uint32_t LED_STEP_MS = 30;
    static uint32_t lastIn = 0, lastOut = 0;
    static uint32_t inAt = 0, outAt = 0, stepAt = 0;
    uint32_t now = millis();
    uint32_t in = MidiOutput::inActivity();
    uint32_t out = MidiOutput::outActivity();
    if (in != lastIn) { lastIn = in; inAt = now; }
    if (out != lastOut) { lastOut = out; outAt = now; }
    if (now - stepAt >= LED_STEP_MS) {
        stepAt = now;
        Look::statusLeds(inAt && now - inAt < LED_HOLD_MS, outAt && now - outAt < LED_HOLD_MS);
    }
    Look::statusBattery(Battery::percentage(), Battery::isCharging());
}

} // namespace Ui

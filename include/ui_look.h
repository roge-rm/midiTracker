#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "ui_strip.h"

// The M12 look: palette, pixel fonts, icons and the shared widgets (status
// strip, keypad footer, list rows, cards). Core 0 only.
//
// Widgets draw in screen coordinates. Most go out through Look::band(),
// which draws into the DMA strip in bands and sends each one, so a widget
// never flickers and costs one strip send per 16 rows or so.
namespace Look {

// ---- palette ----------------------------------------------------------
// The theme entries in ui.cpp point at these, so a loaded .thm changes them.
extern uint16_t COLOR_BG;        // background
extern uint16_t COLOR_SURFACE;   // status strip, footer, tiles
extern uint16_t COLOR_RULE;      // rule lines and box borders
extern uint16_t COLOR_TEXT;
extern uint16_t COLOR_DIM;
extern uint16_t COLOR_HILITE_BG; // selected row and tile fill
extern uint16_t COLOR_SELECT;    // selection outline (cyan)
extern uint16_t COLOR_ACCENT;    // amber: playing, folders, titles
extern uint16_t COLOR_ERROR;     // record and error red
extern uint16_t COLOR_MIDI;      // .mid files
extern uint16_t COLOR_WAV;       // .wav files, charging

extern uint16_t COLOR_TRACK_ARMED;
extern uint16_t COLOR_TRACK_RECORDING;
extern uint16_t COLOR_TRACK_PLAYING;
extern uint16_t COLOR_TRACK_STOPPED;
extern uint16_t COLOR_TRACK_PAUSED;
extern uint16_t COLOR_PULSE;
extern uint16_t COLOR_DOWNBEAT;
extern uint16_t COLOR_NOTE_HIGH;
extern uint16_t COLOR_NOTE_MID;
extern uint16_t COLOR_NOTE_LOW;
extern uint16_t COLOR_PERC_HIGH;
extern uint16_t COLOR_PERC_MID;
extern uint16_t COLOR_PERC_LOW;

// #RRGGBB to RGB565.
constexpr uint16_t rgb(uint32_t hex) {
    return (uint16_t)((((hex >> 16) & 0xF8) << 8) | (((hex >> 8) & 0xFC) << 3) | ((hex & 0xFF) >> 3));
}

// ---- fonts ------------------------------------------------------------
// 1-bit glyphs in flash, made by tools/pixfont.cpp. yo is the glyph's top
// relative to the baseline. 0x7F holds a middle dot.
struct PixGlyph {
    uint16_t offset; // bit offset / 8 into bits, each glyph starts on a byte
    uint8_t w, h;
    int8_t xo, yo;
    uint8_t adv;
};
struct PixFont {
    const uint8_t* bits;
    const PixGlyph* glyphs;
    uint8_t first, last;
    uint8_t cap;     // cap height in pixels
    uint8_t descent; // pixels below the baseline
};

extern const PixFont FONT_LABEL; // Silkscreen, caps 5 px, 6 px advance
extern const PixFont FONT_TEXT;  // VT323, caps 7 px, 5 px advance

const char MIDDOT[] = "\x7f";

// Width of s in pixels at scale.
int textWidth(const PixFont& f, const char* s, int scale = 1);

// Draws s with the cap tops at y (baseline at y + cap * scale), background
// left alone. Silkscreen has no lower case; lower case letters come out as
// small caps. Bytes outside the font draw as '?', one per UTF-8 character.
// Returns the x after the last glyph.
int drawText(TFT_eSPI& g, const PixFont& f, const char* s, int x, int y, uint16_t fg, int scale = 1);

// Copies s into out, cut to fit maxW with "..." at the end, or at the
// front when keepEnd is set (for paths).
void fitText(const PixFont& f, const char* s, int maxW, char* out, size_t outSize, bool keepEnd = false);

// ---- icons ------------------------------------------------------------
// One uint16_t per row, the leftmost pixel in bit 15.
struct Icon {
    uint8_t w, h;
    const uint16_t* rows;
};
extern const Icon ICON_FILES;     // 12x12, folder with a play mark
extern const Icon ICON_LOOPER;    // 12x12
extern const Icon ICON_PARISYNTH; // 12x12
extern const Icon ICON_SLIDERS;   // 12x12, settings lines
extern const Icon ICON_KNOBS;     // 12x12, settings knobs, drawn over ICON_SLIDERS
extern const Icon ICON_FOLDER;    // 10x10
extern const Icon ICON_TRACKER;   // 10x10
extern const Icon ICON_NOTE;      // 10x10, MIDI
extern const Icon ICON_WAVE;      // 10x10, WAV
extern const Icon ICON_REC;       // 10x10
extern const Icon ICON_WARNING;   // 12x12
extern const Icon ICON_PLAY;      // 7x7
extern const Icon ICON_DIR;       // 3x5, folder badge

void drawIcon(TFT_eSPI& g, const Icon& icon, int x, int y, uint16_t c, int scale = 1);

// ---- layout -------------------------------------------------------------
const int SCREEN_W = 320;
const int SCREEN_H = 240;
const int STATUS_H = 16;           // the strip itself
const int HEADER_H = STATUS_H + 1; // plus its rule line
const int KEY_BOX_H = 11;
const int FOOTER_H = 31;           // rule, two rows of key boxes
const int FOOTER_H1 = 18;          // rule, one row
const int LIST_ROW_H = 18;
const int LIST_TOP = HEADER_H + 2;
const int LIST_W = SCREEN_W - 6;   // the scrollbar sits in the last 6 px

// The screen for direct draws. Ui::begin() sets it.
void setScreen(TFT_eSPI* tft);
TFT_eSPI& screen();

// Draws a w x h area through the strip: clears each band to bg, calls
// draw(g) with g's origin moved so screen coordinates land in the band,
// then sends it. draw may be called several times and must draw the same
// thing each time.
template <class F>
void band(int x, int y, int w, int h, uint16_t bg, F draw) {
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (w <= 0 || h <= 0) return;
    int rows = Ui::stripMaxRows(w);
    for (int by = 0; by < h; by += rows) {
        int bh = (h - by < rows) ? h - by : rows;
        TFT_eSprite& s = Ui::stripBegin(x, y + by, w, bh, bg);
        s.setViewport(-x, -(y + by), x + w, y + by + bh, true);
        draw(s);
        Ui::stripSend();
    }
}

// ---- status strip -------------------------------------------------------
// Right side: the MIDI in and out LEDs, or a label and value (the browser
// shows the output target there), then the battery.
struct Status {
    const Icon* icon = nullptr;
    uint16_t iconColor = 0;
    const char* title = nullptr;  // Silkscreen
    uint16_t titleColor = 0;      // 0 = COLOR_TEXT
    const char* note = nullptr;   // Silkscreen, dim, after the title (version)
    const char* text = nullptr;   // VT323, dim, after the title (path, name)
    const char* textLast = nullptr; // VT323, bright, straight after text
    const char* rightLabel = nullptr; // replaces the LEDs when set
    const char* rightValue = nullptr;
};

// Draws the strip and its rule line and remembers where the LEDs and
// battery went for updateStatus().
void drawStatus(const Status& st);

// Space the strip leaves for the title text, from x to the right-side
// items, for callers that fit a path into it.
int statusTextRoom(const Status& st);

// Set the LEDs and battery. Each redraws only its own few pixels, and only
// when what it shows changes, so they're cheap to call every loop.
void statusLeds(bool in, bool out);
void statusBattery(int percent, bool charging);

// Forgets the strip, for screens without one (splash).
void hideStatus();

// False on the splash and before any screen.
bool statusShown();

// ---- keypad footer ------------------------------------------------------
enum Tone : uint8_t { TONE_TEXT, TONE_PLAY, TONE_SELECT, TONE_DIM, TONE_ERROR };
struct Key {
    const char* key; // Silkscreen, dim
    const char* act; // VT323
    Tone tone;
};

// One or two rows of equal key boxes across the bottom, FOOTER_H tall with
// two rows, FOOTER_H1 with one.
void drawKeyFooter(const Key* row1, int n1, const Key* row2 = nullptr, int n2 = 0);

// Splits a hint line into keys: chunks are separated by two spaces, and a
// chunk whose first word is in capitals is a key with the rest its action,
// otherwise plain text (key null). Strings point into buf.
int parseHints(const char* line, char* buf, size_t bufSize, Key* keys, int maxKeys);

// Clears the gaps around the mode tiles.
void clearModeGrid();

// The same look from hint lines such as "UD move  ENTER select": each
// double-space chunk whose first word is in capitals becomes a key box,
// anything else is plain text. Always FOOTER_H tall; one line is centred.
// line2Color set draws line2 as plain text in that colour (an error).
void drawHintFooter(const char* line1, const char* line2, uint16_t line2Color = 0);

// ---- lists ----------------------------------------------------------------
// A row of a list: cyan stripe and outline when selected. Draws through the
// strip. The caller draws the content in the lambda.
template <class F>
void listRow(int y, int w, bool selected, F content) {
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_BG;
    band(0, y, w, LIST_ROW_H, bg, [&](TFT_eSPI& g) {
        if (selected) {
            g.drawRect(0, y, w, LIST_ROW_H, COLOR_SELECT);
            g.fillRect(0, y, 2, LIST_ROW_H, COLOR_SELECT);
        }
        content(g, bg);
    });
}

// Text top for a FONT_TEXT line centred in a list row at y.
inline int rowTextY(int y) { return y + (LIST_ROW_H - 7) / 2; }

// Scrollbar in the last 6 px of a list: count entries, rows on screen.
void drawScrollbar(int top, int rows, int count, int scrollOffset);

// File type badge: a 13x10 box with a letter, in the type colour.
void drawBadge(TFT_eSPI& g, int x, int y, const char* letter, bool dirIcon, uint16_t c);

// "212K", "3.1M".
void formatSize(uint32_t bytes, char* out, size_t outSize);

// A browser row: type badge, name, size and type. Null row blanks the slot.
enum RowType : uint8_t { ROW_DIR, ROW_TRACKER, ROW_MIDI, ROW_WAV, ROW_SYX };
struct FileRow {
    const char* name;
    RowType type;
    const char* typeLabel; // "XM", "MID"
    uint32_t size;
};
void drawFileRow(int y, const FileRow* row, bool selected);

// A menu or settings row: label, and value at valueX if set. Null label
// blanks the slot.
void drawTextRow(int y, const char* label, const char* value, int valueX, bool selected);

// ---- cards ----------------------------------------------------------------
// Dims the screen below the status strip with scanlines, so a card reads
// as being over it. A card still showing from before is cleared first.
void dimBody();

struct Card {
    const Icon* icon = nullptr;   // shown before the title
    const char* title = nullptr;  // Silkscreen
    const char* line1 = nullptr;  // VT323, text colour
    const char* line2 = nullptr;  // VT323, dim
    uint16_t border = 0;          // 0 = COLOR_RULE
    const Key* buttons = nullptr; // up to 3
    int buttonCount = 0;
};

// Draws a card in the middle of the screen. Call dimBody() first.
void drawCard(const Card& c);

// ---- mode select ----------------------------------------------------------
// One of the four tiles in a 2x2 grid.
void drawModeTile(int index, const char* label, const char* note, bool selected);

// ---- splash ---------------------------------------------------------------
void drawSplash(const char* version);

} // namespace Look

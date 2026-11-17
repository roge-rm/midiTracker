#include "ui_look.h"
#include "fonts/silkscreen_8.h"
#include "fonts/vt323_12.h"
#include <string.h>
#include <stdio.h>

namespace Look {

uint16_t COLOR_BG        = rgb(0x0A0E14);
uint16_t COLOR_SURFACE   = rgb(0x141A22);
uint16_t COLOR_RULE      = rgb(0x2A3340);
uint16_t COLOR_TEXT      = rgb(0xE6E9EE);
uint16_t COLOR_DIM       = rgb(0x8A95A5);
uint16_t COLOR_HILITE_BG = rgb(0x122028);
uint16_t COLOR_SELECT    = rgb(0x2FD4E6);
uint16_t COLOR_ACCENT    = rgb(0xFFB000);
uint16_t COLOR_ERROR     = rgb(0xFF3B30);
uint16_t COLOR_MIDI      = rgb(0xB08CFF);
uint16_t COLOR_WAV       = rgb(0x7FD67F);

uint16_t COLOR_TRACK_ARMED     = rgb(0xFF8A82);
uint16_t COLOR_TRACK_RECORDING = rgb(0xFF3B30);
uint16_t COLOR_TRACK_PLAYING   = rgb(0xFFB000);
uint16_t COLOR_TRACK_STOPPED   = rgb(0x8A95A5);
uint16_t COLOR_TRACK_PAUSED    = rgb(0x2FD4E6);
uint16_t COLOR_PULSE    = rgb(0x2FD4E6);
uint16_t COLOR_DOWNBEAT = rgb(0xFFB000);
uint16_t COLOR_NOTE_HIGH = rgb(0xFFB000);
uint16_t COLOR_NOTE_MID  = rgb(0x2FD4E6);
uint16_t COLOR_NOTE_LOW  = rgb(0x1F6E78);
uint16_t COLOR_PERC_HIGH = rgb(0xFF3B30);
uint16_t COLOR_PERC_MID  = rgb(0xB08CFF);
uint16_t COLOR_PERC_LOW  = rgb(0x5A4A80);

namespace {

// Icon bitmaps, the leftmost pixel in bit 15.
// 12x12
const uint16_t FILES_ROWS[] = {
    0x0000, // ............
    0x0000, // ............
    0x0000, // ............
    0x7C00, // .#####......
    0x43E0, // .#....#####.
    0x4020, // .#........#.
    0x4820, // .#..#.....#.
    0x4C20, // .#..##....#.
    0x4E20, // .#..###...#.
    0x4C20, // .#..##....#.
    0x4820, // .#..#.....#.
    0x7FE0, // .##########.
};
// 12x12
const uint16_t LOOPER_ROWS[] = {
    0x0000, // ............
    0x0F40, // ....####.#..
    0x3060, // ..##.....##.
    0x20E0, // ..#.....###.
    0x4000, // .#..........
    0x4010, // .#.........#
    0x4010, // .#.........#
    0x4010, // .#.........#
    0x2020, // ..#.......#.
    0x3060, // ..##.....##.
    0x0F80, // ....#####...
    0x0000, // ............
};
// 12x12
const uint16_t PARISYNTH_ROWS[] = {
    0x0000, // ............
    0x7DF0, // .#####.#####
    0x45F0, // .#...#.#####
    0x45F0, // .#...#.#####
    0x45F0, // .#...#.#####
    0x7DF0, // .#####.#####
    0x0000, // ............
    0x7DF0, // .#####.#####
    0x7D10, // .#####.#...#
    0x7D10, // .#####.#...#
    0x7D10, // .#####.#...#
    0x7DF0, // .#####.#####
};
// 12x12
const uint16_t SLIDERS_ROWS[] = {
    0x0000, // ............
    0x0000, // ............
    0x0000, // ............
    0x7FE0, // .##########.
    0x0000, // ............
    0x0000, // ............
    0x7FE0, // .##########.
    0x0000, // ............
    0x0000, // ............
    0x7FE0, // .##########.
    0x0000, // ............
    0x0000, // ............
};
// 12x12
const uint16_t KNOBS_ROWS[] = {
    0x0000, // ............
    0x0000, // ............
    0x0380, // ......###...
    0x0380, // ......###...
    0x0380, // ......###...
    0x1C00, // ...###......
    0x1C00, // ...###......
    0x1C00, // ...###......
    0x01C0, // .......###..
    0x01C0, // .......###..
    0x01C0, // .......###..
    0x0000, // ............
};
// 10x10
const uint16_t FOLDER_ROWS[] = {
    0x0000, // ..........
    0x0000, // ..........
    0xF000, // ####......
    0x8F80, // #...#####.
    0x8080, // #.......#.
    0x8080, // #.......#.
    0x8080, // #.......#.
    0x8080, // #.......#.
    0xFF80, // #########.
    0x0000, // ..........
};
// 10x10
const uint16_t TRACKER_ROWS[] = {
    0xFF80, // #########.
    0x8080, // #.......#.
    0xBE80, // #.#####.#.
    0x8080, // #.......#.
    0xBE80, // #.#####.#.
    0x8080, // #.......#.
    0xB880, // #.###...#.
    0x8080, // #.......#.
    0xFF80, // #########.
    0x0000, // ..........
};
// 10x10
const uint16_t NOTE_ROWS[] = {
    0x0700, // .....###..
    0x0580, // .....#.##.
    0x0480, // .....#..#.
    0x0400, // .....#....
    0x0400, // .....#....
    0x3C00, // ..####....
    0x7C00, // .#####....
    0x7C00, // .#####....
    0x3800, // ..###.....
    0x0000, // ..........
};
// 10x10
const uint16_t WAVE_ROWS[] = {
    0x0000, // ..........
    0x1000, // ...#......
    0x2800, // ..#.#.....
    0x2800, // ..#.#.....
    0x4440, // .#...#...#
    0x8440, // #....#...#
    0x0480, // .....#..#.
    0x0280, // ......#.#.
    0x0100, // .......#..
    0x0000, // ..........
};
// 10x10
const uint16_t REC_ROWS[] = {
    0x0000, // ..........
    0x1E00, // ...####...
    0x3F00, // ..######..
    0x7F80, // .########.
    0x7F80, // .########.
    0x7F80, // .########.
    0x7F80, // .########.
    0x3F00, // ..######..
    0x1E00, // ...####...
    0x0000, // ..........
};
// 12x12
const uint16_t WARNING_ROWS[] = {
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x0900, // ....#..#....
    0x0900, // ....#..#....
    0x1680, // ...#.##.#...
    0x1680, // ...#.##.#...
    0x2640, // ..#..##..#..
    0x2640, // ..#..##..#..
    0x4020, // .#........#.
    0x4620, // .#...##...#.
    0xFFF0, // ############
    0x0000, // ............
};
// 12x12
const uint16_t METRONOME_ROWS[] = {
    0x0610, // .....##....#
    0x0960, // ....#..#.##.
    0x0960, // ....#..#.##.
    0x10C0, // ...#....##..
    0x1080, // ...#....#...
    0x1180, // ...#...##...
    0x2140, // ..#....#.#..
    0x2240, // ..#...#..#..
    0x2640, // ..#..##..#..
    0x4020, // .#........#.
    0x7FE0, // .##########.
    0x0000, // ............
};
// 7x7
const uint16_t PLAY_ROWS[] = {
    0xC000, // ##.....
    0xF000, // ####...
    0xFC00, // ######.
    0xFE00, // #######
    0xFC00, // ######.
    0xF000, // ####...
    0xC000, // ##.....
};
// 3x5
const uint16_t DIR_ROWS[] = {
    0x8000, // #..
    0xC000, // ##.
    0xE000, // ###
    0xC000, // ##.
    0x8000, // #..
};
// 7x9
const uint16_t LOCK_ROWS[] = {
    0x3800, // ..###..
    0x4400, // .#...#.
    0x4400, // .#...#.
    0xFE00, // #######
    0xFE00, // #######
    0xEE00, // ###.###
    0xEE00, // ###.###
    0xFE00, // #######
    0xFE00, // #######
};

} // namespace

const Icon ICON_FILES = {12, 12, FILES_ROWS};
const Icon ICON_LOOPER = {12, 12, LOOPER_ROWS};
const Icon ICON_PARISYNTH = {12, 12, PARISYNTH_ROWS};
const Icon ICON_SLIDERS = {12, 12, SLIDERS_ROWS};
const Icon ICON_KNOBS = {12, 12, KNOBS_ROWS};
const Icon ICON_FOLDER = {10, 10, FOLDER_ROWS};
const Icon ICON_TRACKER = {10, 10, TRACKER_ROWS};
const Icon ICON_NOTE = {10, 10, NOTE_ROWS};
const Icon ICON_WAVE = {10, 10, WAVE_ROWS};
const Icon ICON_REC = {10, 10, REC_ROWS};
const Icon ICON_WARNING = {12, 12, WARNING_ROWS};
const Icon ICON_METRONOME = {12, 12, METRONOME_ROWS};
const Icon ICON_PLAY = {7, 7, PLAY_ROWS};
const Icon ICON_DIR = {3, 5, DIR_ROWS};
const Icon ICON_LOCK = {7, 9, LOCK_ROWS};

namespace {

TFT_eSPI* g_screen = nullptr;

// Status strip state, for the LED and battery redraws.
bool g_statusShown = false;
// The card on screen, cleared by the next dimBody() so a smaller card
// doesn't leave its edges. W 0 = none.
int g_cardX = 0, g_cardY = 0, g_cardW = 0, g_cardH = 0;
bool g_ledsShown = false;
int g_ledX[2] = {0, 0};
bool g_ledOn[2] = {false, false};
int g_battX = 0;
int g_battPct = -1;
bool g_battCharging = false;
bool g_battLow = false;
uint32_t g_battKey = 0xFFFFFFFF;

const int PAD = 6;
const int LED = 4;
const int LED_Y = (STATUS_H - LED) / 2;
// Bolt (5 px), a gap, then the battery (14 px with its nub).
const int BATT_W = 20, BATT_H = 7;
const int BOLT_W = 6;
const int BATT_Y = (STATUS_H - BATT_H) / 2;

const PixGlyph* glyphFor(const PixFont& f, unsigned char c) {
    if (c < f.first || c > f.last) c = '?';
    return &f.glyphs[c - f.first];
}

// Steps over one character, '?' for anything outside the font. Returns the
// byte to draw, or 0 for a UTF-8 continuation byte to skip.
inline unsigned char nextChar(const char*& p) {
    unsigned char c = (unsigned char)*p++;
    if (c >= 0x80) return (c & 0xC0) == 0x80 ? 0 : '?';
    return c;
}

uint16_t toneColor(Tone t) {
    switch (t) {
        case TONE_PLAY:   return COLOR_ACCENT;
        case TONE_SELECT: return COLOR_SELECT;
        case TONE_DIM:    return COLOR_DIM;
        case TONE_ERROR:  return COLOR_ERROR;
        default:          return COLOR_TEXT;
    }
}

uint32_t batteryKey(int pct, bool charging, bool low, int& fillW, uint16_t& fill) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fillW = (8 * pct + 50) / 100;
    if (pct > 0 && fillW == 0) fillW = 1;
    fill = COLOR_TEXT;
    if (low && !charging) fill = COLOR_ERROR;
    return (charging ? 1u << 31 : 0) | ((uint32_t)fill << 8) | (uint32_t)fillW;
}

void drawBattery(TFT_eSPI& g, int x, int y) {
    int fillW;
    uint16_t fill;
    g_battKey = batteryKey(g_battPct, g_battCharging, g_battLow, fillW, fill);
    // A small green bolt to the left while on USB power.
    if (g_battCharging) {
        static const uint8_t BOLT[BATT_H] = {0x03, 0x06, 0x0C, 0x1F, 0x06, 0x0C, 0x18};
        for (int r = 0; r < BATT_H; r++)
            for (int c = 0; c < 5; c++)
                if (BOLT[r] & (0x10 >> c)) g.drawPixel(x + c, y + r, COLOR_WAV);
    }
    x += BOLT_W;
    g.drawRect(x, y, 12, BATT_H, COLOR_TEXT);
    g.fillRect(x + 12, y + 2, 2, 3, COLOR_TEXT);
    if (fillW > 0) g.fillRect(x + 2, y + 2, fillW, BATT_H - 4, fill);
}

// Where the title text starts and where the right-hand items begin.
void statusLayout(const Status& st, int& textX, int& rightX, int& ledInX, int& ledOutX,
                  int& labelX, int& valueX) {
    int x = PAD;
    if (st.icon) x += st.icon->w + 4;
    if (st.title) x += textWidth(FONT_LABEL, st.title) + 4;
    if (st.note) x += textWidth(FONT_LABEL, st.note) + 4;
    textX = x;

    int r = SCREEN_W - PAD - BATT_W;
    g_battX = r;
    if (st.rightLabel) {
        int vw = st.rightValue ? textWidth(FONT_LABEL, st.rightValue) : 0;
        valueX = r - PAD - vw;
        labelX = valueX - 3 - textWidth(FONT_LABEL, st.rightLabel);
        rightX = labelX;
        ledInX = ledOutX = -1;
    } else {
        ledOutX = r - PAD - LED;
        int outLabel = ledOutX - 3 - textWidth(FONT_LABEL, "OUT");
        ledInX = outLabel - PAD - LED;
        int inLabel = ledInX - 3 - textWidth(FONT_LABEL, "IN");
        rightX = inLabel;
        labelX = valueX = -1;
    }
}

void drawKeyBox(TFT_eSPI& g, int x, int y, int w, const Key& k) {
    g.fillRect(x, y, w, KEY_BOX_H, COLOR_BG);
    g.drawRect(x, y, w, KEY_BOX_H, COLOR_RULE);
    int tx = x + 3;
    if (k.key && k.key[0]) {
        tx = drawText(g, FONT_LABEL, k.key, tx, y + 3, COLOR_DIM) + 2;
    }
    if (k.act && k.act[0]) {
        char buf[48];
        fitText(FONT_TEXT, k.act, x + w - 2 - tx, buf, sizeof(buf));
        drawText(g, FONT_TEXT, buf, tx, y + 2, toneColor(k.tone));
    }
}

int keyNaturalW(const Key& k) {
    int w = 6;
    if (k.key && k.key[0]) w += textWidth(FONT_LABEL, k.key) + 2;
    if (k.act && k.act[0]) w += textWidth(FONT_TEXT, k.act);
    return w;
}

const int FOOTER_X = PAD;
const int FOOTER_W = SCREEN_W - 2 * PAD;
const int KEY_GAP = 3;

// Lays one row of keys out across the footer. Equal boxes when they all
// fit, boxes sized to their text when not, plain text when even that
// overflows.
void drawKeyRow(TFT_eSPI& g, const Key* keys, int n, int y) {
    if (n <= 0) return;
    int share = (FOOTER_W - KEY_GAP * (n - 1)) / n;
    int widest = 0, total = KEY_GAP * (n - 1);
    for (int i = 0; i < n; i++) {
        int w = keyNaturalW(keys[i]);
        if (w > widest) widest = w;
        total += w;
    }
    if (widest <= share) {
        int x = FOOTER_X;
        for (int i = 0; i < n; i++) {
            int w = (i == n - 1) ? FOOTER_X + FOOTER_W - x : share;
            drawKeyBox(g, x, y, w, keys[i]);
            x += w + KEY_GAP;
        }
    } else if (total <= FOOTER_W) {
        int x = FOOTER_X;
        int extra = (FOOTER_W - total) / n;
        for (int i = 0; i < n; i++) {
            int w = keyNaturalW(keys[i]) + extra;
            drawKeyBox(g, x, y, w, keys[i]);
            x += w + KEY_GAP;
        }
    } else {
        int x = FOOTER_X;
        for (int i = 0; i < n; i++) {
            if (keys[i].key && keys[i].key[0]) x = drawText(g, FONT_LABEL, keys[i].key, x, y + 3, COLOR_DIM) + 2;
            if (keys[i].act && keys[i].act[0]) x = drawText(g, FONT_TEXT, keys[i].act, x, y + 2, toneColor(keys[i].tone));
            x += 6;
        }
    }
}

void drawFooterBand(int h, const Key* row1, int n1, const Key* row2, int n2, const char* plain2, uint16_t plain2Color) {
    int top = SCREEN_H - h;
    screen().drawFastHLine(0, top, SCREEN_W, COLOR_RULE);
    bool two = n2 > 0 || plain2;
    int y1 = (h == FOOTER_H && !two) ? top + 1 + (h - 1 - KEY_BOX_H) / 2 : top + 4;
    int y2 = y1 + KEY_BOX_H + 2;
    band(0, top + 1, SCREEN_W, h - 1, COLOR_SURFACE, [&](TFT_eSPI& g) {
        drawKeyRow(g, row1, n1, y1);
        if (n2 > 0) drawKeyRow(g, row2, n2, y2);
        else if (plain2) drawText(g, FONT_TEXT, plain2, FOOTER_X, y2 + 2, plain2Color);
    });
}

} // namespace

// ---- fonts ------------------------------------------------------------

int textWidth(const PixFont& f, const char* s, int scale) {
    if (!s) return 0;
    int w = 0;
    const char* p = s;
    while (*p) {
        unsigned char c = nextChar(p);
        if (c) w += glyphFor(f, c)->adv;
    }
    return w * scale;
}

int drawText(TFT_eSPI& g, const PixFont& f, const char* s, int x, int y, uint16_t fg, int scale) {
    if (!s) return x;
    int base = y + f.cap * scale;
    const char* p = s;
    while (*p) {
        unsigned char c = nextChar(p);
        if (!c) continue;
        const PixGlyph* gl = glyphFor(f, c);
        const uint8_t* bits = f.bits + gl->offset;
        int gx = x + gl->xo * scale;
        int gy = base + gl->yo * scale;
        int bit = 0;
        for (int row = 0; row < gl->h; row++) {
            int runStart = -1;
            for (int col = 0; col <= gl->w; col++) {
                bool on = false;
                if (col < gl->w) {
                    on = (bits[bit >> 3] >> (7 - (bit & 7))) & 1;
                    bit++;
                }
                if (on && runStart < 0) runStart = col;
                if (!on && runStart >= 0) {
                    if (scale == 1) g.drawFastHLine(gx + runStart, gy + row, col - runStart, fg);
                    else g.fillRect(gx + runStart * scale, gy + row * scale, (col - runStart) * scale, scale, fg);
                    runStart = -1;
                }
            }
        }
        x += gl->adv * scale;
    }
    return x;
}

void fitText(const PixFont& f, const char* s, int maxW, char* out, size_t outSize, bool keepEnd) {
    if (!outSize) return;
    out[0] = '\0';
    if (!s) return;
    if (textWidth(f, s) <= maxW) {
        strncpy(out, s, outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }
    int dots = textWidth(f, "...");
    if (dots > maxW) {
        // Not even "..." fits: as many dots as do.
        int dw = textWidth(f, ".");
        size_t n = 0;
        for (int w = dw; w <= maxW && n < 3 && n + 1 < outSize; w += dw) out[n++] = '.';
        out[n] = '\0';
        return;
    }
    if (keepEnd) {
        const char* tail = s;
        while (*tail && textWidth(f, tail) + dots > maxW) {
            tail++;
            while ((*tail & 0xC0) == 0x80) tail++;
        }
        snprintf(out, outSize, "...%s", tail);
        return;
    }
    size_t n = 0;
    int w = 0;
    const char* p = s;
    while (*p && n + 4 < outSize) {
        const char* q = p;
        unsigned char c = nextChar(q);
        int cw = c ? glyphFor(f, c)->adv : 0;
        if (w + cw + dots > maxW) break;
        w += cw;
        while (p < q && n + 4 < outSize) out[n++] = *p++;
    }
    memcpy(out + n, "...", 4);
}

// ---- icons ------------------------------------------------------------

void drawIcon(TFT_eSPI& g, const Icon& icon, int x, int y, uint16_t c, int scale) {
    for (int row = 0; row < icon.h; row++) {
        uint16_t bits = icon.rows[row];
        int col = 0;
        while (col < icon.w) {
            if (!(bits & (0x8000 >> col))) {
                col++;
                continue;
            }
            int start = col;
            while (col < icon.w && (bits & (0x8000 >> col))) col++;
            g.fillRect(x + start * scale, y + row * scale, (col - start) * scale, scale, c);
        }
    }
}

// ---- screen -------------------------------------------------------------

void setScreen(TFT_eSPI* tft) { g_screen = tft; }
TFT_eSPI& screen() { return *g_screen; }

// ---- status strip -------------------------------------------------------

int statusTextRoom(const Status& st) {
    int textX, rightX, a, b, c, d;
    statusLayout(st, textX, rightX, a, b, c, d);
    return rightX - PAD - textX;
}

void drawStatus(const Status& st) {
    int textX, rightX, ledIn, ledOut, labelX, valueX;
    statusLayout(st, textX, rightX, ledIn, ledOut, labelX, valueX);
    band(0, 0, SCREEN_W, STATUS_H, COLOR_SURFACE, [&](TFT_eSPI& g) {
        int x = PAD;
        if (st.icon) {
            drawIcon(g, *st.icon, x, (STATUS_H - st.icon->h) / 2, st.iconColor);
            x += st.icon->w + 4;
        }
        if (st.title) x = drawText(g, FONT_LABEL, st.title, x, 5, st.titleColor ? st.titleColor : COLOR_TEXT) + 4;
        if (st.note) x = drawText(g, FONT_LABEL, st.note, x, 5, COLOR_DIM) + 4;
        if (st.text) x = drawText(g, FONT_TEXT, st.text, x, 4, COLOR_DIM);
        if (st.textLast) drawText(g, FONT_TEXT, st.textLast, x, 4, COLOR_TEXT);
        if (st.rightLabel) {
            drawText(g, FONT_LABEL, st.rightLabel, labelX, 5, COLOR_DIM);
            if (st.rightValue) drawText(g, FONT_LABEL, st.rightValue, valueX, 5, COLOR_SELECT);
        } else {
            drawText(g, FONT_LABEL, "IN", ledIn - 3 - textWidth(FONT_LABEL, "IN"), 5, COLOR_DIM);
            drawText(g, FONT_LABEL, "OUT", ledOut - 3 - textWidth(FONT_LABEL, "OUT"), 5, COLOR_DIM);
            g.fillRect(ledIn, LED_Y, LED, LED, g_ledOn[0] ? COLOR_SELECT : COLOR_RULE);
            g.fillRect(ledOut, LED_Y, LED, LED, g_ledOn[1] ? COLOR_SELECT : COLOR_RULE);
        }
        drawBattery(g, g_battX, BATT_Y);
    });
    screen().drawFastHLine(0, STATUS_H, SCREEN_W, COLOR_RULE);
    g_statusShown = true;
    g_cardW = 0;
    g_ledsShown = !st.rightLabel;
    g_ledX[0] = ledIn;
    g_ledX[1] = ledOut;
}

void statusLeds(bool in, bool out) {
    bool on[2] = {in, out};
    for (int i = 0; i < 2; i++) {
        if (on[i] == g_ledOn[i]) continue;
        g_ledOn[i] = on[i];
        if (g_statusShown && g_ledsShown) {
            screen().fillRect(g_ledX[i], LED_Y, LED, LED, on[i] ? COLOR_SELECT : COLOR_RULE);
        }
    }
}

void statusBattery(int percent, bool charging, bool low) {
    g_battPct = percent;
    g_battCharging = charging;
    g_battLow = low;
    int fillW;
    uint16_t fill;
    if (batteryKey(percent, charging, low, fillW, fill) == g_battKey) return;
    if (!g_statusShown) return;
    TFT_eSPI& g = screen();
    g.fillRect(g_battX, BATT_Y, BATT_W, BATT_H, COLOR_SURFACE);
    drawBattery(g, g_battX, BATT_Y);
}

void hideStatus() {
    g_statusShown = false;
    g_cardW = 0;
}

bool statusShown() { return g_statusShown; }

// ---- footer -------------------------------------------------------------

int parseHints(const char* line, char* buf, size_t bufSize, Key* keys, int maxKeys) {
    if (!line || !line[0]) return 0;
    strncpy(buf, line, bufSize - 1);
    buf[bufSize - 1] = '\0';
    int n = 0;
    char* p = buf;
    while (*p && n < maxKeys) {
        while (*p == ' ') p++;
        if (!*p) break;
        char* end = strstr(p, "  ");
        if (end) *end = '\0';
        char* sp = strchr(p, ' ');
        bool isKey = sp != nullptr;
        bool letter = false;
        for (char* q = p; isKey && q < sp; q++) {
            char c = *q;
            if (c >= 'A' && c <= 'Z') letter = true;
            else if (!((c >= '0' && c <= '9') || c == '+' || c == '/' || c == '&')) isKey = false;
        }
        if (isKey && letter) {
            *sp = '\0';
            keys[n++] = {p, sp + 1, TONE_TEXT};
        } else {
            keys[n++] = {nullptr, p, TONE_TEXT};
        }
        if (!end) break;
        p = end + 2;
    }
    return n;
}


void drawKeyFooter(const Key* row1, int n1, const Key* row2, int n2) {
    drawFooterBand(n2 > 0 ? FOOTER_H : FOOTER_H1, row1, n1, row2, n2, nullptr, 0);
}

void drawHintFooter(const char* line1, const char* line2, uint16_t line2Color) {
    char buf1[96], buf2[96];
    Key k1[6], k2[6];
    int n1 = parseHints(line1, buf1, sizeof(buf1), k1, 6);
    if (line2Color) {
        drawFooterBand(FOOTER_H, k1, n1, nullptr, 0, (line2 && line2[0]) ? line2 : nullptr, line2Color);
        return;
    }
    int n2 = parseHints(line2, buf2, sizeof(buf2), k2, 6);
    if (n1 == 0 && n2 > 0) {
        drawFooterBand(FOOTER_H, k2, n2, nullptr, 0, nullptr, 0);
        return;
    }
    drawFooterBand(FOOTER_H, k1, n1, k2, n2, nullptr, 0);
}

// ---- lists ----------------------------------------------------------------

void drawScrollbar(int top, int rows, int count, int scrollOffset) {
    TFT_eSPI& g = screen();
    int x = LIST_W + 1, w = 3;
    int y = top + 1, h = rows * LIST_ROW_H - 2;
    // Clear the margin around the bar so the whole column is drawn.
    g.fillRect(LIST_W, top, 1, h + 2, COLOR_BG);
    g.fillRect(x + w, top, SCREEN_W - x - w, h + 2, COLOR_BG);
    g.fillRect(x, top, w, 1, COLOR_BG);
    g.fillRect(x, y + h, w, 1, COLOR_BG);
    if (count <= rows) {
        g.fillRect(x, y, w, h, COLOR_BG);
        return;
    }
    int thumbH = h * rows / count;
    if (thumbH < 8) thumbH = 8;
    int maxScroll = count - rows;
    int thumbY = maxScroll > 0 ? (h - thumbH) * scrollOffset / maxScroll : 0;
    if (thumbY < 0) thumbY = 0;
    if (thumbY > h - thumbH) thumbY = h - thumbH;
    if (thumbY > 0) g.fillRect(x, y, w, thumbY, COLOR_SURFACE);
    g.fillRect(x, y + thumbY, w, thumbH, COLOR_SELECT);
    if (thumbY + thumbH < h) g.fillRect(x, y + thumbY + thumbH, w, h - thumbY - thumbH, COLOR_SURFACE);
}

void drawBadge(TFT_eSPI& g, int x, int y, const char* letter, bool dirIcon, uint16_t c) {
    g.drawRect(x, y, 13, 10, c);
    if (dirIcon) drawIcon(g, ICON_DIR, x + 5, y + 2, c);
    else if (letter) drawText(g, FONT_LABEL, letter, x + 13 / 2 - textWidth(FONT_LABEL, letter) / 2, y + 2, c);
}

void formatSize(uint32_t bytes, char* out, size_t outSize) {
    if (bytes < 1000) {
        snprintf(out, outSize, "%luB", (unsigned long)bytes);
    } else if (bytes < 1000UL * 1024) {
        snprintf(out, outSize, "%luK", (unsigned long)((bytes + 1023) / 1024));
    } else if (bytes < 10UL * 1024 * 1024) {
        uint32_t tenths = (uint32_t)(((uint64_t)bytes * 10 + 512 * 1024) / (1024 * 1024));
        snprintf(out, outSize, "%lu.%luM", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
    } else {
        snprintf(out, outSize, "%luM", (unsigned long)((bytes + 512 * 1024) / (1024 * 1024)));
    }
}

void drawFileRow(int y, const FileRow* row, bool selected) {
    if (!row) {
        band(0, y, LIST_W, LIST_ROW_H, COLOR_BG, [](TFT_eSPI&) {});
        return;
    }
    uint16_t tc;
    const char* letter;
    switch (row->type) {
        case ROW_DIR:     tc = COLOR_ACCENT; letter = nullptr; break;
        case ROW_MIDI:    tc = COLOR_MIDI;   letter = "M"; break;
        case ROW_WAV:     tc = COLOR_WAV;    letter = "W"; break;
        case ROW_SYX:     tc = COLOR_DIM;    letter = "S"; break;
        default:          tc = COLOR_SELECT; letter = "T"; break;
    }
    bool dir = row->type == ROW_DIR;
    listRow(y, LIST_W, selected, [&](TFT_eSPI& g, uint16_t) {
        drawBadge(g, 6, y + 4, letter, dir, tc);
        const int typeRight = LIST_W - 6;
        const int sizeRight = typeRight - 24;
        int nameMax = dir ? typeRight - 24 : sizeRight - 30 - 24;
        char buf[72];
        fitText(FONT_TEXT, row->name, nameMax, buf, sizeof(buf));
        uint16_t nc = dir ? COLOR_ACCENT : COLOR_TEXT;
        drawText(g, FONT_TEXT, buf, 24, rowTextY(y), nc);
        if (!dir) {
            char sz[12];
            formatSize(row->size, sz, sizeof(sz));
            drawText(g, FONT_TEXT, sz, sizeRight - textWidth(FONT_TEXT, sz), rowTextY(y), COLOR_DIM);
            if (row->typeLabel) {
                drawText(g, FONT_LABEL, row->typeLabel, typeRight - textWidth(FONT_LABEL, row->typeLabel) + 1,
                         y + (LIST_ROW_H - 5) / 2, tc);
            }
        }
    });
}

void drawTextRow(int y, const char* label, const char* value, int valueX, bool selected) {
    if (!label) {
        band(0, y, LIST_W, LIST_ROW_H, COLOR_BG, [](TFT_eSPI&) {});
        return;
    }
    listRow(y, LIST_W, selected, [&](TFT_eSPI& g, uint16_t) {
        char buf[64];
        int labelMax = (value && valueX > 0 ? valueX - 8 : LIST_W - 8) - 10;
        fitText(FONT_TEXT, label, labelMax, buf, sizeof(buf));
        drawText(g, FONT_TEXT, buf, 10, rowTextY(y), COLOR_TEXT);
        if (value && valueX > 0) {
            fitText(FONT_TEXT, value, LIST_W - 6 - valueX, buf, sizeof(buf));
            drawText(g, FONT_TEXT, buf, valueX, rowTextY(y), selected ? COLOR_SELECT : COLOR_DIM);
        }
    });
}

// ---- cards ----------------------------------------------------------------

void dimBody() {
    TFT_eSPI& g = screen();
    const uint16_t shade = rgb(0x05070A);
    g.startWrite();
    if (g_cardW) g.fillRect(g_cardX, g_cardY, g_cardW, g_cardH, COLOR_BG);
    g_cardW = 0;
    for (int y = HEADER_H + 1; y < SCREEN_H; y += 2) g.drawFastHLine(0, y, SCREEN_W, shade);
    g.endWrite();
}

void drawCard(const Card& c) {
    const int B = 2;
    // 210 wide, wider up to 300 when a line or the buttons need it.
    int W = 210;
    int n = c.buttonCount > 3 ? 3 : c.buttonCount;
    int btnW = 0;
    for (int i = 0; i < n; i++) {
        const Key& k = c.buttons[i];
        int w = (k.key ? textWidth(FONT_LABEL, k.key) + 3 : 0) + textWidth(FONT_TEXT, k.act) + 12;
        if (w > btnW) btnW = w;
    }
    int need = n > 0 ? n * btnW + 5 * (n - 1) + 2 * B + 16 : 0;
    int l1 = textWidth(FONT_TEXT, c.line1) + 2 * B + 16;
    int l2 = textWidth(FONT_TEXT, c.line2) + 2 * B + 16;
    if (need > W) W = need;
    if (l1 > W) W = l1;
    if (l2 > W) W = l2;
    if (W > 300) W = 300;
    W &= ~1;
    const int X = (SCREEN_W - W) / 2;
    const int TITLE_H = 24;
    const int LINE_H = 14;
    const int BTN_H = 18;
    int lines = (c.line1 ? 1 : 0) + (c.line2 ? 1 : 0);
    int h = B + (c.title ? TITLE_H + 1 : 0) + 7 + lines * LINE_H + 7 + (c.buttonCount > 0 ? BTN_H + 8 : 0) + B;
    int y = (SCREEN_H - h) / 2;
    if (y < HEADER_H + 4) y = HEADER_H + 4;
    uint16_t border = c.border ? c.border : COLOR_RULE;
    g_cardX = X;
    g_cardY = y;
    g_cardW = W;
    g_cardH = h;
    band(X, y, W, h, COLOR_SURFACE, [&](TFT_eSPI& g) {
        g.fillRect(X, y, W, B, border);
        g.fillRect(X, y + h - B, W, B, border);
        g.fillRect(X, y, B, h, border);
        g.fillRect(X + W - B, y, B, h, border);
        int cy = y + B;
        int tx = X + B + 8;
        if (c.title) {
            int x = tx;
            if (c.icon) {
                drawIcon(g, *c.icon, x, cy + (TITLE_H - c.icon->h) / 2, border);
                x += c.icon->w + 5;
            }
            drawText(g, FONT_LABEL, c.title, x, cy + (TITLE_H - 5) / 2, COLOR_TEXT);
            cy += TITLE_H;
            g.drawFastHLine(X + B, cy, W - 2 * B, COLOR_RULE);
            cy += 1;
        }
        cy += 7;
        char buf[64];
        int maxW = W - 2 * B - 16;
        if (c.line1) {
            fitText(FONT_TEXT, c.line1, maxW, buf, sizeof(buf));
            drawText(g, FONT_TEXT, buf, tx, cy + 3, COLOR_TEXT);
            cy += LINE_H;
        }
        if (c.line2) {
            fitText(FONT_TEXT, c.line2, maxW, buf, sizeof(buf));
            drawText(g, FONT_TEXT, buf, tx, cy + 3, COLOR_DIM);
            cy += LINE_H;
        }
        cy += 7;
        if (n > 0) {
            int bw = (W - 2 * B - 16 - 5 * (n - 1)) / n;
            int bx = tx;
            for (int i = 0; i < n; i++) {
                const Key& k = c.buttons[i];
                bool strong = k.tone == TONE_ERROR || k.tone == TONE_PLAY || k.tone == TONE_SELECT;
                uint16_t edge = strong ? toneColor(k.tone) : COLOR_RULE;
                g.fillRect(bx, cy, bw, BTN_H, COLOR_BG);
                g.drawRect(bx, cy, bw, BTN_H, edge);
                int kw = k.key ? textWidth(FONT_LABEL, k.key) + 3 : 0;
                char act[40];
                fitText(FONT_TEXT, k.act ? k.act : "", bw - 8 - kw, act, sizeof(act));
                int cw = kw + textWidth(FONT_TEXT, act);
                int x = bx + (bw - cw) / 2;
                if (k.key) x = drawText(g, FONT_LABEL, k.key, x, cy + (BTN_H - 5) / 2, COLOR_DIM) + 3;
                drawText(g, FONT_TEXT, act, x, cy + (BTN_H - 7) / 2, toneColor(k.tone));
                bx += bw + 5;
            }
        }
    });
}

// ---- mode select ----------------------------------------------------------

namespace {
// Four rows filling the space between the strip and a one-row footer.
const int MODE_X = 10, MODE_W = SCREEN_W - 2 * MODE_X;
const int MODE_ROW_H = 42, MODE_GAP = 6;
const int MODE_BODY_H = SCREEN_H - FOOTER_H1 - HEADER_H;
const int MODE_Y = HEADER_H + (MODE_BODY_H - 4 * MODE_ROW_H - 3 * MODE_GAP) / 2;
}

void clearModeList() {
    TFT_eSPI& g = screen();
    int bottom = SCREEN_H - FOOTER_H1;
    int end = MODE_Y + 4 * MODE_ROW_H + 3 * MODE_GAP;
    g.fillRect(0, HEADER_H, SCREEN_W, MODE_Y - HEADER_H, COLOR_BG);
    g.fillRect(0, end, SCREEN_W, bottom - end, COLOR_BG);
    g.fillRect(0, MODE_Y, MODE_X, end - MODE_Y, COLOR_BG);
    g.fillRect(MODE_X + MODE_W, MODE_Y, SCREEN_W - MODE_X - MODE_W, end - MODE_Y, COLOR_BG);
    for (int i = 0; i < 3; i++) {
        g.fillRect(MODE_X, MODE_Y + (i + 1) * MODE_ROW_H + i * MODE_GAP, MODE_W, MODE_GAP, COLOR_BG);
    }
}

namespace {
// One row of the list: frame, the icon drawn by drawIconFn(g, x, y) in a
// 24 px square, label, note and the play arrow when selected.
template <typename F>
void listRow(int index, const char* label, const char* note, bool selected, F drawIconFn) {
    int x = MODE_X, w = MODE_W, h = MODE_ROW_H;
    int y = MODE_Y + index * (MODE_ROW_H + MODE_GAP);
    uint16_t bg = selected ? COLOR_HILITE_BG : COLOR_SURFACE;
    band(x, y, w, h, bg, [&](TFT_eSPI& g) {
        if (selected) {
            g.drawRect(x, y, w, h, COLOR_SELECT);
            g.fillRect(x, y, 2, h, COLOR_SELECT);
        } else {
            g.drawRect(x, y, w, h, COLOR_RULE);
        }
        int ix = x + 12, iy = y + (h - 24) / 2;
        drawIconFn(g, ix, iy);
        // Label caps 10 px, 5 px gap, note caps 7 px: 22 px centred on the icon.
        int tx = ix + 24 + 12, ty = y + (h - 22) / 2;
        drawText(g, FONT_LABEL, label, tx, ty, selected ? COLOR_SELECT : COLOR_TEXT, 2);
        if (note) {
            char buf[48];
            fitText(FONT_TEXT, note, x + w - 20 - tx, buf, sizeof(buf));
            drawText(g, FONT_TEXT, buf, tx, ty + 15, COLOR_DIM);
        }
        if (selected) drawIcon(g, ICON_PLAY, x + w - 14, y + (h - 7) / 2, COLOR_SELECT);
    });
}
} // namespace

void drawModeRow(int index, const char* label, const char* note, bool selected) {
    listRow(index, label, note, selected, [&](TFT_eSPI& g, int ix, int iy) {
        switch (index) {
            case 0: drawIcon(g, ICON_FILES, ix, iy, COLOR_SELECT, 2); break;
            case 1: drawIcon(g, ICON_LOOPER, ix, iy, COLOR_ACCENT, 2); break;
            case 2: drawIcon(g, ICON_PARISYNTH, ix, iy, COLOR_MIDI, 2); break;
            default:
                drawIcon(g, ICON_SLIDERS, ix, iy, COLOR_DIM, 2);
                drawIcon(g, ICON_KNOBS, ix, iy, COLOR_TEXT, 2);
                break;
        }
    });
}

void drawIconRow(int index, const Icon& icon, uint16_t iconColor, const char* label, const char* note, bool selected) {
    listRow(index, label, note, selected, [&](TFT_eSPI& g, int ix, int iy) {
        drawIcon(g, icon, ix + (24 - 2 * icon.w) / 2, iy + (24 - 2 * icon.h) / 2, iconColor, 2);
    });
}

void clearModeRow(int index) {
    screen().fillRect(MODE_X, MODE_Y + index * (MODE_ROW_H + MODE_GAP), MODE_W, MODE_ROW_H, COLOR_BG);
}

} // namespace Look

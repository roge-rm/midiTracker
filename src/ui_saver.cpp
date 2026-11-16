#include "ui_saver.h"
#include "ui_look.h"
#include "ui_strip.h"
#include "ui_tap.h"
#include "ui_player.h"
#include "synth.h"
#include <stdio.h>
#include <string.h>

using namespace Look;

namespace UiSaver {
namespace {

// Colours from the player screens.
const uint16_t UNLIT = rgb(0x1A212B);
const uint16_t EMPTY_CELL = rgb(0x3A4452);
const uint16_t INST_DIM = rgb(0x5A6575);

View g_view = VIEW_IDLE;
Source g_src = SRC_NONE;
int g_clearY = SCREEN_H; // the screen is cleared a strip at a time down to here
int g_sent = 0;          // strips drawn in this frame() call

// Stop drawing: the slice is spent, or the ring has run low after this
// call's first strip (the call itself is already thinned while it's low).
bool mustStop() { return !Ui::sliceLeft() || (g_sent > 0 && Ui::ringLow()); }

// ---- the drifting block (idle card, Now playing, Meters) ----------------

struct Drift {
    int x, y, w, h;
    int dx, dy;
    int px, py; // where it was last drawn
    bool drawn;
    int tick;
};
Drift g_blk;

void driftStart(int w, int h) {
    g_blk.w = w;
    g_blk.h = h;
    g_blk.x = (SCREEN_W - w) / 2;
    g_blk.y = (SCREEN_H - h) / 2;
    g_blk.dx = 1;
    g_blk.dy = 1;
    g_blk.drawn = false;
    g_blk.tick = 0;
}

// One pixel each way every `every` frames, bouncing off the edges.
void driftStep(int every) {
    if (++g_blk.tick < every) return;
    g_blk.tick = 0;
    if (g_blk.x + g_blk.dx < 0 || g_blk.x + g_blk.dx + g_blk.w > SCREEN_W) g_blk.dx = -g_blk.dx;
    if (g_blk.y + g_blk.dy < 0 || g_blk.y + g_blk.dy + g_blk.h > SCREEN_H) g_blk.dy = -g_blk.dy;
    g_blk.x += g_blk.dx;
    g_blk.y += g_blk.dy;
}

// Draws over where the block was and where it is now, so the edge it left
// is cleared in the same pass.
template <class F>
void driftDraw(F paint) {
    int x0 = g_blk.x, y0 = g_blk.y, x1 = g_blk.x + g_blk.w, y1 = g_blk.y + g_blk.h;
    if (g_blk.drawn) {
        if (g_blk.px < x0) x0 = g_blk.px;
        if (g_blk.py < y0) y0 = g_blk.py;
        if (g_blk.px + g_blk.w > x1) x1 = g_blk.px + g_blk.w;
        if (g_blk.py + g_blk.h > y1) y1 = g_blk.py + g_blk.h;
    }
    int x = g_blk.x, y = g_blk.y;
    band(x0, y0, x1 - x0, y1 - y0, COLOR_BG, [&](TFT_eSPI& g) { paint(g, x, y); });
    g_blk.px = x;
    g_blk.py = y;
    g_blk.drawn = true;
}

// ---- levels ---------------------------------------------------------------

uint8_t g_lv[2];          // L and R, 0-255, as shown
uint8_t g_chLv[16];       // MIDI channels, 0-255
uint8_t g_lastHits[16];   // UiTap::g_midi.hits when last looked at

// Peaks fall at most this much a frame.
const int FALL = 40;

void fallTo(uint8_t& shown, int v) {
    if (v > 255) v = 255;
    if (v >= shown) shown = (uint8_t)v;
    else shown = (uint8_t)(shown - v > FALL ? shown - FALL : v);
}

// L and R peaks of what's about to play, 0-255.
void ringLevels(const int16_t* f, int n) {
    int pl = 0, pr = 0;
    for (int i = 0; i < n; i++) {
        int l = f[i * 2], r = f[i * 2 + 1];
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    fallTo(g_lv[0], pl >> 7);
    fallTo(g_lv[1], pr >> 7);
}

// Channel levels from MIDI note-ons: a hit jumps to its velocity, then
// falls by about a quarter a frame. Both g_lv take the loudest channel.
void midiLevels() {
    int top = 0;
    for (int ch = 0; ch < 16; ch++) {
        int v = g_chLv[ch];
        v -= v / 4 + 6;
        if (v < 0) v = 0;
        if (UiTap::g_midi.hits[ch] != g_lastHits[ch]) {
            g_lastHits[ch] = UiTap::g_midi.hits[ch];
            int hit = UiTap::g_midi.vel[ch] * 2;
            if (hit > v) v = hit;
        }
        if (v > 255) v = 255;
        g_chLv[ch] = (uint8_t)v;
        if (v > top) top = v;
    }
    fallTo(g_lv[0], top);
    fallTo(g_lv[1], top);
}

void formatTime(uint32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    snprintf(out, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void paintBar(TFT_eSPI& g, int x, int y, int w, int h, int level) {
    int lit = level * w / 255;
    g.fillRect(x, y, w, h, UNLIT);
    if (lit > 0) g.fillRect(x, y, lit, h, COLOR_ACCENT);
}

// ---- idle card ------------------------------------------------------------

const char TITLE[] = "midiTracker";
const int IDLE_TITLE_SCALE = 2;
const int IDLE_H = 10 + 8 + 9;

void paintIdle(TFT_eSPI& g, int x, int y, const Info& info) {
    drawText(g, FONT_LABEL, TITLE, x, y, COLOR_TEXT, IDLE_TITLE_SCALE);
    int by = y + 18;
    uint16_t fill = info.charging ? COLOR_WAV : info.battery <= 15 ? COLOR_ERROR : COLOR_TEXT;
    int pct = info.battery < 0 ? 0 : info.battery > 100 ? 100 : info.battery;
    int fw = (8 * pct + 50) / 100;
    if (pct > 0 && fw == 0) fw = 1;
    g.drawRect(x, by, 12, 9, COLOR_DIM);
    g.fillRect(x + 12, by + 3, 2, 3, COLOR_DIM);
    if (fw) g.fillRect(x + 2, by + 2, fw, 5, fill);
    char t[8];
    snprintf(t, sizeof(t), "%d%%", pct);
    drawText(g, FONT_TEXT, t, x + 18, by + 1, info.battery <= 15 && !info.charging ? COLOR_ERROR : COLOR_DIM);
}

// ---- Now playing ----------------------------------------------------------

const int NP_W = 224;
const int NP_H = 52;
const int NP_METER_W = 64;

void paintNowPlaying(TFT_eSPI& g, int x, int y, const Info& info) {
    char name[48];
    fitText(FONT_TEXT, info.name ? info.name : "", NP_W / 2, name, sizeof(name));
    drawText(g, FONT_TEXT, name, x, y, COLOR_TEXT, 2);
    char tm[12];
    formatTime(info.elapsedMs, tm, sizeof(tm));
    drawText(g, FONT_TEXT, tm, x, y + 24, COLOR_ACCENT, 3);
    int mx = x + NP_W - NP_METER_W;
    paintBar(g, mx, y + 28, NP_METER_W, 6, g_lv[0]);
    paintBar(g, mx, y + 38, NP_METER_W, 6, g_lv[1]);
    g.fillRect(x, y + NP_H - 2, NP_W, 2, COLOR_RULE);
    if (info.progress > 0) {
        int p = info.progress > 1000 ? 1000 : info.progress;
        g.fillRect(x, y + NP_H - 2, NP_W * p / 1000, 2, COLOR_ACCENT);
    }
}

// ---- Meters ----------------------------------------------------------------

const int MT_W = 260;
const int MT_H = 104;
const int MT_SCOPE_H = 80;

// Scope of the next frames, scaled to the window's peak up to 8x, then L
// and R bars.
void paintRingMeters(TFT_eSPI& g, int x, int y, const int16_t* f, int n) {
    int mid = y + MT_SCOPE_H / 2;
    int half = MT_SCOPE_H / 2 - 1;
    g.drawFastHLine(x, mid, MT_W, UNLIT);
    if (n >= 2) {
        int peak = 4096;
        for (int i = 0; i < MT_W; i++) {
            int k = i * n / MT_W;
            int s = (f[k * 2] + f[k * 2 + 1]) / 2;
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        int prev = mid;
        for (int i = 0; i < MT_W; i++) {
            int k = i * n / MT_W;
            int s = (f[k * 2] + f[k * 2 + 1]) / 2;
            int dy = s * half / peak;
            int py = mid - dy;
            if (i == 0) prev = py;
            int a = prev < py ? prev : py, b = prev < py ? py : prev;
            g.drawFastVLine(x + i, a, b - a + 1, COLOR_ACCENT);
            prev = py;
        }
    }
    paintBar(g, x, y + MT_SCOPE_H + 8, MT_W, 6, g_lv[0]);
    paintBar(g, x, y + MT_SCOPE_H + 18, MT_W, 6, g_lv[1]);
}

// 16 channel bars, rising from the bottom.
void paintMidiMeters(TFT_eSPI& g, int x, int y) {
    const int step = MT_W / 16, bw = step - 4;
    for (int ch = 0; ch < 16; ch++) {
        int bx = x + ch * step + 2;
        int h = g_chLv[ch] * MT_H / 255;
        g.fillRect(bx, y + MT_H - 1, bw, 1, UNLIT);
        if (h > 0) g.fillRect(bx, y + MT_H - h, bw, h, COLOR_ACCENT);
    }
}

// ---- Pattern rain ------------------------------------------------------------
// 15 lines of 16 px. A new row overwrites the oldest line and becomes the
// amber bar; the line that was the bar goes back to dim. Nothing scrolls.

const int LINES = SCREEN_H / Ui::STRIP_H;
const int LINE_H = Ui::STRIP_H;
const int RAIN_X0 = 22;   // after the row number
const int MAX_COLS = 8;
const int32_t IDX_UNSET = -0x7FFFFFFF;

int g_bar = -1;            // line of the newest row
bool g_barAmber = false;   // g_bar still drawn as the bar
UiTap::Row g_barRow;       // its row, to draw it again dim
uint8_t g_barNotes[16];    // the same for MIDI
int32_t g_lastIdx = IDX_UNSET; // count index of the last row drawn
int g_cols = MAX_COLS;

void paintTrackerLine(int line, const UiTap::Row& r, bool bar) {
    int y = line * LINE_H;
    int colW = (SCREEN_W - RAIN_X0 - 2) / g_cols;
    if (colW > 60) colW = 60;
    uint16_t bg = bar ? COLOR_HILITE_BG : COLOR_BG;
    band(0, y, SCREEN_W, LINE_H, bg, [&](TFT_eSPI& g) {
        int ty = y + (LINE_H - 7) / 2;
        char t[8];
        snprintf(t, sizeof(t), "%02X", r.row & 0xFF);
        drawText(g, FONT_TEXT, t, 4, ty, bar ? COLOR_ACCENT : EMPTY_CELL);
        for (int c = 0; c < g_cols; c++) {
            int x = RAIN_X0 + c * colW + (colW - 30) / 2;
            uint8_t note = r.note[c];
            UiPlayer::formatNote(note, t);
            drawText(g, FONT_TEXT, t, x, ty, note == UiTap::NOTE_NONE ? EMPTY_CELL : bar ? COLOR_ACCENT : COLOR_DIM);
            if (r.inst[c]) snprintf(t, sizeof(t), "%02X", r.inst[c]);
            else strcpy(t, "\x7f\x7f");
            drawText(g, FONT_TEXT, t, x + 20, ty, !r.inst[c] ? EMPTY_CELL : bar ? COLOR_ACCENT : INST_DIM);
        }
    });
}

const char MIDI_NOTES[12][3] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

// 16 channels of 20 px, the note each one last played; 0xFF for none.
void paintMidiLine(int line, const uint8_t* notes, bool bar) {
    int y = line * LINE_H;
    uint16_t bg = bar ? COLOR_HILITE_BG : COLOR_BG;
    band(0, y, SCREEN_W, LINE_H, bg, [&](TFT_eSPI& g) {
        int ty = y + (LINE_H - 7) / 2;
        for (int ch = 0; ch < 16; ch++) {
            char t[4];
            uint8_t n = notes[ch];
            if (n < 128) {
                int oct = n / 12 - 1;
                t[0] = MIDI_NOTES[n % 12][0];
                t[1] = MIDI_NOTES[n % 12][1];
                t[2] = (char)('0' + (oct < 0 ? 0 : oct));
                t[3] = 0;
            } else {
                strcpy(t, "\x7f\x7f\x7f");
            }
            drawText(g, FONT_TEXT, t, ch * 20 + 3, ty, n >= 128 ? EMPTY_CELL : bar ? COLOR_ACCENT : COLOR_DIM);
        }
    });
}

bool rainTracker() {
    int32_t newest = UiTap::rowIndexAt(Synth::wavStreamPlayed());
    if (newest < 0) return false;
    // First frame, or a new song: start from the rows that fill the screen.
    if (g_lastIdx == IDX_UNSET || newest < g_lastIdx) g_lastIdx = newest - LINES;
    if (newest - g_lastIdx > LINES) g_lastIdx = newest - LINES;
    while (g_lastIdx < newest) {
        if (mustStop()) return true;
        int32_t i = g_lastIdx + 1;
        UiTap::Row r;
        if (i < 0 || !UiTap::rowByIndex((uint32_t)i, r)) {
            g_lastIdx = i;
            continue;
        }
        if (g_barAmber) {
            paintTrackerLine(g_bar, g_barRow, false);
            g_barAmber = false;
            g_sent++;
            if (mustStop()) return true;
        }
        g_bar = (g_bar + 1) % LINES;
        bool last = i == newest;
        paintTrackerLine(g_bar, r, last);
        g_sent++;
        if (last) {
            g_barRow = r;
            g_barAmber = true;
        }
        g_lastIdx = i;
    }
    return false;
}

// One line a frame at most, of the channels that played since the last.
void rainMidi() {
    uint8_t notes[16];
    bool any = false;
    for (int ch = 0; ch < 16; ch++) {
        notes[ch] = 0xFF;
        if (UiTap::g_midi.hits[ch] != g_lastHits[ch]) {
            g_lastHits[ch] = UiTap::g_midi.hits[ch];
            notes[ch] = UiTap::g_midi.note[ch];
            any = true;
        }
    }
    if (!any) return;
    if (g_barAmber) paintMidiLine(g_bar, g_barNotes, false);
    g_bar = (g_bar + 1) % LINES;
    paintMidiLine(g_bar, notes, true);
    memcpy(g_barNotes, notes, sizeof(g_barNotes));
    g_barAmber = true;
}

} // namespace

uint32_t frameMs(View v) {
    switch (v) {
        case VIEW_RAIN:        return g_src == SRC_TRACKER ? 200 : 250;
        case VIEW_METERS:      return 200;
        case VIEW_NOW_PLAYING: return 250;
        default:               return 500;
    }
}

void start(View v, const Info& info) {
    g_view = v;
    g_src = info.src;
    g_lv[0] = g_lv[1] = 0;
    memset(g_chLv, 0, sizeof(g_chLv));
    memcpy(g_lastHits, UiTap::g_midi.hits, sizeof(g_lastHits));
    g_bar = -1;
    g_barAmber = false;
    g_lastIdx = IDX_UNSET;
    g_cols = info.channels > 0 && info.channels < MAX_COLS ? info.channels : MAX_COLS;
    g_clearY = 0;
    switch (v) {
        case VIEW_IDLE:        driftStart(textWidth(FONT_LABEL, TITLE, IDLE_TITLE_SCALE), IDLE_H); break;
        case VIEW_NOW_PLAYING: driftStart(NP_W, NP_H); break;
        case VIEW_METERS:      driftStart(MT_W, MT_H); break;
        case VIEW_RAIN:        break;
    }
}

bool frame(const Info& info) {
    // Clearing what was on the screen comes first, inside the slice.
    g_sent = 0;
    while (g_clearY < SCREEN_H) {
        if (mustStop()) return true;
        band(0, g_clearY, SCREEN_W, LINE_H, COLOR_BG, [](TFT_eSPI&) {});
        g_clearY += LINE_H;
        g_sent++;
    }
    if (mustStop()) return true;
    switch (g_view) {
        case VIEW_IDLE:
            driftStep(1);
            driftDraw([&](TFT_eSPI& g, int x, int y) { paintIdle(g, x, y, info); });
            return false;
        case VIEW_NOW_PLAYING: {
            if (hasRing(g_src)) {
                const int16_t* f;
                int n = UiPlayer::peek(f);
                ringLevels(f, n);
            } else {
                midiLevels();
            }
            driftStep(2);
            driftDraw([&](TFT_eSPI& g, int x, int y) { paintNowPlaying(g, x, y, info); });
            return false;
        }
        case VIEW_METERS: {
            driftStep(2);
            if (hasRing(g_src)) {
                const int16_t* f;
                int n = UiPlayer::peek(f);
                ringLevels(f, n);
                driftDraw([&](TFT_eSPI& g, int x, int y) { paintRingMeters(g, x, y, f, n); });
            } else {
                midiLevels();
                driftDraw([&](TFT_eSPI& g, int x, int y) { paintMidiMeters(g, x, y); });
            }
            return false;
        }
        case VIEW_RAIN:
            if (g_src == SRC_TRACKER) return rainTracker();
            rainMidi();
            return false;
    }
    return false;
}

// ---- boot animation -----------------------------------------------------------

namespace {

const int BOOT_COLS = 16;
const int BOOT_COL_W = SCREEN_W / BOOT_COLS; // 20 x 240 fits in the strip
const int BOOT_PITCH = 12;                   // rain line height
const char BOOT_NAME[] = "MIDITRACKER";
const int BOOT_NAME_SCALE = 3;
const int BOOT_NAME_Y = 96;
const int BOOT_VER_Y = BOOT_NAME_Y + 5 * BOOT_NAME_SCALE + 9;
const int BOOT_VER_H = 12;
const uint32_t START_STEP_MS = 20;           // columns start 0-5 steps late
const uint32_t DROP_PX_S = 1600;             // the rain's front falls this fast
const uint32_t FADE_MS = 160;                // a column brightens from the background over this
const int FADE_STEPS = 4;
const uint32_t LOCK_START_MS = 260;          // column 0 locks
const uint32_t LOCK_STEP_MS = 20;            // then one more every step
const uint32_t FLASH_MS = 50;                // a locked column shows bright this long
const uint32_t VER_MS = LOCK_START_MS + (BOOT_COLS - 1) * LOCK_STEP_MS + FLASH_MS;
const uint32_t VER_FADE_MS = 30;             // version at half, then full this much later
const uint32_t HOLD_MS = 400;                // name and version hold this long at the end
const uint32_t DONE_MS = VER_MS + VER_FADE_MS + HOLD_MS;
const uint16_t FLASH = rgb(0xFFF0C8);

// Colours taken at bootStart(), so a theme loading part way through
// doesn't mix two palettes on the screen.
uint16_t g_bootBg, g_bootAccent, g_bootDim, g_bootText;
uint32_t g_bootT0;
const char* g_bootVersion = "";
uint8_t g_bootCol[BOOT_COLS]; // 0 raining, 1 drawn locked bright, 2 drawn locked
uint8_t g_bootVer = 2;        // version steps drawn, 0-2
bool g_bootDone = true;

uint32_t bootHash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

// c moved from the background towards itself, n of FADE_STEPS of the way.
uint16_t bootFade(uint16_t c, int n) {
    if (n >= FADE_STEPS) return c;
    int br = g_bootBg >> 11, bg = (g_bootBg >> 5) & 0x3F, bb = g_bootBg & 0x1F;
    int r = br + ((c >> 11) - br) * n / FADE_STEPS;
    int g = bg + (((c >> 5) & 0x3F) - bg) * n / FADE_STEPS;
    int b = bb + ((c & 0x1F) - bb) * n / FADE_STEPS;
    return (uint16_t)(r << 11 | g << 5 | b);
}

// A pattern cell from a hash: a note, an instrument, an effect or dots,
// in the player's colours, now and then a lit amber note.
uint16_t bootCell(uint32_t h, char* t) {
    switch ((h >> 8) % 10) {
        case 0: case 1: case 2: case 3:
            UiPlayer::formatNote((uint8_t)(25 + (h >> 16) % 60), t);
            return h % 19 == 0 ? g_bootAccent : g_bootDim;
        case 4: case 5:
            snprintf(t, 4, "%02X", (unsigned)((h >> 16) & 0x7F));
            return INST_DIM;
        case 6:
            snprintf(t, 4, "%c%02X", "0ACDEF"[(h >> 12) % 6], (unsigned)((h >> 20) & 0xFF));
            return INST_DIM;
        case 7:
            strcpy(t, "===");
            return g_bootDim;
        default:
            strcpy(t, "\x7f\x7f\x7f");
            return EMPTY_CELL;
    }
}

// Column k at t ms: lines of cells moving down at the column's own speed.
// The column starts a little late, then its front falls from the top with
// the lowest line lit, and it brightens from the background as it goes.
void paintBootRain(int k, uint32_t t) {
    int x = k * BOOT_COL_W;
    uint32_t start = bootHash(k, 3) % 6 * START_STEP_MS;
    uint32_t ct = t > start ? t - start : 0;
    int front = (int)(ct * DROP_PX_S / 1000);
    int fade = (int)(ct * FADE_STEPS / FADE_MS);
    uint32_t speed = 120 + bootHash(k, 1) % 180; // px a second
    uint32_t off = t * speed / 1000 + bootHash(k, 2) % (BOOT_PITCH * 20);
    int sub = (int)(off % BOOT_PITCH);
    int32_t first = (int32_t)(off / BOOT_PITCH);
    band(x, 0, BOOT_COL_W, SCREEN_H, g_bootBg, [&](TFT_eSPI& g) {
        for (int i = -1; i * BOOT_PITCH + sub < SCREEN_H; i++) {
            int y = i * BOOT_PITCH + sub;
            if (y + BOOT_PITCH > front) break;
            char t[8];
            uint16_t c = bootCell(bootHash(k + 100, (uint32_t)(i - first)), t);
            if (front < SCREEN_H && y + 2 * BOOT_PITCH > front) c = FLASH;
            drawText(g, FONT_TEXT, t, x + (BOOT_COL_W - textWidth(FONT_TEXT, t)) / 2, y + 2, bootFade(c, fade));
        }
    });
}

// Column k locked: blank, with its slice of the name.
void paintBootLocked(int k, uint16_t c) {
    int nameX = (SCREEN_W - textWidth(FONT_LABEL, BOOT_NAME, BOOT_NAME_SCALE)) / 2 + 1;
    band(k * BOOT_COL_W, 0, BOOT_COL_W, SCREEN_H, g_bootBg, [&](TFT_eSPI& g) {
        drawText(g, FONT_LABEL, BOOT_NAME, nameX, BOOT_NAME_Y, c, BOOT_NAME_SCALE);
    });
}

void paintBootVersion(uint16_t c) {
    char ver[24];
    snprintf(ver, sizeof(ver), "v%s", g_bootVersion);
    band(0, BOOT_VER_Y, SCREEN_W, BOOT_VER_H, g_bootBg, [&](TFT_eSPI& g) {
        drawText(g, FONT_TEXT, ver, (SCREEN_W - textWidth(FONT_TEXT, ver)) / 2, BOOT_VER_Y + 1, c);
    });
}

void bootDraw(uint32_t t) {
    for (int k = 0; k < BOOT_COLS; k++) {
        uint32_t lockAt = LOCK_START_MS + k * LOCK_STEP_MS;
        if (t < lockAt) {
            paintBootRain(k, t);
        } else if (t < lockAt + FLASH_MS) {
            if (g_bootCol[k] < 1) paintBootLocked(k, FLASH);
            g_bootCol[k] = 1;
        } else if (g_bootCol[k] < 2) {
            paintBootLocked(k, g_bootAccent);
            g_bootCol[k] = 2;
        }
    }
    if (t >= VER_MS + VER_FADE_MS && g_bootVer < 2) {
        paintBootVersion(g_bootText);
        g_bootVer = 2;
    } else if (t >= VER_MS && g_bootVer < 1) {
        paintBootVersion(bootFade(g_bootText, FADE_STEPS / 2));
        g_bootVer = 1;
    }
    if (t >= DONE_MS) g_bootDone = true;
}

} // namespace

void bootStart(const char* version, uint32_t nowMs) {
    hideStatus();
    g_bootBg = COLOR_BG;
    g_bootAccent = COLOR_ACCENT;
    g_bootDim = COLOR_DIM;
    g_bootText = COLOR_TEXT;
    g_bootVersion = version ? version : "";
    g_bootT0 = nowMs;
    memset(g_bootCol, 0, sizeof(g_bootCol));
    g_bootVer = 0;
    g_bootDone = false;
    bootDraw(0);
}

bool bootFrame(uint32_t nowMs) {
    if (g_bootDone) return false;
    bootDraw(nowMs - g_bootT0);
    return !g_bootDone;
}

void bootSkip() {
    if (!g_bootDone) bootDraw(DONE_MS);
}

bool bootDone() { return g_bootDone; }


} // namespace UiSaver

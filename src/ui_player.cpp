#include "ui_player.h"
#include "ui_look.h"
#include "ui_strip.h"
#include "ui_tap.h"
#include "synth.h"
#include "midi_output.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace Look;

namespace UiPlayer {
namespace {

// ---- layout, 320x240 ------------------------------------------------------
const int X0 = 6; // same as the status strip's padding
const int W = SCREEN_W - 2 * X0;
const int BODY_BOTTOM = SCREEN_H - FOOTER_H;

const int TITLE_Y = 21;  // name at 2x, caps 14 px
const int TOP_Y = 40;    // big time, scope and the side column
const int TOP_H = 28;
const int TIME_W = 104;
const int SIDE_W = 52;   // SPD / BPM / VOL
const int SIDE_X = X0 + W - SIDE_W;
const int SIDE_ROW = 10;

// Tracker
const int SCOPE_X = X0 + TIME_W + 2;
const int SCOPE_W = SIDE_X - 8 - SCOPE_X;
const int LANE_HDR_Y = 73;  // rule, labels, rule
const int LANE_HDR_H = 13;
const int TRAIL_Y = LANE_HDR_Y + LANE_HDR_H + 1;
const int TRAIL_ROWS = 3;
const int TRAIL_ROW_H = 14;
const int METER_Y = TRAIL_Y + TRAIL_ROWS * TRAIL_ROW_H + 4;
const int METER_SEGS = 12;
const int SEG_STEP = 4;     // 3 px lit, 1 px gap
const int ORD_Y = METER_Y + METER_SEGS * SEG_STEP + 6;
const int ORD_H = 10;
const int POS_W = 70;       // "07/32 ROW 2E"
const int ORD_BAR_X = X0 + 20;
const int ORD_BAR_W = X0 + W - POS_W - 6 - ORD_BAR_X;
const int MAX_LANES = UiTap::LANES;

// MIDI
const int MIDI_INFO_Y = 74;
const int MIDI_RULE_Y = 86;
const int CH_METER_Y = 92;
const int CH_SEGS = 8;
const int CH_LABEL_Y = CH_METER_Y + CH_SEGS * SEG_STEP + 2;
const int KEYS_Y = 142;
const int KEYS_H = 50;
// Where the keybed is now: the .mid screen's, or pariSynth's (keybedDraw()).
int g_keysY = KEYS_Y, g_keysH = KEYS_H, g_blackH = 30;
const int WHITE_W = 6;
const int KEYS_X = (SCREEN_W - 52 * WHITE_W) / 2;
const int PIANO_LO = 21, PIANO_HI = 108;
const int PIANO_KEYS = PIANO_HI - PIANO_LO + 1;

// WAV
const int WAVE_Y = 74;
const int WAVE_H = 76;
const int WAVE_COLS = W - 2;
const int PEAK_Y = WAVE_Y + WAVE_H + 8;
const int PEAK_X = X0 + 10;
const int PEAK_SEGS = 40;
const int PEAK_SEG_W = 7;   // 6 px lit, 1 px gap
const int PEAK_H = 6;
const int PEAK_ROW = 11;

// Draw rates.
const uint32_t METER_MS = 40; // 25 fps
const uint32_t SCOPE_MS = 80; // 12 fps
const uint32_t TRAIL_MS = 40;

// Colours from the mockups that aren't theme entries.
const uint16_t UNLIT = rgb(0x1A212B);
const uint16_t EMPTY_CELL = rgb(0x3A4452);
const uint16_t SCOPE_BG = rgb(0x0D1219);
const uint16_t SCOPE_MID = rgb(0x1C2430);
const uint16_t KEY_WHITE = rgb(0xD6DAE0);
const uint16_t LABEL_OFF = rgb(0x5A6575);

enum Screen : uint8_t { SCREEN_NONE, SCREEN_TRACKER, SCREEN_MIDI, SCREEN_WAV };
Screen g_screen = SCREEN_NONE;

int g_chipX = 0;
PlayState g_state = PS_IDLE;
int g_volume = -1;
char g_time[12];
uint32_t g_lastMeterMs = 0, g_lastScopeMs = 0, g_lastTrailMs = 0;

// What's about to play, for the scope, waveform and peak meters.
const int PEEK_FRAMES = 512;
int16_t g_peek[PEEK_FRAMES * 2];

// ---- small helpers ---------------------------------------------------------

void formatTime(uint32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    snprintf(out, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// Cyan to 60%, amber to 80%, red above, as the mockups.
uint16_t meterColor(int seg, int segs) {
    if (seg * 10 < segs * 6) return COLOR_SELECT;
    if (seg * 10 < segs * 8) return COLOR_ACCENT;
    return COLOR_ERROR;
}

void drawTextRight(TFT_eSPI& g, const PixFont& f, const char* s, int xr, int y, uint16_t c, int scale = 1) {
    drawText(g, f, s, xr - textWidth(f, s, scale), y, c, scale);
}

const char NOTE_NAMES[12][3] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

void noteText(uint8_t n, char* out) {
    if (n >= 1 && n <= 120) {
        out[0] = NOTE_NAMES[(n - 1) % 12][0];
        out[1] = NOTE_NAMES[(n - 1) % 12][1];
        out[2] = (char)('0' + (n - 1) / 12);
        out[3] = 0;
    } else if (n == UiTap::NOTE_OFF) {
        strcpy(out, "===");
    } else if (n == UiTap::NOTE_CUT) {
        strcpy(out, "^^^");
    } else if (n == UiTap::NOTE_FADE) {
        strcpy(out, "~~~");
    } else {
        strcpy(out, "\x7f\x7f\x7f");
    }
}

// Segments lit for a level, by thresholds 3 dB apart (made once, at the
// first screen draw, so the hot paths stay integer).
uint16_t g_laneThr[METER_SEGS];
uint16_t g_peakThr[PEAK_SEGS];
bool g_thrMade = false;

void makeThresholds() {
    if (g_thrMade) return;
    // Lane levels: the top segment at 200 of 255, 3 dB a segment.
    for (int k = 0; k < METER_SEGS; k++) {
        g_laneThr[k] = (uint16_t)(200.0f * powf(10.0f, -3.0f * (METER_SEGS - 1 - k) / 20.0f) + 0.5f);
        if (g_laneThr[k] < 1) g_laneThr[k] = 1;
    }
    // Output peaks: -39 to 0 dBFS, 1 dB a segment.
    for (int k = 0; k < PEAK_SEGS; k++) {
        g_peakThr[k] = (uint16_t)(32767.0f * powf(10.0f, -1.0f * (PEAK_SEGS - 1 - k) / 20.0f) + 0.5f);
    }
    g_thrMade = true;
}

int segsFor(uint32_t v, const uint16_t* thr, int n) {
    int s = 0;
    while (s < n && v >= thr[s]) s++;
    return s;
}

// ---- shared pieces ---------------------------------------------------------

void drawStatusFor(const Icon& icon, uint16_t iconColor, const char* title) {
    Status st;
    st.icon = &icon;
    st.iconColor = iconColor;
    st.title = title;
    drawStatus(st);
    g_chipX = X0 + icon.w + 4 + textWidth(FONT_LABEL, title) + 4 + 6;
}

const int CHIP_W = 46;

// Play state after the title in the status strip.
void paintChip(TFT_eSPI& g, PlayState s) {
    int x = g_chipX, y = 5;
    uint16_t c = COLOR_DIM;
    const char* label = "";
    switch (s) {
        case PS_PLAYING:
            c = COLOR_ACCENT;
            drawIcon(g, ICON_PLAY, x, 4, c);
            label = "PLAY";
            break;
        case PS_PAUSED:
            g.fillRect(x + 1, 4, 2, 7, c);
            g.fillRect(x + 5, 4, 2, 7, c);
            label = "PAUSE";
            break;
        case PS_STOPPED:
            g.fillRect(x + 1, 5, 6, 6, c);
            label = "STOP";
            break;
        case PS_DONE:
            label = "END";
            break;
        case PS_ERROR:
            c = COLOR_ERROR;
            label = "ERROR";
            break;
        default:
            break;
    }
    drawText(g, FONT_LABEL, label, x + 10, y, c);
}

void drawChip(PlayState s) {
    band(g_chipX, 0, CHIP_W, STATUS_H, COLOR_SURFACE, [&](TFT_eSPI& g) { paintChip(g, s); });
}

// Name at 2x on the left, an info line in Silkscreen on the right.
void paintTitle(TFT_eSPI& g, const char* name, const char* info) {
    int infoW = info ? textWidth(FONT_LABEL, info) : 0;
    if (info) drawText(g, FONT_LABEL, info, X0 + W - infoW, TITLE_Y + 7, COLOR_DIM);
    char buf[64];
    fitText(FONT_TEXT, name ? name : "", (W - infoW - 10) / 2, buf, sizeof(buf));
    drawText(g, FONT_TEXT, buf, X0, TITLE_Y, COLOR_TEXT, 2);
}

void paintTime(TFT_eSPI& g, const char* t) {
    drawText(g, FONT_TEXT, t, X0, TOP_Y, COLOR_ACCENT, 4);
}

// Puts the footer up for the screen, PLAY's action from the state.
void drawFooterFor(Screen s, PlayState st) {
    const char* play = st == PS_PLAYING ? "pause" : "play";
    if (s == SCREEN_MIDI) {
        Key r1[] = {{"UD", "tempo", TONE_TEXT}, {"PLAY", play, TONE_PLAY}, {"EDIT", "inst", TONE_TEXT},
                    {"ALT", "out", TONE_TEXT}, {"NAV", "stop", TONE_TEXT}};
        Key r2[] = {{"LEFT", "back", TONE_TEXT}, {"EDIT+UD", "vol", TONE_TEXT}, {"ENT+LR", "seek", TONE_TEXT},
                    {"ALT+EDIT", "synth", TONE_TEXT}};
        drawKeyFooter(r1, 5, r2, 4);
    } else if (s == SCREEN_WAV) {
        Key r1[] = {{"PLAY", play, TONE_PLAY}, {"EDIT+UD", "vol", TONE_TEXT}, {"NAV", "stop", TONE_TEXT}};
        Key r2[] = {{"LEFT", "back", TONE_TEXT}, {"ENT+LR", "seek", TONE_TEXT}};
        drawKeyFooter(r1, 3, r2, 2);
    } else {
        Key r1[] = {{"PLAY", play, TONE_PLAY}, {"EDIT+UD", "vol", TONE_TEXT}, {"NAV", "stop", TONE_TEXT}};
        Key r2[] = {{"LEFT", "back", TONE_TEXT}};
        drawKeyFooter(r1, 3, r2, 1);
    }
}

// A state change: the chip, and the footer when PLAY's action flips.
void changeState(Screen s, PlayState st) {
    if (g_screen != s) return;
    bool playFlip = (st == PS_PLAYING) != (g_state == PS_PLAYING);
    g_state = st;
    drawChip(st);
    if (playFlip) drawFooterFor(s, st);
}

void paintError(TFT_eSPI& g, const char* error) {
    drawText(g, FONT_LABEL, "CAN'T PLAY THIS FILE", X0, TOP_Y + 4, COLOR_ERROR);
    char buf[80];
    fitText(FONT_TEXT, error ? error : "", W, buf, sizeof(buf));
    drawText(g, FONT_TEXT, buf, X0, TOP_Y + 16, COLOR_TEXT);
}

// The body under the title for an error, then the footer.
void drawErrorBody(Screen s, const char* error) {
    band(0, TOP_Y - 2, SCREEN_W, BODY_BOTTOM - (TOP_Y - 2), COLOR_BG, [&](TFT_eSPI& g) { paintError(g, error); });
    drawFooterFor(s, PS_ERROR);
}

// The side column: up to three label and value rows.
struct SideRow { const char* label; char value[8]; };

void paintSide(TFT_eSPI& g, const SideRow* rows, int n) {
    for (int i = 0; i < n; i++) {
        int y = TOP_Y + i * SIDE_ROW;
        drawText(g, FONT_LABEL, rows[i].label, SIDE_X, y + 2, COLOR_DIM);
        drawTextRight(g, FONT_TEXT, rows[i].value, X0 + W, y + 1, COLOR_TEXT);
    }
}

// ---- tracker ---------------------------------------------------------------

struct TrackerView {
    int lanes, laneW;
    int channels, songLength;
    int speed, tempo;
    int order, row;          // shown position
    int beat;                // lit beat square
    uint8_t segs[MAX_LANES]; // meter segments shown
    uint32_t trailAt;        // newest trail row drawn
    bool trailDrawn;
};
TrackerView g_trk;

int laneX(int i) { return X0 + i * g_trk.laneW + (W - g_trk.lanes * g_trk.laneW) / 2; }

void trackerSide(SideRow* rows) {
    rows[0].label = "SPD";
    snprintf(rows[0].value, sizeof(rows[0].value), "%d", g_trk.speed);
    rows[1].label = "BPM";
    snprintf(rows[1].value, sizeof(rows[1].value), "%d", g_trk.tempo);
    rows[2].label = "VOL";
    snprintf(rows[2].value, sizeof(rows[2].value), "%d", g_volume);
}

void drawTrackerSide() {
    SideRow rows[3];
    trackerSide(rows);
    band(SIDE_X, TOP_Y, SIDE_W, TOP_H, COLOR_BG, [&](TFT_eSPI& g) { paintSide(g, rows, 3); });
}

// Scope box with the next frames to play, mixed to mono.
void paintScope(TFT_eSPI& g, int frames) {
    int x = SCOPE_X, y = TOP_Y, w = SCOPE_W, h = TOP_H;
    g.fillRect(x, y, w, h, SCOPE_BG);
    g.drawRect(x, y, w, h, COLOR_RULE);
    int mid = y + h / 2;
    int half = h / 2 - 2;
    int inner = w - 2;
    g.drawFastHLine(x + 1, mid, inner, SCOPE_MID);
    if (frames < 2) return;
    // Scaled to the window's peak, up to 8x, so quiet parts still show.
    int peak = 4096;
    for (int i = 0; i < inner; i++) {
        int k = i * frames / inner;
        int s = (g_peek[k * 2] + g_peek[k * 2 + 1]) / 2;
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    int prev = 0;
    for (int i = 0; i < inner; i++) {
        int k = i * frames / inner;
        int s = (g_peek[k * 2] + g_peek[k * 2 + 1]) / 2;
        int dy = s * half / peak;
        if (dy > half) dy = half;
        if (dy < -half) dy = -half;
        int py = mid - dy;
        if (i == 0) prev = py;
        int a = prev < py ? prev : py, b = prev < py ? py : prev;
        g.drawFastVLine(x + 1 + i, a, b - a + 1, COLOR_SELECT);
        prev = py;
    }
}

void drawScope(bool live) {
    int n = live ? (int)Synth::wavStreamPeek(g_peek, PEEK_FRAMES) : 0;
    band(SCOPE_X, TOP_Y, SCOPE_W, TOP_H, SCOPE_BG, [&](TFT_eSPI& g) { paintScope(g, n); });
}

void paintLaneHeader(TFT_eSPI& g) {
    g.drawFastHLine(0, LANE_HDR_Y, SCREEN_W, COLOR_RULE);
    g.drawFastHLine(0, LANE_HDR_Y + LANE_HDR_H - 1, SCREEN_W, COLOR_RULE);
    for (int i = 0; i < g_trk.lanes; i++) {
        char n[12];
        snprintf(n, sizeof(n), "%02d", i + 1);
        int tw = textWidth(FONT_LABEL, n);
        drawText(g, FONT_LABEL, n, laneX(i) + (g_trk.laneW - tw) / 2, LANE_HDR_Y + 4, COLOR_DIM);
    }
}

// The trail: older rows dim, the newest at the bottom in amber on a band.
void paintTrail(TFT_eSPI& g, const UiTap::Row* rows, int n) {
    for (int r = 0; r < n; r++) {
        int slot = TRAIL_ROWS - n + r;
        int y = TRAIL_Y + slot * TRAIL_ROW_H;
        bool current = r == n - 1;
        if (current) g.fillRect(0, y, SCREEN_W, TRAIL_ROW_H, COLOR_HILITE_BG);
        for (int i = 0; i < g_trk.lanes; i++) {
            char t[4];
            uint8_t note = rows[r].note[i];
            noteText(note, t);
            uint16_t c = note == UiTap::NOTE_NONE ? EMPTY_CELL : current ? COLOR_ACCENT : COLOR_DIM;
            int tw = textWidth(FONT_TEXT, t);
            drawText(g, FONT_TEXT, t, laneX(i) + (g_trk.laneW - tw) / 2, y + 3, c);
        }
    }
}

void drawTrail(const UiTap::Row* rows, int n) {
    band(0, TRAIL_Y, SCREEN_W, TRAIL_ROWS * TRAIL_ROW_H, COLOR_BG, [&](TFT_eSPI& g) { paintTrail(g, rows, n); });
}

int laneBarW() {
    int w = g_trk.laneW / 2;
    if (w < 3) w = 3;
    if (w > 24) w = 24;
    return w;
}

void paintLaneSeg(TFT_eSPI& g, int lane, int seg, bool lit) {
    int bw = laneBarW();
    int x = laneX(lane) + (g_trk.laneW - bw) / 2;
    int y = METER_Y + (METER_SEGS - 1 - seg) * SEG_STEP;
    g.fillRect(x, y, bw, SEG_STEP - 1, lit ? meterColor(seg, METER_SEGS) : UNLIT);
}

void paintMeters(TFT_eSPI& g) {
    for (int i = 0; i < g_trk.lanes; i++)
        for (int s = 0; s < METER_SEGS; s++) paintLaneSeg(g, i, s, s < g_trk.segs[i]);
}

// Order cells: played cyan, current amber, the rest unlit. Long songs get
// a plain bar.
void paintOrder(TFT_eSPI& g) {
    drawText(g, FONT_LABEL, "ORD", X0, ORD_Y + 2, COLOR_DIM);
    int n = g_trk.songLength > 0 ? g_trk.songLength : 1;
    int y = ORD_Y + 2, h = 5;
    if (ORD_BAR_W / n >= 3) {
        for (int i = 0; i < n; i++) {
            int a = ORD_BAR_X + i * ORD_BAR_W / n, b = ORD_BAR_X + (i + 1) * ORD_BAR_W / n - 1;
            uint16_t c = i < g_trk.order ? COLOR_SELECT : i == g_trk.order ? COLOR_ACCENT : UNLIT;
            g.fillRect(a, y, b - a, h, c);
        }
    } else {
        int at = ORD_BAR_X + g_trk.order * ORD_BAR_W / n;
        g.fillRect(ORD_BAR_X, y, ORD_BAR_W, h, UNLIT);
        g.fillRect(ORD_BAR_X, y, at - ORD_BAR_X, h, COLOR_SELECT);
        g.fillRect(at, y, 2, h, COLOR_ACCENT);
    }
}

void paintPos(TFT_eSPI& g) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d/%02d", g_trk.order + 1, g_trk.songLength);
    int x = X0 + W - POS_W;
    x = drawText(g, FONT_TEXT, buf, x, ORD_Y + 1, COLOR_TEXT) + 4;
    x = drawText(g, FONT_LABEL, "ROW", x, ORD_Y + 2, COLOR_DIM) + 3;
    snprintf(buf, sizeof(buf), "%02X", g_trk.row & 0xFF);
    drawText(g, FONT_TEXT, buf, x, ORD_Y + 1, COLOR_ACCENT);
}

// Four squares after the chip, one lit per four rows.
void paintBeat(TFT_eSPI& g) {
    for (int i = 0; i < 4; i++)
        g.fillRect(g_chipX + CHIP_W + i * 6, 6, 4, 4, i == g_trk.beat ? COLOR_ACCENT : COLOR_RULE);
}

void drawBeat() {
    band(g_chipX + CHIP_W, 0, 24, STATUS_H, COLOR_SURFACE, [&](TFT_eSPI& g) { paintBeat(g); });
}

void setPosition(int order, int row, bool draw) {
    bool orderChanged = order != g_trk.order;
    bool rowChanged = row != g_trk.row;
    int beat = (row / 4) & 3;
    bool beatChanged = beat != g_trk.beat;
    g_trk.order = order;
    g_trk.row = row;
    g_trk.beat = beat;
    if (!draw) return;
    if (orderChanged) band(X0, ORD_Y, X0 + W - POS_W - X0 - 2, ORD_H, COLOR_BG, [&](TFT_eSPI& g) { paintOrder(g); });
    if (orderChanged || rowChanged)
        band(X0 + W - POS_W, ORD_Y, POS_W, ORD_H, COLOR_BG, [&](TFT_eSPI& g) { paintPos(g); });
    if (beatChanged && g_state == PS_PLAYING) drawBeat();
}

// True once everything written to the ring has been heard.
bool drained() { return (int32_t)(Synth::wavStreamPlayed() - Synth::wavStreamWritten()) >= 0; }

// quiet: the song has ended and played out, so the meters fall to zero.
void updateLaneMeters(uint32_t played, bool quiet) {
    UiTap::Levels lv;
    bool have = !quiet && UiTap::levelsAt(played, lv);
    TFT_eSPI& g = screen();
    for (int i = 0; i < g_trk.lanes; i++) {
        int target = have ? segsFor(lv.level[i], g_laneThr, METER_SEGS) : 0;
        int old = g_trk.segs[i];
        int now = target >= old ? target : old - 1; // falls a segment a frame
        if (now == old) continue;
        int lo = now < old ? now : old, hi = now < old ? old : now;
        for (int s = lo; s < hi; s++) paintLaneSeg(g, i, s, s < now);
        g_trk.segs[i] = (uint8_t)now;
    }
}

// ---- MIDI --------------------------------------------------------------------

struct MidiView {
    uint8_t hits[16];
    uint8_t level[16]; // 0-127
    uint8_t segs[16];
    bool active[16];
    uint8_t key[PIANO_KEYS]; // 0 off, 1 lit, 2 drums
    unsigned bpm;
    int tempoPct;
    char out[12];
    bool audioOn;
    int format, tracks;
};
MidiView g_midi;

// Key layout: white index for white keys, the white key to the left for black.
bool isBlack(int note) {
    int p = note % 12;
    return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
}
int whiteIndex(int note) {
    static const uint8_t W_IN_OCT[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    int oct = note / 12, p = note % 12;
    // A0 (21) is white key 0.
    return oct * 7 + W_IN_OCT[p] - (1 * 7 + 5);
}

uint16_t keyColor(int note, uint8_t state) {
    if (state == 2) return COLOR_MIDI;
    if (state == 1) return COLOR_ACCENT;
    return isBlack(note) ? COLOR_BG : KEY_WHITE;
}

void paintWhite(TFT_eSPI& g, int note) {
    int x = KEYS_X + whiteIndex(note) * WHITE_W;
    g.fillRect(x, g_keysY, WHITE_W - 1, g_keysH, keyColor(note, g_midi.key[note - PIANO_LO]));
}
void paintBlack(TFT_eSPI& g, int note) {
    int x = KEYS_X + (whiteIndex(note) + 1) * WHITE_W - 3;
    g.fillRect(x, g_keysY, 4, g_blackH, keyColor(note, g_midi.key[note - PIANO_LO]));
}

void paintKeys(TFT_eSPI& g) {
    g.drawFastHLine(0, g_keysY - 2, SCREEN_W, COLOR_RULE);
    g.drawFastHLine(0, g_keysY - 1, SCREEN_W, COLOR_RULE);
    for (int n = PIANO_LO; n <= PIANO_HI; n++) if (!isBlack(n)) paintWhite(g, n);
    for (int n = PIANO_LO; n <= PIANO_HI; n++) if (isBlack(n)) paintBlack(g, n);
}

uint8_t keyState(int note) {
    if (!MidiOutput::isNoteActive((uint8_t)note)) return 0;
    return MidiOutput::isNotePercussion((uint8_t)note) ? 2 : 1;
}

// Redraws only the keys that changed. A white key repaints the black keys
// that sit over it.
void updateKeys() {
    TFT_eSPI& g = screen();
    for (int n = PIANO_LO; n <= PIANO_HI; n++) {
        uint8_t s = keyState(n);
        if (s == g_midi.key[n - PIANO_LO]) continue;
        g_midi.key[n - PIANO_LO] = s;
        if (isBlack(n)) {
            paintBlack(g, n);
        } else {
            paintWhite(g, n);
            if (n > PIANO_LO && isBlack(n - 1)) paintBlack(g, n - 1);
            if (n < PIANO_HI && isBlack(n + 1)) paintBlack(g, n + 1);
        }
    }
}

int chColW() { return W / 16; }
int chX(int ch) { return X0 + ch * chColW() + (W - 16 * chColW()) / 2; }

uint16_t chSegColor(int ch, int seg) {
    return ch == 9 ? COLOR_MIDI : meterColor(seg, CH_SEGS);
}

void paintChSeg(TFT_eSPI& g, int ch, int seg, bool lit) {
    int y = CH_METER_Y + (CH_SEGS - 1 - seg) * SEG_STEP;
    g.fillRect(chX(ch) + 1, y, chColW() - 3, SEG_STEP - 1, lit ? chSegColor(ch, seg) : UNLIT);
}

void paintChLabel(TFT_eSPI& g, int ch) {
    char n[12];
    snprintf(n, sizeof(n), "%d", ch + 1);
    int tw = textWidth(FONT_LABEL, n);
    drawText(g, FONT_LABEL, n, chX(ch) + 1 + (chColW() - 3 - tw) / 2, CH_LABEL_Y, g_midi.active[ch] ? COLOR_TEXT : LABEL_OFF);
}

void paintChannels(TFT_eSPI& g) {
    for (int ch = 0; ch < 16; ch++) {
        for (int s = 0; s < CH_SEGS; s++) paintChSeg(g, ch, s, s < g_midi.segs[ch]);
        paintChLabel(g, ch);
    }
}

// A note-on jumps a channel's bar to its velocity, then it falls, holding
// at half while notes are down.
void updateChannels() {
    uint16_t mask = MidiOutput::activeChannelMask();
    TFT_eSPI& g = screen();
    for (int ch = 0; ch < 16; ch++) {
        bool active = (mask >> ch) & 1;
        int lvl = g_midi.level[ch];
        if (UiTap::g_midi.hits[ch] != g_midi.hits[ch]) {
            g_midi.hits[ch] = UiTap::g_midi.hits[ch];
            lvl = UiTap::g_midi.vel[ch];
        } else {
            int floor = active ? UiTap::g_midi.vel[ch] / 2 : 0;
            lvl -= 8;
            if (lvl < floor) lvl = floor;
            if (lvl < 0) lvl = 0;
        }
        g_midi.level[ch] = (uint8_t)lvl;
        int segs = (lvl * CH_SEGS + 126) / 127;
        int old = g_midi.segs[ch];
        if (segs != old) {
            int lo = segs < old ? segs : old, hi = segs < old ? old : segs;
            for (int s = lo; s < hi; s++) paintChSeg(g, ch, s, s < segs);
            g_midi.segs[ch] = (uint8_t)segs;
        }
        if (active != g_midi.active[ch]) {
            g_midi.active[ch] = active;
            band(chX(ch), CH_LABEL_Y, chColW(), 6, COLOR_BG, [&](TFT_eSPI& gg) { paintChLabel(gg, ch); });
        }
    }
}

void paintMidiTop(TFT_eSPI& g) {
    // BPM after the time, the tempo box and volume on the right.
    char buf[24];
    snprintf(buf, sizeof(buf), "%u BPM", g_midi.bpm);
    drawText(g, FONT_TEXT, buf, X0 + TIME_W + 6, TOP_Y + TOP_H - 14, COLOR_DIM, 2);
    snprintf(buf, sizeof(buf), "TEMPO %d%%", g_midi.tempoPct);
    int bw = textWidth(FONT_TEXT, buf) + 8;
    int bx = X0 + W - bw;
    uint16_t c = g_midi.tempoPct != 100 ? COLOR_SELECT : COLOR_DIM;
    g.drawRect(bx, TOP_Y, bw, 13, c);
    drawText(g, FONT_TEXT, buf, bx + 4, TOP_Y + 3, c);
    snprintf(buf, sizeof(buf), "VOL %d", g_volume);
    drawTextRight(g, FONT_LABEL, buf, X0 + W, TOP_Y + 18, COLOR_DIM);
}

void drawMidiTopRight() {
    int x = X0 + TIME_W + 2;
    band(x, TOP_Y, X0 + W - x, TOP_H, COLOR_BG, [&](TFT_eSPI& g) { paintMidiTop(g); });
}

void paintMidiInfo(TFT_eSPI& g) {
    int x = X0;
    x = drawText(g, FONT_LABEL, "OUT", x, MIDI_INFO_Y + 2, COLOR_DIM) + 4;
    x = drawText(g, FONT_LABEL, g_midi.out, x, MIDI_INFO_Y + 2, COLOR_SELECT) + 10;
    x = drawText(g, FONT_LABEL, "SYNTH", x, MIDI_INFO_Y + 2, COLOR_DIM) + 4;
    drawText(g, FONT_LABEL, g_midi.audioOn ? "ON" : "OFF", x, MIDI_INFO_Y + 2, g_midi.audioOn ? COLOR_SELECT : COLOR_DIM);
    char buf[24];
    snprintf(buf, sizeof(buf), "FMT %d %s %d TRK", g_midi.format, MIDDOT, g_midi.tracks);
    drawTextRight(g, FONT_LABEL, buf, X0 + W, MIDI_INFO_Y + 2, COLOR_DIM);
    g.drawFastHLine(X0, MIDI_RULE_Y, W, COLOR_RULE);
}

void drawMidiInfo() {
    band(0, MIDI_INFO_Y, SCREEN_W, MIDI_RULE_Y + 1 - MIDI_INFO_Y, COLOR_BG, [&](TFT_eSPI& g) { paintMidiInfo(g); });
}

// ---- WAV ---------------------------------------------------------------------

struct WavView {
    uint8_t col[WAVE_COLS]; // peak half-height per column, as played
    int head;               // playhead column, -1 = none
    uint8_t segs[2];
    uint32_t totalMs;
    uint32_t key;           // which file the columns belong to
};
WavView g_wav;

// The file and its length, so a file of the same name elsewhere, or one
// changed over USB, starts a fresh waveform.
uint32_t waveKey(uint32_t fileKey, uint32_t total) { return (fileKey ^ total) * 16777619u; }

const int WAVE_HALF = WAVE_H / 2 - 2;

void paintWaveCol(TFT_eSPI& g, int i) {
    int x = X0 + 1 + i, top = WAVE_Y + 1, mid = WAVE_Y + WAVE_H / 2;
    g.fillRect(x, top, 1, WAVE_H - 2, SCOPE_BG);
    if (i == g_wav.head) {
        g.fillRect(x, top, 1, WAVE_H - 2, COLOR_ACCENT);
        return;
    }
    int a = g_wav.col[i];
    if (a) g.fillRect(x, mid - a, 1, 2 * a + 1, COLOR_WAV);
    else g.drawPixel(x, mid, SCOPE_MID);
}

void paintWave(TFT_eSPI& g) {
    g.drawRect(X0, WAVE_Y, W, WAVE_H, COLOR_RULE);
    for (int i = 0; i < WAVE_COLS; i++) paintWaveCol(g, i);
}

void paintPeakSeg(TFT_eSPI& g, int ch, int seg, bool lit) {
    int x = PEAK_X + seg * PEAK_SEG_W, y = PEAK_Y + ch * PEAK_ROW;
    g.fillRect(x, y, PEAK_SEG_W - 1, PEAK_H, lit ? meterColor(seg, PEAK_SEGS) : UNLIT);
}

void paintPeaks(TFT_eSPI& g) {
    for (int ch = 0; ch < 2; ch++) {
        drawText(g, FONT_LABEL, ch ? "R" : "L", X0, PEAK_Y + ch * PEAK_ROW, COLOR_DIM);
        for (int s = 0; s < PEAK_SEGS; s++) paintPeakSeg(g, ch, s, s < g_wav.segs[ch]);
    }
    // dB marks under the bars.
    static const int MARKS[] = {-30, -20, -10, -3};
    for (int m : MARKS) {
        int seg = PEAK_SEGS - 1 + m;
        char buf[6];
        snprintf(buf, sizeof(buf), "%d", m);
        int x = PEAK_X + seg * PEAK_SEG_W + PEAK_SEG_W / 2 - textWidth(FONT_LABEL, buf) / 2;
        drawText(g, FONT_LABEL, buf, x, PEAK_Y + 2 * PEAK_ROW + 1, COLOR_DIM);
    }
}

int wavColumn(uint32_t elapsed, uint32_t total) {
    if (!total) return 0;
    if (elapsed > total) elapsed = total;
    int c = (int)((uint64_t)elapsed * WAVE_COLS / total);
    return c >= WAVE_COLS ? WAVE_COLS - 1 : c;
}

void wavSide(SideRow* rows) {
    rows[0].label = "VOL";
    snprintf(rows[0].value, sizeof(rows[0].value), "%d", g_volume);
}

void drawWavSide() {
    SideRow rows[1];
    wavSide(rows);
    band(SIDE_X, TOP_Y, SIDE_W, SIDE_ROW, COLOR_BG, [&](TFT_eSPI& g) { paintSide(g, rows, 1); });
}

// The column under the playhead takes the peak of what's about to play,
// the bars the peaks, falling two segments a frame.
void updateWav(const WavInfo& w, bool draw) {
    int n = (int)Synth::wavStreamPeek(g_peek, PEEK_FRAMES);
    int pk[2] = {0, 0};
    for (int i = 0; i < n * 2; i++) {
        int a = g_peek[i] < 0 ? -g_peek[i] : g_peek[i];
        if (a > pk[i & 1]) pk[i & 1] = a;
    }
    if (n && w.channels == 1) pk[1] = pk[0];
    int col = wavColumn(w.elapsedMs, w.totalMs);
    int amp = ((pk[0] > pk[1] ? pk[0] : pk[1]) * WAVE_HALF) >> 15;
    TFT_eSPI& g = screen();
    if (amp > g_wav.col[col]) g_wav.col[col] = (uint8_t)amp;
    if (col != g_wav.head) {
        int old = g_wav.head;
        g_wav.head = col;
        if (draw && old >= 0) paintWaveCol(g, old);
    }
    if (draw) paintWaveCol(g, col);
    for (int ch = 0; ch < 2; ch++) {
        int target = n ? segsFor((uint32_t)pk[ch], g_peakThr, PEAK_SEGS) : 0;
        int old = g_wav.segs[ch];
        int now = target >= old ? target : (old >= 2 ? old - 2 : 0);
        if (now < target) now = target;
        if (now == old || !draw) {
            if (!draw) g_wav.segs[ch] = (uint8_t)now;
            continue;
        }
        int lo = now < old ? now : old, hi = now < old ? old : now;
        for (int s = lo; s < hi; s++) paintPeakSeg(g, ch, s, s < now);
        g_wav.segs[ch] = (uint8_t)now;
    }
}

// ---- time --------------------------------------------------------------------

// Big time, plus "/ total" for WAV. Draws only when the text changes.
void updateTime(uint32_t ms, const char* total) {
    char t[12];
    formatTime(ms, t, sizeof(t));
    if (strcmp(t, g_time) == 0) return;
    strcpy(g_time, t);
    int w = total ? SIDE_X - 4 - X0 : TIME_W;
    band(X0, TOP_Y, w, TOP_H, COLOR_BG, [&](TFT_eSPI& g) {
        paintTime(g, t);
        if (total) {
            char buf[16];
            snprintf(buf, sizeof(buf), "/ %s", total);
            drawText(g, FONT_TEXT, buf, X0 + TIME_W + 6, TOP_Y + TOP_H - 14, COLOR_DIM, 2);
        }
    });
}

} // namespace

// ---- tracker -----------------------------------------------------------------

void drawTracker(const TrackerInfo& t) {
    makeThresholds();
    g_screen = SCREEN_TRACKER;
    g_state = t.state;
    g_volume = t.volume;
    g_time[0] = 0;
    memset(&g_trk, 0, sizeof(g_trk));
    g_trk.channels = t.channels;
    g_trk.lanes = t.channels < 1 ? 1 : t.channels > MAX_LANES ? MAX_LANES : t.channels;
    g_trk.laneW = W / g_trk.lanes;
    g_trk.songLength = t.songLength;
    g_trk.speed = t.speed;
    g_trk.tempo = t.tempo;
    g_trk.order = t.order;
    g_trk.row = t.row;
    g_trk.beat = (t.row / 4) & 3;

    drawStatusFor(ICON_TRACKER, COLOR_ACCENT, "TRACKER");
    drawChip(t.state);
    if (t.state == PS_PLAYING) drawBeat();

    char info[40];
    if (t.state == PS_ERROR) {
        snprintf(info, sizeof(info), "%s", t.format ? t.format : "");
    } else {
        snprintf(info, sizeof(info), "%s %s %dCH %s %d INS", t.format ? t.format : "", MIDDOT, t.channels, MIDDOT, t.instruments);
    }
    band(0, HEADER_H, SCREEN_W, TOP_Y - 2 - HEADER_H, COLOR_BG, [&](TFT_eSPI& g) { paintTitle(g, t.name, info); });
    if (t.state == PS_ERROR) {
        drawErrorBody(SCREEN_TRACKER, t.error);
        return;
    }

    char tm[12];
    formatTime(t.elapsedMs, tm, sizeof(tm));
    strcpy(g_time, tm);
    SideRow side[3];
    trackerSide(side);
    // Top row and lanes in one pass of bands, so nothing flashes.
    UiTap::Row rows[TRAIL_ROWS];
    int nRows = 0;
    if (t.state != PS_STOPPED) UiTap::rowsBefore(Synth::wavStreamPlayed(), rows, TRAIL_ROWS, nRows);
    if (nRows) {
        g_trk.trailAt = rows[nRows - 1].at;
        g_trk.trailDrawn = true;
    }
    band(0, TOP_Y - 2, SCREEN_W, ORD_Y - (TOP_Y - 2), COLOR_BG, [&](TFT_eSPI& g) {
        paintTime(g, tm);
        paintScope(g, 0);
        paintSide(g, side, 3);
        paintLaneHeader(g);
        paintTrail(g, rows, nRows);
        paintMeters(g);
    });
    band(0, ORD_Y, SCREEN_W, BODY_BOTTOM - ORD_Y, COLOR_BG, [&](TFT_eSPI& g) {
        paintOrder(g);
        paintPos(g);
    });
    drawFooterFor(SCREEN_TRACKER, t.state);
}

void trackerState(PlayState s) {
    changeState(SCREEN_TRACKER, s);
    if (g_screen == SCREEN_TRACKER && s != PS_PLAYING) {
        band(g_chipX + CHIP_W, 0, 24, STATUS_H, COLOR_SURFACE, [&](TFT_eSPI&) {});
    } else if (g_screen == SCREEN_TRACKER) {
        drawBeat();
    }
}

void trackerLive(const TrackerInfo& t) {
    if (g_screen != SCREEN_TRACKER || t.state == PS_ERROR) return;
    uint32_t now = millis();
    updateTime(t.elapsedMs, nullptr);
    if (t.speed != g_trk.speed || t.tempo != g_trk.tempo) {
        g_trk.speed = t.speed;
        g_trk.tempo = t.tempo;
        drawTrackerSide();
    }
    uint32_t played = Synth::wavStreamPlayed();
    UiTap::Row rows[TRAIL_ROWS];
    int nRows = 0;
    UiTap::rowsBefore(played, rows, TRAIL_ROWS, nRows);
    // Playing or playing out the end, the position follows what's heard and
    // waits for the first row; paused or stopped it's the player's own.
    bool live = t.state == PS_PLAYING || t.state == PS_DONE;
    if (nRows && live) setPosition(rows[nRows - 1].order, rows[nRows - 1].row, true);
    else if (!live) setPosition(t.order, t.row, true);

    if (nRows && (!g_trk.trailDrawn || rows[nRows - 1].at != g_trk.trailAt) && now - g_lastTrailMs >= TRAIL_MS &&
        Ui::sliceAllows(Ui::DRAW_METER)) {
        Ui::SliceTimer timer;
        g_lastTrailMs = now;
        g_trk.trailAt = rows[nRows - 1].at;
        g_trk.trailDrawn = true;
        drawTrail(rows, nRows);
    }
    if (now - g_lastMeterMs >= METER_MS && Ui::sliceAllows(Ui::DRAW_METER)) {
        Ui::SliceTimer timer;
        g_lastMeterMs = now;
        updateLaneMeters(played, t.state == PS_DONE && drained());
    }
    if (now - g_lastScopeMs >= SCOPE_MS && Ui::sliceAllows(Ui::DRAW_METER)) {
        Ui::SliceTimer timer;
        g_lastScopeMs = now;
        drawScope(true);
    }
}

// ---- MIDI --------------------------------------------------------------------

void drawMidi(const MidiInfo& m) {
    makeThresholds();
    g_screen = SCREEN_MIDI;
    g_state = m.state;
    g_volume = m.volume;
    g_time[0] = 0;
    memset(&g_midi, 0, sizeof(g_midi));
    memcpy(g_midi.hits, UiTap::g_midi.hits, sizeof(g_midi.hits));
    g_midi.bpm = m.bpm;
    g_midi.tempoPct = m.tempoPct;
    snprintf(g_midi.out, sizeof(g_midi.out), "%s", m.out ? m.out : "");
    g_midi.audioOn = m.audioOn;
    g_midi.format = m.format;
    g_midi.tracks = m.tracks;
    g_keysY = KEYS_Y;
    g_keysH = KEYS_H;
    g_blackH = 30;
    uint16_t mask = MidiOutput::activeChannelMask();
    for (int ch = 0; ch < 16; ch++) g_midi.active[ch] = (mask >> ch) & 1;
    for (int n = PIANO_LO; n <= PIANO_HI; n++) g_midi.key[n - PIANO_LO] = keyState(n);

    drawStatusFor(ICON_NOTE, COLOR_MIDI, "MIDI");
    drawChip(m.state);
    band(0, HEADER_H, SCREEN_W, TOP_Y - 2 - HEADER_H, COLOR_BG, [&](TFT_eSPI& g) { paintTitle(g, m.name, nullptr); });
    if (m.state == PS_ERROR) {
        drawErrorBody(SCREEN_MIDI, m.error);
        return;
    }
    char tm[12];
    formatTime(m.elapsedMs, tm, sizeof(tm));
    strcpy(g_time, tm);
    band(0, TOP_Y - 2, SCREEN_W, KEYS_Y - 2 - (TOP_Y - 2), COLOR_BG, [&](TFT_eSPI& g) {
        paintTime(g, tm);
        paintMidiTop(g);
        paintMidiInfo(g);
        paintChannels(g);
    });
    band(0, KEYS_Y - 2, SCREEN_W, BODY_BOTTOM - (KEYS_Y - 2), COLOR_BG, [&](TFT_eSPI& g) { paintKeys(g); });
    drawFooterFor(SCREEN_MIDI, m.state);
}

void midiState(PlayState s) { changeState(SCREEN_MIDI, s); }

void midiLive(const MidiInfo& m) {
    if (g_screen != SCREEN_MIDI || m.state == PS_ERROR) return;
    uint32_t now = millis();
    updateTime(m.elapsedMs, nullptr);
    if (m.bpm != g_midi.bpm || m.tempoPct != g_midi.tempoPct) {
        g_midi.bpm = m.bpm;
        g_midi.tempoPct = m.tempoPct;
        drawMidiTopRight();
    }
    // The catch-up draw on a pause or a seek skips the rate check.
    bool rate = m.state != PS_PLAYING || now - g_lastMeterMs >= METER_MS;
    if (rate && Ui::sliceAllows(Ui::DRAW_METER)) {
        Ui::SliceTimer timer;
        g_lastMeterMs = now;
        updateChannels();
        updateKeys();
    }
}

void keybedDraw(int y, int h) {
    g_screen = SCREEN_NONE;
    g_keysY = y + 2;
    g_keysH = h - 2;
    g_blackH = g_keysH * 3 / 5;
    for (int n = PIANO_LO; n <= PIANO_HI; n++) g_midi.key[n - PIANO_LO] = keyState(n);
    band(0, y, SCREEN_W, h, COLOR_BG, [&](TFT_eSPI& g) { paintKeys(g); });
}

void keybedUpdate() { updateKeys(); }

void midiOutput(const char* out, bool audioOn) {
    if (g_screen != SCREEN_MIDI) return;
    snprintf(g_midi.out, sizeof(g_midi.out), "%s", out ? out : "");
    g_midi.audioOn = audioOn;
    drawMidiInfo();
}

// ---- WAV ---------------------------------------------------------------------

void drawWav(const WavInfo& w) {
    makeThresholds();
    g_screen = SCREEN_WAV;
    g_state = w.state;
    g_volume = w.volume;
    g_time[0] = 0;
    uint32_t key = waveKey(w.fileKey, w.totalMs);
    if (key != g_wav.key) {
        memset(&g_wav, 0, sizeof(g_wav));
        g_wav.key = key;
    }
    g_wav.totalMs = w.totalMs;
    g_wav.head = wavColumn(w.elapsedMs, w.totalMs);
    g_wav.segs[0] = g_wav.segs[1] = 0;

    drawStatusFor(ICON_WAVE, COLOR_WAV, "WAV");
    drawChip(w.state);
    char info[40];
    snprintf(info, sizeof(info), "%lu.%luK %s %dBIT %s %s", (unsigned long)(w.sampleRate / 1000),
             (unsigned long)(w.sampleRate % 1000 / 100), MIDDOT, w.bits, MIDDOT, w.channels == 2 ? "STEREO" : "MONO");
    band(0, HEADER_H, SCREEN_W, TOP_Y - 2 - HEADER_H, COLOR_BG,
         [&](TFT_eSPI& g) { paintTitle(g, w.name, w.state == PS_ERROR ? nullptr : info); });
    if (w.state == PS_ERROR) {
        drawErrorBody(SCREEN_WAV, w.error);
        return;
    }
    char tm[12], total[12], tot[16];
    formatTime(w.elapsedMs, tm, sizeof(tm));
    formatTime(w.totalMs, total, sizeof(total));
    snprintf(tot, sizeof(tot), "/ %s", total);
    strcpy(g_time, tm);
    SideRow side[1];
    wavSide(side);
    band(0, TOP_Y - 2, SCREEN_W, BODY_BOTTOM - (TOP_Y - 2), COLOR_BG, [&](TFT_eSPI& g) {
        paintTime(g, tm);
        drawText(g, FONT_TEXT, tot, X0 + TIME_W + 6, TOP_Y + TOP_H - 14, COLOR_DIM, 2);
        paintSide(g, side, 1);
        paintWave(g);
        paintPeaks(g);
    });
    drawFooterFor(SCREEN_WAV, w.state);
}

void wavState(PlayState s) { changeState(SCREEN_WAV, s); }

void wavLive(const WavInfo& w) {
    if (g_screen != SCREEN_WAV || w.state == PS_ERROR) return;
    uint32_t now = millis();
    char total[12];
    formatTime(w.totalMs, total, sizeof(total));
    updateTime(w.elapsedMs, total);
    // A seek or stop moves the playhead while nothing plays, so those
    // draws (meters forced) skip the rate check.
    bool playing = w.state == PS_PLAYING || (w.state == PS_DONE && !drained());
    if ((!playing || now - g_lastMeterMs >= METER_MS) && Ui::sliceAllows(Ui::DRAW_METER)) {
        Ui::SliceTimer timer;
        g_lastMeterMs = now;
        if (playing) {
            updateWav(w, true);
        } else {
            // Just the playhead, and the bars down.
            int col = wavColumn(w.elapsedMs, w.totalMs);
            TFT_eSPI& g = screen();
            if (col != g_wav.head) {
                int old = g_wav.head;
                g_wav.head = col;
                if (old >= 0) paintWaveCol(g, old);
                paintWaveCol(g, col);
            }
            for (int ch = 0; ch < 2; ch++) {
                for (int s = 0; s < g_wav.segs[ch]; s++) paintPeakSeg(g, ch, s, false);
                g_wav.segs[ch] = 0;
            }
        }
    }
}

// ---- shared ------------------------------------------------------------------

bool liveSettled() {
    if (!drained()) return false;
    if (g_screen == SCREEN_TRACKER) {
        for (int i = 0; i < g_trk.lanes; i++)
            if (g_trk.segs[i]) return false;
    } else if (g_screen == SCREEN_WAV) {
        if (g_wav.segs[0] || g_wav.segs[1]) return false;
    }
    return true;
}

void setVolume(int volume) {
    g_volume = volume;
    if (g_screen == SCREEN_TRACKER && g_state != PS_ERROR) drawTrackerSide();
    else if (g_screen == SCREEN_MIDI && g_state != PS_ERROR) drawMidiTopRight();
    else if (g_screen == SCREEN_WAV && g_state != PS_ERROR) drawWavSide();
}

void formatNote(uint8_t note, char* out) { noteText(note, out); }

int peek(const int16_t*& frames) {
    frames = g_peek;
    return (int)Synth::wavStreamPeek(g_peek, PEEK_FRAMES);
}

} // namespace UiPlayer

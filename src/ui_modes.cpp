#include "ui_modes.h"
#include "ui_look.h"
#include "ui_strip.h"
#include "ui_player.h"
#include "ui_tap.h"
#include "midi_output.h"
#include "keyboard_layout.h"
#include <stdio.h>
#include <string.h>

using namespace Look;

namespace UiModes {
namespace {

// Colours from the mockups that aren't theme entries.
const uint16_t UNLIT = rgb(0x1A212B);
const uint16_t LANE_BG = rgb(0x0D1219);
const uint16_t LANE_BORDER = rgb(0x1F2733);
const uint16_t TICK_BEAT = rgb(0x1A212B);
const uint16_t TICK_BAR = rgb(0x3A4452);
const uint16_t REC_ROW = rgb(0x160E10);
const uint16_t LABEL_OFF = rgb(0x5A6575);

const uint32_t LIVE_MS = 40; // 25 fps, as the player meters

const int BODY_BOTTOM = SCREEN_H - FOOTER_H;

// a over b, alpha of 256.
uint16_t blend(uint16_t a, uint16_t b, int alpha) {
    int ra = a >> 11, ga = (a >> 5) & 63, ba = a & 31;
    int rb = b >> 11, gb = (b >> 5) & 63, bb = b & 31;
    int r = (ra * alpha + rb * (256 - alpha)) >> 8;
    int g = (ga * alpha + gb * (256 - alpha)) >> 8;
    int bl = (ba * alpha + bb * (256 - alpha)) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Cyan to 60%, amber to 80%, red above, as the player meters.
uint16_t meterColor(int seg, int segs) {
    if (seg * 10 < segs * 6) return COLOR_SELECT;
    if (seg * 10 < segs * 8) return COLOR_ACCENT;
    return COLOR_ERROR;
}

void drawTextRight(TFT_eSPI& g, const PixFont& f, const char* s, int xr, int y, uint16_t c, int scale = 1) {
    drawText(g, f, s, xr - textWidth(f, s, scale), y, c, scale);
}

void drawTextCentre(TFT_eSPI& g, const PixFont& f, const char* s, int cx, int y, uint16_t c, int scale = 1) {
    drawText(g, f, s, cx - textWidth(f, s, scale) / 2, y, c, scale);
}

// A selectable box: cyan outline and left stripe when selected, else a rule.
void paintBox(TFT_eSPI& g, int x, int y, int w, int h, bool selected) {
    if (selected) {
        g.drawRect(x, y, w, h, COLOR_SELECT);
        g.fillRect(x, y, 2, h, COLOR_SELECT);
    } else {
        g.drawRect(x, y, w, h, COLOR_RULE);
    }
}

// =============================================================================
// Looper
// =============================================================================

// ---- layout, 320x240 --------------------------------------------------------
const int BPM_Y = HEADER_H;      // the BPM block
const int BPM_H = 39;
const int BPM_RULE_Y = BPM_Y + BPM_H;
const int BIG_X = 10;            // big BPM digits, VT323 at 4x
const int BIG_Y = BPM_Y + 6;
const int FIELD_A_X = 112;       // time sig, count in
const int FIELD_B_X = 160;       // metro, sync
const int FIELD_Y1 = BPM_Y + 9;
const int FIELD_Y2 = BPM_Y + 24;
const int BB_X = 232;            // BAR · BEAT area, right aligned at BB_R
const int BB_R = SCREEN_W - 7;

const int ROW_X = 6;
const int ROW_W = SCREEN_W - 2 * ROW_X;
const int ROW_Y0 = BPM_RULE_Y + 5;
const int ROW_H = 33;
const int ROW_STEP = ROW_H + 4;
const int CHIP_DX = 20, CHIP_DY = 6, CHIP_W = 32, CHIP_H = 11;
const int ROUTE_DX = 58;
const int LANE_DX = 104;
const int LANE_X = ROW_X + LANE_DX;
const int LANE_W = 170;           // outer, with its border
const int LANE_IW = LANE_W - 2;   // inner columns
const int LANE_DY = 7;            // border top within the row
const int LANE_H = 19;
const int LANE_IH = LANE_H - 2;
const int HEAD_DY = LANE_DY - 2;  // the playhead stands 2 px out of the lane
const int HEAD_H = LANE_H + 4;
const int TIME_R = ROW_X + ROW_W - 5;

int rowY(int idx) { return ROW_Y0 + idx * ROW_STEP; }

// Event dots: a nibble per lane column, 0 none, else the dot's row + 1.
const int DOT_BYTES = LANE_IW / 2;
uint8_t g_dots[4][DOT_BYTES];
uint8_t g_dotsNew[DOT_BYTES];
int g_dotsTrack = -1;

int dotAt(int track, int c) {
    if (c < 0 || c >= LANE_IW) return 0;
    uint8_t b = g_dots[track][c >> 1];
    return (c & 1) ? b >> 4 : b & 15;
}

// What each lane shows now, so a live update repaints only what moved.
struct Lane {
    int16_t head;       // first playhead column, -1 none
    int16_t fill;       // filled columns
    int16_t ticks;      // segments the lane is split into, 0 none
    uint8_t beatsPerBar;
    LoopTrackState state;
    uint32_t lengthMs;
    uint16_t rowBg, fillColor, headColor, dotColor;
    int16_t dirtyLo, dirtyHi; // dot columns to repaint, lo > hi none
    int8_t leds;        // activity lights shown
    bool drawn;
    bool regrid;        // the tempo or metre changed, check the ticks
};
Lane g_lane[4];

bool g_looperUp = false; // which of the two screens is up, for the live calls
bool g_synUp = false;
bool g_bpmSelected = false;
int g_sigNum = 4;
uint32_t g_barMs = 2000;
int g_beatX = 0;
int g_bar = 0, g_beat = 0;
bool g_countIn = false;

// ---- track rows --------------------------------------------------------------

uint16_t rowBgFor(const LoopTrackView& t, bool selected) {
    if (selected) return COLOR_HILITE_BG;
    return t.state == LOOP_TRACK_RECORDING ? REC_ROW : COLOR_BG;
}

bool headShown(LoopTrackState s) {
    return s == LOOP_TRACK_PLAYING || s == LOOP_TRACK_RECORDING || s == LOOP_TRACK_PAUSED || s == LOOP_TRACK_MUTED;
}

// Bars the lane covers: the set bar length, or a freeform loop's length
// rounded to bars. 0 when there's nothing to measure.
int laneBars(const LoopTrackView& t) {
    if (t.barLength > 0) return (t.lengthMs > 0 || t.state == LOOP_TRACK_RECORDING) ? t.barLength : 0;
    if (t.lengthMs == 0 || g_barMs == 0) return 0;
    return (int)((t.lengthMs + g_barMs / 2) / g_barMs);
}

// Head and fill columns for a track's position.
void headFor(const LoopTrackView& t, int16_t& head, int16_t& fill) {
    if (!headShown(t.state) || (t.lengthMs == 0 && t.barLength == 0)) {
        head = -1;
        fill = 0;
        return;
    }
    int c = (int)(((uint32_t)t.pos * LANE_IW) >> 16);
    if (c >= LANE_IW) c = LANE_IW - 1;
    head = (int16_t)c;
    fill = (int16_t)c;
}

// Sets up a lane's colours and ticks from the track. The head and dots
// stay as drawn.
void laneSetup(const LoopTrackView& t, int idx, bool selected) {
    Lane& L = g_lane[idx];
    L.state = t.state;
    L.lengthMs = t.lengthMs;
    L.rowBg = rowBgFor(t, selected);
    switch (t.state) {
        case LOOP_TRACK_RECORDING:
            L.fillColor = blend(COLOR_TRACK_RECORDING, LANE_BG, 56);
            L.headColor = COLOR_ACCENT;
            break;
        case LOOP_TRACK_PAUSED:
        case LOOP_TRACK_MUTED:
            L.fillColor = blend(COLOR_DIM, LANE_BG, 36);
            L.headColor = COLOR_DIM;
            break;
        default:
            L.fillColor = blend(COLOR_TRACK_PLAYING, LANE_BG, 36);
            L.headColor = COLOR_ACCENT;
            break;
    }
    bool live = t.state == LOOP_TRACK_PLAYING || t.state == LOOP_TRACK_RECORDING || t.state == LOOP_TRACK_PAUSED;
    L.dotColor = live ? COLOR_TEXT : COLOR_DIM;
    int bars = laneBars(t);
    int beats = bars * g_sigNum;
    if (bars > 0 && beats * 3 <= LANE_IW) {
        L.ticks = (int16_t)beats;
        L.beatsPerBar = (uint8_t)g_sigNum;
    } else if (bars > 0 && bars * 3 <= LANE_IW) {
        L.ticks = (int16_t)bars;
        L.beatsPerBar = 1;
    } else {
        L.ticks = 0;
        L.beatsPerBar = 1;
    }
    L.regrid = false;
}

// After a full lane paint: the head where the track is, no dots pending.
void laneFresh(const LoopTrackView& t, int idx) {
    Lane& L = g_lane[idx];
    headFor(t, L.head, L.fill);
    L.dirtyLo = 1;
    L.dirtyHi = 0;
    L.drawn = true;
}

// Tick colour for a column, 0 for none: the last column of each segment
// gets the line, as the mockup's border-right.
uint16_t tickAt(const Lane& L, int c) {
    if (L.ticks < 2 || c >= LANE_IW - 1) return 0;
    int a = c * L.ticks / LANE_IW, b = (c + 1) * L.ticks / LANE_IW;
    if (a == b) return 0;
    return (b % L.beatsPerBar) == 0 ? TICK_BAR : TICK_BEAT;
}

// Lane columns c0..c1, from 2 px above the lane to 2 px below.
void paintLaneCols(TFT_eSPI& g, int idx, int c0, int c1) {
    const Lane& L = g_lane[idx];
    int y = rowY(idx) + HEAD_DY;
    if (c0 < 0) c0 = 0;
    if (c1 >= LANE_IW) c1 = LANE_IW - 1;
    for (int c = c0; c <= c1; c++) {
        int x = LANE_X + 1 + c;
        if (L.head >= 0 && (c == L.head || c == L.head + 1)) {
            g.fillRect(x, y, 1, HEAD_H, L.headColor);
            continue;
        }
        g.fillRect(x, y, 1, 2, L.rowBg);
        g.fillRect(x, y + HEAD_H - 2, 1, 2, L.rowBg);
        g.drawPixel(x, y + 2, LANE_BORDER);
        g.drawPixel(x, y + HEAD_H - 3, LANE_BORDER);
        uint16_t tick = tickAt(L, c);
        uint16_t base = tick ? tick : (c < L.fill ? L.fillColor : LANE_BG);
        g.fillRect(x, y + 3, 1, LANE_IH, base);
        int d = dotAt(idx, c);
        if (!d) d = dotAt(idx, c - 1);
        if (d) g.fillRect(x, y + 3 + d - 1, 1, 2, L.dotColor);
    }
}

void paintLane(TFT_eSPI& g, int idx) {
    int y = rowY(idx) + HEAD_DY;
    const Lane& L = g_lane[idx];
    g.fillRect(LANE_X, y, 1, 2, L.rowBg);
    g.fillRect(LANE_X + LANE_W - 1, y, 1, 2, L.rowBg);
    g.fillRect(LANE_X, y + HEAD_H - 2, 1, 2, L.rowBg);
    g.fillRect(LANE_X + LANE_W - 1, y + HEAD_H - 2, 1, 2, L.rowBg);
    g.fillRect(LANE_X, y + 2, 1, LANE_H, LANE_BORDER);
    g.fillRect(LANE_X + LANE_W - 1, y + 2, 1, LANE_H, LANE_BORDER);
    paintLaneCols(g, idx, 0, LANE_IW - 1);
}

void drawLaneCols(int idx, int c0, int c1) {
    if (c0 < 0) c0 = 0;
    if (c1 >= LANE_IW) c1 = LANE_IW - 1;
    if (c1 < c0) return;
    band(LANE_X + 1 + c0, rowY(idx) + HEAD_DY, c1 - c0 + 1, HEAD_H, g_lane[idx].rowBg,
         [&](TFT_eSPI& g) { paintLaneCols(g, idx, c0, c1); });
}

void drawLane(int idx) {
    band(LANE_X, rowY(idx) + HEAD_DY, LANE_W, HEAD_H, g_lane[idx].rowBg, [&](TFT_eSPI& g) { paintLane(g, idx); });
}

// The state chip: label, fill or outline.
void paintChip(TFT_eSPI& g, const LoopTrackView& t, int x, int y) {
    const char* label = "EMPTY";
    uint16_t fill = 0, line = 0, fg = LABEL_OFF;
    switch (t.state) {
        case LOOP_TRACK_ARMED: label = "ARMED"; line = fg = COLOR_TRACK_ARMED; break;
        case LOOP_TRACK_RECORDING:
            label = t.lengthMs > 0 ? "DUB" : "REC";
            fill = COLOR_TRACK_RECORDING;
            fg = COLOR_BG;
            break;
        case LOOP_TRACK_PLAYING: label = "PLAY"; fill = COLOR_TRACK_PLAYING; fg = COLOR_BG; break;
        case LOOP_TRACK_MUTED: label = "MUTE"; fill = COLOR_RULE; fg = COLOR_DIM; break;
        case LOOP_TRACK_STOPPED: label = "STOP"; line = COLOR_RULE; fg = COLOR_TRACK_STOPPED; break;
        case LOOP_TRACK_PAUSED: label = "PAUSE"; line = fg = COLOR_TRACK_PAUSED; break;
        default: break;
    }
    if (fill) g.fillRect(x, y, CHIP_W, CHIP_H, fill);
    if (line) g.drawRect(x, y, CHIP_W, CHIP_H, line);
    drawTextCentre(g, FONT_LABEL, label, x + CHIP_W / 2 + 1, y + 3, fg);
}

void routeText(const LoopTrackView& t, char* out, size_t n) {
    char in[6], o[6];
    if (t.channelIn >= 16) snprintf(in, sizeof(in), "OMNI");
    else snprintf(in, sizeof(in), "%02d", t.channelIn + 1);
    if (t.channelOut >= 16) snprintf(o, sizeof(o), "REC");
    else snprintf(o, sizeof(o), "%02d", t.channelOut + 1);
    snprintf(out, n, "%s>%s", in, o);
}

void paintLeds(TFT_eSPI& g, int idx, int leds) {
    int x = ROW_X + 5, y = rowY(idx) + 19;
    g.fillRect(x, y, 4, 3, (leds & 1) ? COLOR_TRACK_RECORDING : COLOR_RULE);
    g.fillRect(x + 6, y, 4, 3, (leds & 2) ? COLOR_TRACK_PLAYING : COLOR_RULE);
}

int ledsFor(const LoopTrackView& t) { return (t.recordActive ? 1 : 0) | (t.playActive ? 2 : 0); }

// T1, the lights, chip, route and bar length.
void paintInfo(TFT_eSPI& g, const LoopTrackView& t, int idx) {
    int x = ROW_X, y = rowY(idx);
    char buf[16];
    snprintf(buf, sizeof(buf), "T%d", idx + 1);
    drawText(g, FONT_LABEL, buf, x + 5, y + 7, t.state == LOOP_TRACK_EMPTY ? COLOR_DIM : COLOR_TEXT);
    paintLeds(g, idx, ledsFor(t));
    paintChip(g, t, x + CHIP_DX, y + CHIP_DY);
    if (t.bufferFull) drawTextCentre(g, FONT_LABEL, "FULL", x + CHIP_DX + CHIP_W / 2 + 1, y + 21, COLOR_ERROR);
    routeText(t, buf, sizeof(buf));
    drawText(g, FONT_TEXT, buf, x + ROUTE_DX, y + 7, t.state == LOOP_TRACK_EMPTY ? COLOR_DIM : COLOR_TEXT);
    if (t.barLength > 0) snprintf(buf, sizeof(buf), "%d BAR", t.barLength);
    else snprintf(buf, sizeof(buf), "FREE");
    drawText(g, FONT_LABEL, buf, x + ROUTE_DX, y + 20, COLOR_DIM);
}

void paintTime(TFT_eSPI& g, const LoopTrackView& t, int idx) {
    if (t.lengthMs == 0) return;
    char buf[12];
    uint32_t s = t.lengthMs / 1000;
    if (s < 100) snprintf(buf, sizeof(buf), "%lu.%lus", (unsigned long)s, (unsigned long)((t.lengthMs / 100) % 10));
    else snprintf(buf, sizeof(buf), "%lus", (unsigned long)s);
    uint16_t c = COLOR_TEXT;
    if (t.state == LOOP_TRACK_RECORDING) c = COLOR_TRACK_RECORDING;
    else if (t.state != LOOP_TRACK_PLAYING && t.state != LOOP_TRACK_PAUSED) c = COLOR_DIM;
    drawTextRight(g, FONT_TEXT, buf, TIME_R, rowY(idx) + 13, c);
}

void drawRow(const LoopTrackView& t, int idx, bool selected) {
    laneSetup(t, idx, selected);
    laneFresh(t, idx);
    Lane& L = g_lane[idx];
    L.leds = (int8_t)ledsFor(t);
    int y = rowY(idx);
    band(ROW_X, y, ROW_W, ROW_H, L.rowBg, [&](TFT_eSPI& g) {
        paintBox(g, ROW_X, y, ROW_W, ROW_H, selected);
        paintInfo(g, t, idx);
        paintLane(g, idx);
        paintTime(g, t, idx);
    });
}

void drawInfo(const LoopTrackView& t, int idx, bool selected) {
    int y = rowY(idx);
    uint16_t bg = rowBgFor(t, selected);
    // Inside the row's outline and stripe.
    band(ROW_X + 2, y + 1, LANE_DX - 3, ROW_H - 2, bg, [&](TFT_eSPI& g) { paintInfo(g, t, idx); });
    g_lane[idx].leds = (int8_t)ledsFor(t);
}

void drawTime(const LoopTrackView& t, int idx, bool selected) {
    int x = LANE_X + LANE_W + 1;
    band(x, rowY(idx) + 1, TIME_R + 1 - x, ROW_H - 2, rowBgFor(t, selected),
         [&](TFT_eSPI& g) { paintTime(g, t, idx); });
}

// ---- BPM block -----------------------------------------------------------------

void paintField(TFT_eSPI& g, const char* s, int x, int y, uint16_t c, bool focused) {
    int w = textWidth(FONT_LABEL, s);
    if (focused) g.drawRect(x - 3, y - 3, w + 5, 11, COLOR_SELECT);
    drawText(g, FONT_LABEL, s, x, y, c);
}

uint16_t onOff(bool on) { return on ? COLOR_SELECT : COLOR_DIM; }

void paintBarBeat(TFT_eSPI& g) {
    drawTextRight(g, FONT_LABEL, "BAR \x7f BEAT", BB_R, BPM_Y + 6, COLOR_DIM);
    // Bar, a dot, beat, at 2x; the dot is drawn, VT323's sits too high.
    char bar[12], beat[12];
    if (g_bar > 0) {
        snprintf(bar, sizeof(bar), "%d", g_bar);
        snprintf(beat, sizeof(beat), "%d", g_beat);
    } else {
        strcpy(bar, "-");
        strcpy(beat, "-");
    }
    uint16_t c = g_bar > 0 ? COLOR_TEXT : COLOR_DIM;
    int y = BPM_Y + 17;
    int x = BB_R - textWidth(FONT_TEXT, beat, 2);
    drawText(g, FONT_TEXT, beat, x, y, c, 2);
    x -= 10;
    g.fillRect(x + 2, y + 6, 3, 3, COLOR_DIM);
    drawTextRight(g, FONT_TEXT, bar, x - 2, y, c, 2);
}

void paintBpm(TFT_eSPI& g, const LoopBpmRow& r) {
    if (r.selected) paintBox(g, 0, BPM_Y, SCREEN_W, BPM_H, true);
    bool sel = r.selected;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", r.bpm);
    int w = textWidth(FONT_TEXT, buf, 4);
    if (sel && r.focus == 0) g.drawRect(BIG_X - 4, BIG_Y - 3, w + 7, 7 * 4 + 5, COLOR_SELECT);
    drawText(g, FONT_TEXT, buf, BIG_X, BIG_Y, COLOR_ACCENT, 4);
    drawText(g, FONT_LABEL, "BPM", BIG_X + w + 6, BIG_Y + 28 - 5, COLOR_DIM);

    snprintf(buf, sizeof(buf), "%d/%d", r.sigNum, r.sigDen);
    paintField(g, buf, FIELD_A_X, FIELD_Y1, COLOR_TEXT, sel && r.focus == 1);
    if (r.showMetroVolume) snprintf(buf, sizeof(buf), "%d%%", r.metroVolume);
    else snprintf(buf, sizeof(buf), "METRO");
    paintField(g, buf, FIELD_B_X, FIELD_Y1, onOff(r.metroOn), sel && r.focus == 2);
    if (r.showCountInBars) snprintf(buf, sizeof(buf), "%d BAR%s", r.countInBars, r.countInBars == 1 ? "" : "S");
    else snprintf(buf, sizeof(buf), "COUNT");
    paintField(g, buf, FIELD_A_X, FIELD_Y2, onOff(r.countIn), sel && r.focus == 3);
    paintField(g, "SYNC", FIELD_B_X, FIELD_Y2, onOff(r.sync), sel && r.focus == 4);
    paintBarBeat(g);
}

void drawBpm(const LoopBpmRow& r) {
    g_bpmSelected = r.selected;
    g_sigNum = r.sigNum > 0 ? r.sigNum : 4;
    g_barMs = r.barMs;
    band(0, BPM_Y, SCREEN_W, BPM_H, r.selected ? COLOR_HILITE_BG : COLOR_BG, [&](TFT_eSPI& g) { paintBpm(g, r); });
}

// ---- footer and status -------------------------------------------------------

void drawLooperFooter(bool onBpm) {
    if (onBpm) {
        Key r1[] = {{"UD", "row", TONE_TEXT}, {"LR", "field", TONE_TEXT}, {"EDIT", "set", TONE_TEXT},
                    {"ALT+UD", "on/off", TONE_TEXT}, {"PLAY", "start", TONE_PLAY}};
        Key r2[] = {{"NAV", "stop", TONE_TEXT}, {"ENT", "menu", TONE_TEXT}};
        drawKeyFooter(r1, 5, r2, 2);
    } else {
        Key r1[] = {{"UD", "row", TONE_TEXT}, {"PLAY", "pause", TONE_PLAY}, {"EDIT", "rec", TONE_ERROR},
                    {"RIGHT", "mute", TONE_TEXT}, {"NAV", "stop", TONE_TEXT}};
        Key r2[] = {{"LEFT", "back", TONE_TEXT}, {"ALT+UD", "out", TONE_TEXT}, {"ALT+LR", "in", TONE_TEXT},
                    {"EDIT+LR", "bar", TONE_TEXT}, {"ENT", "menu", TONE_TEXT}};
        drawKeyFooter(r1, 5, r2, 5);
    }
}

const int BEAT_MAX = 7;

void drawLooperStatus() {
    Status st;
    st.icon = &ICON_LOOPER;
    st.iconColor = COLOR_ACCENT;
    st.title = "LOOPER";
    drawStatus(st);
    g_beatX = 6 + ICON_LOOPER.w + 4 + textWidth(FONT_LABEL, "LOOPER") + 8;
}

} // namespace

// ---- looper, public ------------------------------------------------------------

void looperDraw(const LoopTrackView t[4], int selectedTrack, const LoopBpmRow& row, bool showBeats, int beat) {
    g_looperUp = true;
    g_synUp = false;
    g_countIn = false;
    drawLooperStatus();
    looperBeats(showBeats, beat, row.sigNum);
    drawBpm(row);
    TFT_eSPI& s = screen();
    s.drawFastHLine(0, BPM_RULE_Y, SCREEN_W, COLOR_RULE);
    s.fillRect(0, BPM_RULE_Y + 1, SCREEN_W, ROW_Y0 - BPM_RULE_Y - 1, COLOR_BG);
    for (int i = 0; i < 4; i++) {
        bool sel = !row.selected && i == selectedTrack;
        drawRow(t[i], i, sel);
        if (i < 3) s.fillRect(0, rowY(i) + ROW_H, SCREEN_W, ROW_STEP - ROW_H, COLOR_BG);
    }
    s.fillRect(0, ROW_Y0, ROW_X, rowY(3) + ROW_H - ROW_Y0, COLOR_BG);
    s.fillRect(ROW_X + ROW_W, ROW_Y0, SCREEN_W - ROW_X - ROW_W, rowY(3) + ROW_H - ROW_Y0, COLOR_BG);
    int end = rowY(3) + ROW_H;
    if (end < BODY_BOTTOM) s.fillRect(0, end, SCREEN_W, BODY_BOTTOM - end, COLOR_BG);
    drawLooperFooter(row.selected);
}

void looperLive(const LoopTrackView t[4], int selectedTrack) {
    if (!g_looperUp || g_countIn) return;
    TFT_eSPI& s = screen();
    for (int i = 0; i < 4; i++) {
        Lane& L = g_lane[i];
        bool sel = i == selectedTrack;
        // A state change the looper didn't draw itself (an auto commit).
        if (!L.drawn || t[i].state != L.state || rowBgFor(t[i], sel) != L.rowBg) {
            drawRow(t[i], i, sel);
            continue;
        }
        if (t[i].lengthMs != L.lengthMs || L.regrid) looperTrackLength(t[i], i, sel);
        int leds = ledsFor(t[i]);
        if (leds != L.leds) {
            L.leds = (int8_t)leds;
            paintLeds(s, i, leds);
        }
        int16_t head, fill;
        headFor(t[i], head, fill);
        bool moved = head != L.head;
        bool dots = L.dirtyLo <= L.dirtyHi;
        if (!moved && !dots) continue;
        // Moving parts wait while the slice is spent or the ring is low.
        if (!Ui::sliceAllows(Ui::DRAW_METER)) continue;
        Ui::SliceTimer timer;
        int lo = LANE_IW, hi = -1;
        if (moved) {
            if (head < 0 || L.head < 0 || head < L.head) {
                lo = 0;  // wrapped or shown or hidden: the fill changes all over
                hi = LANE_IW - 1;
            } else {
                lo = L.head;
                hi = head + 1;
            }
            L.head = head;
            L.fill = fill;
        }
        if (dots) {
            // Separate spans go out separately, so a dot far off doesn't
            // pull the whole lane in.
            if (hi >= 0 && (L.dirtyLo > hi + 8 || L.dirtyHi + 8 < lo)) {
                drawLaneCols(i, L.dirtyLo, L.dirtyHi + 1);
            } else {
                if (L.dirtyLo < lo) lo = L.dirtyLo;
                if (L.dirtyHi + 1 > hi) hi = L.dirtyHi + 1;
            }
            L.dirtyLo = 1;
            L.dirtyHi = 0;
        }
        if (hi >= lo) drawLaneCols(i, lo, hi);
    }
}

void looperTrackRow(const LoopTrackView& t, int idx, bool selected) {
    if (idx < 0 || idx > 3) return;
    drawRow(t, idx, selected);
}

void looperTrackInfo(const LoopTrackView& t, int idx, bool selected) {
    if (idx < 0 || idx > 3) return;
    drawInfo(t, idx, selected);
}

void looperTrackLength(const LoopTrackView& t, int idx, bool selected) {
    if (idx < 0 || idx > 3) return;
    Lane& L = g_lane[idx];
    int16_t oldTicks = L.ticks;
    uint8_t oldBpb = L.beatsPerBar;
    bool timeChanged = t.lengthMs != L.lengthMs;
    laneSetup(t, idx, selected);
    if (timeChanged) drawTime(t, idx, selected);
    if (L.ticks != oldTicks || L.beatsPerBar != oldBpb) {
        laneFresh(t, idx);
        drawLane(idx);
    }
}

void looperSelection(const LoopTrackView t[4], int prevSel, bool prevOnBpm, int newSel, bool newOnBpm,
                     const LoopBpmRow& row) {
    if (prevOnBpm || newOnBpm) drawBpm(row);
    if (!prevOnBpm && prevSel >= 0 && prevSel < 4) drawRow(t[prevSel], prevSel, false);
    if (!newOnBpm && newSel >= 0 && newSel < 4) drawRow(t[newSel], newSel, true);
    if (prevOnBpm != newOnBpm) drawLooperFooter(newOnBpm);
}

void looperBpm(const LoopBpmRow& row) {
    bool gridChanged = row.sigNum != g_sigNum || row.barMs != g_barMs;
    drawBpm(row);
    // Freeform lanes measure their bars against the tempo; looperLive()
    // redraws a lane whose ticks change.
    if (gridChanged) {
        for (int i = 0; i < 4; i++) g_lane[i].regrid = true;
    }
}

void looperBeatFlash(bool active, bool downbeat) {
    if (!g_looperUp || g_countIn) return;
    uint16_t bg = g_bpmSelected ? COLOR_HILITE_BG : COLOR_BG;
    uint16_t c = active ? (downbeat ? COLOR_DOWNBEAT : COLOR_PULSE) : bg;
    screen().drawRect(3, BPM_Y + 2, SCREEN_W - 6, BPM_H - 4, c);
}

void looperBeats(bool visible, int beat, int num) {
    if (num > BEAT_MAX) num = BEAT_MAX;
    band(g_beatX, 0, BEAT_MAX * 8, STATUS_H, COLOR_SURFACE, [&](TFT_eSPI& g) {
        if (!visible) return;
        for (int i = 0; i < num; i++) {
            int x = g_beatX + i * 8;
            if (i == beat) g.fillRect(x, 5, 6, 6, i == 0 ? COLOR_DOWNBEAT : COLOR_PULSE);
            else g.drawRect(x, 5, 6, 6, i == 0 ? COLOR_DIM : COLOR_RULE);
        }
    });
}

void looperBarBeat(int bar, int beat) {
    if (bar < 0) bar = 0;
    if (bar == g_bar && beat == g_beat) return;
    g_bar = bar;
    g_beat = beat;
    if (!g_looperUp || g_countIn) return;
    // Inside the beat flash's outline.
    band(BB_X, BPM_Y + 4, BB_R + 1 - BB_X, BPM_H - 8, g_bpmSelected ? COLOR_HILITE_BG : COLOR_BG,
         [&](TFT_eSPI& g) { paintBarBeat(g); });
}

static const int COUNT_Y = 92; // the big count, VT323 at 8x

static void paintCount(TFT_eSPI& g, int beatsLeft) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", beatsLeft);
    drawTextCentre(g, FONT_TEXT, buf, SCREEN_W / 2, COUNT_Y, COLOR_ACCENT, 8);
}

void looperCountIn(int beatsLeft) {
    g_looperUp = true;
    g_synUp = false;
    g_countIn = true;
    for (int i = 0; i < 4; i++) g_lane[i].drawn = false;
    drawLooperStatus();
    looperBeats(false, -1, 0);
    band(0, HEADER_H, SCREEN_W, BODY_BOTTOM - HEADER_H, COLOR_BG, [&](TFT_eSPI& g) {
        drawTextCentre(g, FONT_LABEL, "COUNT IN", SCREEN_W / 2, COUNT_Y - 22, COLOR_DIM);
        paintCount(g, beatsLeft);
    });
    drawLooperFooter(true);
}

void looperCountInBeat(int beatsLeft) {
    if (!g_countIn) return;
    band(0, COUNT_Y - 4, SCREEN_W, 7 * 8 + 8, COLOR_BG, [&](TFT_eSPI& g) { paintCount(g, beatsLeft); });
}

// Column scale for the lane being built, Q16: offset * scale >> 16 is the
// column, which fits 32 bits as offset < length.
uint32_t g_dotScale = 0;
uint32_t g_dotLen = 0;

void laneDotsBegin(int track, uint32_t lengthMs) {
    g_dotsTrack = (track >= 0 && track < 4) ? track : -1;
    g_dotLen = lengthMs;
    g_dotScale = lengthMs ? ((uint32_t)LANE_IW << 16) / lengthMs : 0;
    memset(g_dotsNew, 0, sizeof(g_dotsNew));
}

void laneDot(uint32_t offsetMs, uint8_t note) {
    if (g_dotsTrack < 0 || g_dotLen == 0) return;
    if (offsetMs >= g_dotLen) offsetMs = g_dotLen - 1;
    int c = (int)((offsetMs * g_dotScale) >> 16);
    if (c >= LANE_IW) c = LANE_IW - 1;
    // Pitch to a row: C2 (36) at the bottom, C7 (96) at the top.
    int n = note < 36 ? 36 : note > 96 ? 96 : note;
    int row = (LANE_IH - 3) - (n - 36) * (LANE_IH - 3) / 60; // 0 to 14
    uint8_t v = (uint8_t)(row + 1);
    uint8_t& b = g_dotsNew[c >> 1];
    if (c & 1) b = (uint8_t)((b & 0x0F) | (v << 4));
    else b = (uint8_t)((b & 0xF0) | v);
}

void laneDotsEnd() {
    int t = g_dotsTrack;
    g_dotsTrack = -1;
    if (t < 0) return;
    Lane& L = g_lane[t];
    for (int i = 0; i < DOT_BYTES; i++) {
        if (g_dots[t][i] == g_dotsNew[i]) continue;
        int c0 = i * 2, c1 = i * 2 + 1;
        if (L.dirtyLo > L.dirtyHi) {
            L.dirtyLo = (int16_t)c0;
            L.dirtyHi = (int16_t)c1;
        } else {
            if (c0 < L.dirtyLo) L.dirtyLo = (int16_t)c0;
            if (c1 > L.dirtyHi) L.dirtyHi = (int16_t)c1;
        }
    }
    memcpy(g_dots[t], g_dotsNew, DOT_BYTES);
}

// =============================================================================
// pariSynth
// =============================================================================

namespace {

// Family icons, 10x10, in GM family order, then drums.
const uint16_t FAM_PIANO_ROWS[] = {
    0x0000, // ..........
    0xFFC0, // ##########
    0xB6C0, // #.##.##.##
    0xB6C0, // #.##.##.##
    0xB6C0, // #.##.##.##
    0x9240, // #..#..#..#
    0x9240, // #..#..#..#
    0x9240, // #..#..#..#
    0xFFC0, // ##########
    0x0000, // ..........
};
const uint16_t FAM_CHROM_ROWS[] = {
    0x0000, // ..........
    0x8000, // #.........
    0xA000, // #.#.......
    0xA800, // #.#.#.....
    0xAA00, // #.#.#.#...
    0xAA80, // #.#.#.#.#.
    0xAA80, // #.#.#.#.#.
    0xAA80, // #.#.#.#.#.
    0x0000, // ..........
    0xFFC0, // ##########
};
const uint16_t FAM_ORGAN_ROWS[] = {
    0x0C00, // ....##....
    0x2D00, // ..#.##.#..
    0x2D00, // ..#.##.#..
    0xAD40, // #.#.##.#.#
    0xAD40, // #.#.##.#.#
    0xAD40, // #.#.##.#.#
    0xAD40, // #.#.##.#.#
    0xFFC0, // ##########
    0x8040, // #........#
    0xFFC0, // ##########
};
const uint16_t FAM_GUITAR_ROWS[] = {
    0x0040, // .........#
    0x00C0, // ........##
    0x0100, // .......#..
    0x3200, // ..##..#...
    0x4C00, // .#..##....
    0x9A00, // #..##.#...
    0x9A00, // #..##.#...
    0x8200, // #.....#...
    0x4400, // .#...#....
    0x3800, // ..###.....
};
const uint16_t FAM_BASS_ROWS[] = {
    0x3C00, // ..####....
    0x4200, // .#....#...
    0x8280, // #.....#.#.
    0x8200, // #.....#...
    0x6480, // .##..#..#.
    0x0800, // ....#.....
    0x1000, // ...#......
    0x6000, // .##.......
    0x8000, // #.........
    0x0000, // ..........
};
const uint16_t FAM_STRINGS_ROWS[] = {
    0x0C00, // ....##....
    0x0C00, // ....##....
    0x0C00, // ....##....
    0x3F00, // ..######..
    0x4080, // .#......#.
    0x2D00, // ..#.##.#..
    0x2D00, // ..#.##.#..
    0x4080, // .#......#.
    0x4080, // .#......#.
    0x3F00, // ..######..
};
const uint16_t FAM_ENSEMBLE_ROWS[] = {
    0x0000, // ..........
    0x3F80, // ..#######.
    0x3F80, // ..#######.
    0x2480, // ..#..#..#.
    0x2480, // ..#..#..#.
    0x2480, // ..#..#..#.
    0x2480, // ..#..#..#.
    0x6D80, // .##.##.##.
    0xEEC0, // ###.###.##
    0x0000, // ..........
};
const uint16_t FAM_BRASS_ROWS[] = {
    0x0000, // ..........
    0x1440, // ...#.#...#
    0x14C0, // ...#.#..##
    0xFFC0, // ##########
    0x20C0, // ..#.....##
    0x3E40, // ..#####..#
    0x0000, // ..........
    0x0000, // ..........
    0x0000, // ..........
    0x0000, // ..........
};
const uint16_t FAM_REED_ROWS[] = {
    0x1E00, // ...####...
    0x1000, // ...#......
    0x3000, // ..##......
    0x3000, // ..##......
    0x3000, // ..##......
    0x3000, // ..##......
    0x3100, // ..##...#..
    0x3180, // ..##...##.
    0x1F00, // ...#####..
    0x0000, // ..........
};
const uint16_t FAM_PIPE_ROWS[] = {
    0x00C0, // ........##
    0x01C0, // .......###
    0x0380, // ......###.
    0x0500, // .....#.#..
    0x0E00, // ....###...
    0x1400, // ...#.#....
    0x3800, // ..###.....
    0x7000, // .###......
    0xE000, // ###.......
    0xC000, // ##........
};
const uint16_t FAM_LEAD_ROWS[] = {
    0x0000, // ..........
    0x0000, // ..........
    0x0840, // ....#....#
    0x18C0, // ...##...##
    0x2940, // ..#.#..#.#
    0x4A40, // .#..#.#..#
    0x8C40, // #...##...#
    0x0000, // ..........
    0x0000, // ..........
    0x0000, // ..........
};
const uint16_t FAM_PAD_ROWS[] = {
    0x0000, // ..........
    0x0000, // ..........
    0x6000, // .##.......
    0x9000, // #..#......
    0x0840, // ....#....#
    0x0480, // .....#..#.
    0x0300, // ......##..
    0x0000, // ..........
    0x0000, // ..........
    0x0000, // ..........
};
const uint16_t FAM_FX_ROWS[] = {
    0x0800, // ....#.....
    0x4900, // .#..#..#..
    0x2A00, // ..#.#.#...
    0x1C00, // ...###....
    0xFF80, // #########.
    0x1C00, // ...###....
    0x2A00, // ..#.#.#...
    0x4900, // .#..#..#..
    0x0800, // ....#.....
    0x0000, // ..........
};
const uint16_t FAM_ETHNIC_ROWS[] = {
    0x8000, // #.........
    0x4000, // .#........
    0x2000, // ..#.......
    0x1000, // ...#......
    0x0E00, // ....###...
    0x1F00, // ...#####..
    0x1F00, // ...#####..
    0x1F00, // ...#####..
    0x0E00, // ....###...
    0x0000, // ..........
};
const uint16_t FAM_PERC_ROWS[] = {
    0x0C00, // ....##....
    0x1E00, // ...####...
    0x2100, // ..#....#..
    0x2100, // ..#....#..
    0x2100, // ..#....#..
    0x4080, // .#......#.
    0xFFC0, // ##########
    0x0C00, // ....##....
    0x0000, // ..........
    0x0000, // ..........
};
const uint16_t FAM_SFX_ROWS[] = {
    0x0700, // .....###..
    0x0E00, // ....###...
    0x1C00, // ...###....
    0x3F00, // ..######..
    0x0E00, // ....###...
    0x1C00, // ...###....
    0x3000, // ..##......
    0x4000, // .#........
    0x8000, // #.........
    0x0000, // ..........
};
const uint16_t FAM_DRUMS_ROWS[] = {
    0x8040, // #........#
    0x4080, // .#......#.
    0x2100, // ..#....#..
    0x7F80, // .########.
    0x8040, // #........#
    0xFFC0, // ##########
    0xAAC0, // #.#.#.#.##
    0xAAC0, // #.#.#.#.##
    0x7F80, // .########.
    0x0000, // ..........
};

const uint16_t* const FAMILY_ICONS[17] = {
    FAM_PIANO_ROWS, FAM_CHROM_ROWS, FAM_ORGAN_ROWS, FAM_GUITAR_ROWS, FAM_BASS_ROWS, FAM_STRINGS_ROWS,
    FAM_ENSEMBLE_ROWS, FAM_BRASS_ROWS, FAM_REED_ROWS, FAM_PIPE_ROWS, FAM_LEAD_ROWS, FAM_PAD_ROWS,
    FAM_FX_ROWS, FAM_ETHNIC_ROWS, FAM_PERC_ROWS, FAM_SFX_ROWS, FAM_DRUMS_ROWS,
};

// ---- layout, 320x240 --------------------------------------------------------
const int CELL_Y0 = HEADER_H + 2;
const int CELL_H = 18;
const int CELL_STEP = CELL_H + 1;
const int CELL_W = 154;
const int CELL_X0 = 4, CELL_X1 = SCREEN_W - 4 - CELL_W;
const int NUM_DX = 6;
const int ICON_DX = 19;
const int NAME_DX = 33;
const int CM_SEGS = 8;            // cell meter: 8 segments, 3 px lit, 1 px gap
const int CM_W = CM_SEGS * 4 - 1;
const int CM_DX = CELL_W - 6 - CM_W;
const int CM_DY = 6, CM_H = 6;
const int KEYBED_Y = CELL_Y0 + 8 * CELL_STEP + 1;

int cellX(int ch) { return ch < 8 ? CELL_X0 : CELL_X1; }
int cellY(int ch) { return CELL_Y0 + (ch & 7) * CELL_STEP; }

struct SynthView {
    uint8_t hits[16];
    uint8_t level[16]; // 0-127
    uint8_t segs[16];
    uint16_t activeDrawn;
    bool volumeShown; // channel 9's cell has VOL over it
};
SynthView g_syn;
uint32_t g_synLastMs = 0;

uint16_t cmColor(int ch, int seg) { return ch == 9 ? COLOR_MIDI : meterColor(seg, CM_SEGS); }

void paintCmSeg(TFT_eSPI& g, int ch, int seg, bool lit) {
    int x = cellX(ch) + CM_DX + seg * 4, y = cellY(ch) + CM_DY;
    g.fillRect(x, y, 3, CM_H, lit ? cmColor(ch, seg) : UNLIT);
}

void paintCell(TFT_eSPI& g, int ch, const SynthCell& cell, bool selected, bool active) {
    int x = cellX(ch), y = cellY(ch);
    paintBox(g, x, y, CELL_W, CELL_H, selected);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d", ch + 1);
    uint16_t on = ch == 9 ? COLOR_MIDI : COLOR_ACCENT;
    drawText(g, FONT_TEXT, buf, x + NUM_DX, y + 6, active ? on : COLOR_DIM);
    int fam = cell.family > 16 ? 16 : cell.family;
    Icon icon = {10, 10, FAMILY_ICONS[fam]};
    drawIcon(g, icon, x + ICON_DX, y + 4, active ? on : COLOR_DIM);
    fitText(FONT_TEXT, cell.name ? cell.name : "", CM_DX - 4 - NAME_DX, buf, sizeof(buf));
    drawText(g, FONT_TEXT, buf, x + NAME_DX, y + 6, COLOR_TEXT);
    for (int s = 0; s < CM_SEGS; s++) paintCmSeg(g, ch, s, s < g_syn.segs[ch]);
}

void drawCell(int ch, const SynthCell& cell, bool selected, bool active) {
    if (active) g_syn.activeDrawn |= (uint16_t)(1u << ch);
    else g_syn.activeDrawn &= (uint16_t)~(1u << ch);
    if (ch == 8) g_syn.volumeShown = false;
    band(cellX(ch), cellY(ch), CELL_W, CELL_H, selected ? COLOR_HILITE_BG : COLOR_BG,
         [&](TFT_eSPI& g) { paintCell(g, ch, cell, selected, active); });
}

// A note-on jumps a cell's meter to its velocity, then it falls, holding at
// half while notes are down. As the .mid screen's channel meters.
void updateCellMeters() {
    uint16_t mask = MidiOutput::activeChannelMask();
    TFT_eSPI& g = screen();
    for (int ch = 0; ch < 16; ch++) {
        int lvl = g_syn.level[ch];
        if (UiTap::g_midi.hits[ch] != g_syn.hits[ch]) {
            g_syn.hits[ch] = UiTap::g_midi.hits[ch];
            lvl = UiTap::g_midi.vel[ch];
        } else {
            int floor = ((mask >> ch) & 1) ? UiTap::g_midi.vel[ch] / 2 : 0;
            lvl -= 8;
            if (lvl < floor) lvl = floor;
            if (lvl < 0) lvl = 0;
        }
        g_syn.level[ch] = (uint8_t)lvl;
        int segs = (lvl * CM_SEGS + 126) / 127;
        int old = g_syn.segs[ch];
        if (segs == old) continue;
        g_syn.segs[ch] = (uint8_t)segs;
        if (ch == 8 && g_syn.volumeShown) continue;
        int lo = segs < old ? segs : old, hi = segs < old ? old : segs;
        for (int s = lo; s < hi; s++) paintCmSeg(g, ch, s, s < segs);
    }
}

void drawSynthFooter() {
    Key r1[] = {{"UD", "channel", TONE_TEXT}, {"LR", "column", TONE_TEXT}, {"EDIT", "edit", TONE_SELECT},
                {"PLAY", "pick", TONE_TEXT}, {"ENT", "bank", TONE_TEXT}};
    Key r2[] = {{"EDIT+LR", "inst", TONE_TEXT}, {"EDIT+UD", "vol", TONE_TEXT}, {"NAV", "back", TONE_TEXT}};
    drawKeyFooter(r1, 5, r2, 3);
}

} // namespace

void synthDraw(const SynthCell* cells, int selected, uint16_t activeMask) {
    g_looperUp = false;
    // Later transition steps keep the first step's hit counts, so notes
    // played during the transition still move the meters after it.
    uint8_t hits[16];
    memcpy(hits, Ui::drawPass() > 0 ? g_syn.hits : UiTap::g_midi.hits, sizeof(hits));
    memset(&g_syn, 0, sizeof(g_syn));
    memcpy(g_syn.hits, hits, sizeof(g_syn.hits));
    g_synUp = true;
    Status st;
    st.icon = &ICON_PARISYNTH;
    st.iconColor = COLOR_MIDI;
    st.title = "PARISYNTH";
    drawStatus(st);
    TFT_eSPI& s = screen();
    s.fillRect(0, HEADER_H, SCREEN_W, CELL_Y0 - HEADER_H, COLOR_BG);
    s.fillRect(0, CELL_Y0, CELL_X0, 8 * CELL_STEP, COLOR_BG);
    s.fillRect(CELL_X0 + CELL_W, CELL_Y0, CELL_X1 - CELL_X0 - CELL_W, 8 * CELL_STEP, COLOR_BG);
    s.fillRect(CELL_X1 + CELL_W, CELL_Y0, SCREEN_W - CELL_X1 - CELL_W, 8 * CELL_STEP, COLOR_BG);
    for (int r = 0; r < 8; r++) s.drawFastHLine(0, CELL_Y0 + r * CELL_STEP + CELL_H, SCREEN_W, COLOR_BG);
    for (int ch = 0; ch < 16; ch++) drawCell(ch, cells[ch], ch == selected, (activeMask >> ch) & 1);
    UiPlayer::keybedDraw(KEYBED_Y, BODY_BOTTOM - KEYBED_Y);
    drawSynthFooter();
}

void synthCell(int ch, const SynthCell& cell, bool selected, bool active) {
    if (ch < 0 || ch > 15) return;
    drawCell(ch, cell, selected, active);
}

void synthLive() {
    if (!g_synUp) return;
    uint32_t now = millis();
    if (now - g_synLastMs < LIVE_MS || !Ui::sliceAllows(Ui::DRAW_METER)) return;
    Ui::SliceTimer timer;
    g_synLastMs = now;
    updateCellMeters();
    UiPlayer::keybedUpdate();
}

void synthVolume(int volume) {
    int ch = 8, x = cellX(ch), y = cellY(ch);
    g_syn.volumeShown = true;
    band(x, y, CELL_W, CELL_H, COLOR_HILITE_BG, [&](TFT_eSPI& g) {
        paintBox(g, x, y, CELL_W, CELL_H, true);
        drawText(g, FONT_LABEL, "VOL", x + NUM_DX, y + 7, COLOR_DIM);
        int bx = x + 30, bw = CELL_W - 30 - 34;
        int v = volume < 0 ? 0 : volume > 100 ? 100 : volume;
        g.fillRect(bx, y + 7, bw, 5, UNLIT);
        g.fillRect(bx, y + 7, bw * v / 100, 5, COLOR_ACCENT);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", v);
        drawTextRight(g, FONT_TEXT, buf, x + CELL_W - 6, y + 6, COLOR_TEXT);
    });
}

uint16_t synthDrawnActiveMask() { return g_syn.activeDrawn; }


// =============================================================================
// synth editor
// =============================================================================

namespace {

// ---- picker -------------------------------------------------------------

void pickRow(const char* const* labels, const uint8_t* families, int count, int idx, int scrollOffset, bool selected) {
    int row = idx - scrollOffset;
    if (row < 0 || row >= SYNTH_PICK_ROWS) return;
    int y = LIST_TOP + row * LIST_ROW_H;
    if (idx < 0 || idx >= count) {
        band(0, y, LIST_W, LIST_ROW_H, COLOR_BG, [](TFT_eSPI&) {});
        return;
    }
    listRow(y, LIST_W, selected, [&](TFT_eSPI& g, uint16_t) {
        int fam = families[idx] > 16 ? 16 : families[idx];
        Icon icon = {10, 10, FAMILY_ICONS[fam]};
        drawIcon(g, icon, 10, y + 4, selected ? COLOR_SELECT : (fam == 16 ? COLOR_MIDI : COLOR_DIM));
        char buf[32];
        fitText(FONT_TEXT, labels[idx], LIST_W - 34, buf, sizeof(buf));
        drawText(g, FONT_TEXT, buf, 26, rowTextY(y), COLOR_TEXT);
    });
}

// ---- field editor, 320x240 -------------------------------------------------
const int SE_Y0 = HEADER_H + 4;
const int SE_ROW_W = 200;
const int SE_VALUE_X = 90;
const int SE_BAR_X = 136, SE_BAR_W = 56, SE_BAR_H = 4;
const int SE_PANEL_X = SE_ROW_W + 4, SE_PANEL_W = SCREEN_W - 4 - SE_PANEL_X;
const int SE_ENV_Y = SE_Y0, SE_ENV_H = 98;
const int SE_WAVE_Y = SE_ENV_Y + SE_ENV_H + 4;
const int SE_WAVE_H = SYNTH_EDIT_MAX_ROWS * LIST_ROW_H - SE_ENV_H - 4;
const int SE_GX = SE_PANEL_X + 6, SE_GW = SE_PANEL_W - 12; // graph columns in both boxes
const uint16_t ENV_FILL = rgb(0x1A212B);

bool isEnvPart(SynthEditPart p) { return p >= PART_ATTACK && p <= PART_PITCH; }

void seRow(const SynthEditView& v, int i, bool selected) {
    int y = SE_Y0 + i * LIST_ROW_H;
    if (i < 0 || i >= v.count) {
        band(0, y, SE_ROW_W, LIST_ROW_H, COLOR_BG, [](TFT_eSPI&) {});
        return;
    }
    const SynthEditRow& r = v.rows[i];
    listRow(y, SE_ROW_W, selected, [&](TFT_eSPI& g, uint16_t) {
        int ty = rowTextY(y);
        drawText(g, FONT_TEXT, r.label, 10, ty, COLOR_TEXT);
        drawText(g, FONT_TEXT, r.value, SE_VALUE_X, ty, selected ? COLOR_SELECT : COLOR_DIM);
        if (r.fill >= 0) {
            int by = y + (LIST_ROW_H - SE_BAR_H) / 2;
            int f = r.fill > 1000 ? 1000 : r.fill;
            g.fillRect(SE_BAR_X, by, SE_BAR_W, SE_BAR_H, UNLIT);
            g.fillRect(SE_BAR_X, by, f * SE_BAR_W / 1000, SE_BAR_H, selected ? COLOR_SELECT : COLOR_DIM);
        }
    });
}

// A vertical run joining the last column's y to this one's, so steep edges
// stay unbroken.
void plotCol(TFT_eSPI& g, int x, int prevY, int y, uint16_t c) {
    int lo = prevY < y ? prevY : y, hi = prevY < y ? y : prevY;
    g.drawFastVLine(x, lo, hi - lo + 1, c);
}

int lerp(int x, int x0, int x1, int y0, int y1) {
    if (x1 <= x0) return y1;
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
}

int isqrt(uint32_t v) {
    uint32_t r = 0;
    for (uint32_t b = 1u << 15; b; b >>= 1)
        if ((r + b) * (r + b) <= v) r += b;
    return (int)r;
}

// A melodic envelope segment's width: 8 px, plus up to 22 more on a square
// root scale so short times still show.
int segW(uint16_t samples) { return 8 + isqrt(samples) * 22 / 256; }

void paintMelodicEnv(TFT_eSPI& g, const SynthEditView& v, SynthEditPart focus, int top, int bottom) {
    int gh = bottom - top;
    int a = segW(v.attack), d = segW(v.decay), r = segW(v.release);
    int x0 = SE_GX, x1 = x0 + a, x2 = x1 + d, x4 = SE_GX + SE_GW, x3 = x4 - r;
    int susY = bottom - (v.sustain > 100 ? 100 : v.sustain) * gh / 100;
    const int xs[5] = {x0, x1, x2, x3, x4};
    const int ys[5] = {bottom, top, susY, susY, bottom};
    int f = focus >= PART_ATTACK && focus <= PART_RELEASE ? focus - PART_ATTACK : -1;
    uint16_t focusFill = blend(COLOR_SELECT, COLOR_BG, 56);
    for (int k = 1; k < 4; k++)
        for (int y = top; y < bottom; y += 2) g.drawPixel(xs[k], y, TICK_BAR);
    int prevY = bottom, seg = 0;
    for (int x = x0; x < x4; x++) {
        while (seg < 3 && x >= xs[seg + 1]) seg++;
        int y = lerp(x, xs[seg], xs[seg + 1], ys[seg], ys[seg + 1]);
        if (y < bottom) g.drawFastVLine(x, y + 1, bottom - y, seg == f ? focusFill : ENV_FILL);
        plotCol(g, x, x == x0 ? y : prevY, y, seg == f ? COLOR_SELECT : COLOR_TEXT);
        prevY = y;
    }
    const char* names[4] = {"A", "D", "S", "R"};
    for (int k = 0; k < 4; k++) {
        int cx = (xs[k] + xs[k + 1]) / 2;
        drawTextCentre(g, FONT_LABEL, names[k], cx + 1, bottom + 3, k == f ? COLOR_SELECT : COLOR_DIM);
    }
}

void paintDrumEnv(TFT_eSPI& g, const SynthEditView& v, SynthEditPart focus, int top, int bottom) {
    int gh = bottom - top;
    uint32_t span = v.decay > v.dropTime ? v.decay : v.dropTime;
    if (span < 441) span = 441;
    int ampEnd = SE_GX + (int)((uint32_t)v.decay * SE_GW / span);
    int dropEnd = SE_GX + (int)((uint32_t)v.dropTime * SE_GW / span);
    if (ampEnd <= SE_GX) ampEnd = SE_GX + 1;
    int yFrom = bottom - (v.pitchFrom > 1000 ? 1000 : v.pitchFrom) * gh / 1000;
    int yTo = bottom - (v.pitchTo > 1000 ? 1000 : v.pitchTo) * gh / 1000;
    bool ampF = focus == PART_AMP, pitchF = focus == PART_PITCH;
    uint16_t ampLine = ampF ? COLOR_SELECT : COLOR_TEXT;
    uint16_t pitchLine = pitchF ? COLOR_SELECT : COLOR_MIDI;
    uint16_t fill = ampF ? blend(COLOR_SELECT, COLOR_BG, 56) : ENV_FILL;
    int prevA = top, prevP = yFrom;
    for (int x = SE_GX; x < SE_GX + SE_GW; x++) {
        int ya = x < ampEnd ? lerp(x, SE_GX, ampEnd, top, bottom) : bottom;
        if (ya < bottom) g.drawFastVLine(x, ya + 1, bottom - ya, fill);
        plotCol(g, x, x == SE_GX ? ya : prevA, ya, ampLine);
        prevA = ya;
    }
    // Pitch is drawn while the drum sounds, over the amp line.
    for (int x = SE_GX; x < ampEnd; x++) {
        int yp = x < dropEnd ? lerp(x, SE_GX, dropEnd, yFrom, yTo) : yTo;
        plotCol(g, x, x == SE_GX ? yp : prevP, yp, pitchLine);
        prevP = yp;
    }
}

void seEnv(const SynthEditView& v, SynthEditPart focus) {
    int x = SE_PANEL_X, y = SE_ENV_Y;
    band(x, y, SE_PANEL_W, SE_ENV_H, COLOR_BG, [&](TFT_eSPI& g) {
        g.drawRect(x, y, SE_PANEL_W, SE_ENV_H, COLOR_RULE);
        int top = y + 17, bottom = y + SE_ENV_H - 10;
        g.drawFastHLine(SE_GX, bottom, SE_GW, COLOR_RULE);
        if (v.drum) {
            int lx = drawText(g, FONT_LABEL, "AMP", x + 6, y + 6, focus == PART_AMP ? COLOR_SELECT : COLOR_TEXT);
            drawText(g, FONT_LABEL, "PITCH", lx + 6, y + 6, focus == PART_PITCH ? COLOR_SELECT : COLOR_MIDI);
            paintDrumEnv(g, v, focus, top, bottom);
        } else {
            drawText(g, FONT_LABEL, "ENVELOPE", x + 6, y + 6, COLOR_DIM);
            paintMelodicEnv(g, v, focus, top, bottom);
        }
    });
}

// Two cycles of the wave, -1000 to 1000.
int waveAt(int wave, int x, uint32_t& seed) {
    const int P = SE_GW / 2, H = P / 2;
    int p = x % P;
    switch (wave) {
        case 0: { // sine, as two parabolas
            int q = p < H ? p : p - H;
            int s = 4000 * q * (H - q) / (H * H);
            return p < H ? s : -s;
        }
        case 1: return p < H ? -1000 + p * 2000 / H : 1000 - (p - H) * 2000 / H;
        case 2: return -1000 + p * 2000 / P;
        case 3: return p < H ? 1000 : -1000;
        default:
            if ((x & 1) == 0) seed = seed * 1103515245u + 12345u;
            return (int)((seed >> 16) % 2001) - 1000;
    }
}

void seWave(const SynthEditView& v, SynthEditPart focus) {
    int x = SE_PANEL_X, y = SE_WAVE_Y;
    band(x, y, SE_PANEL_W, SE_WAVE_H, COLOR_BG, [&](TFT_eSPI& g) {
        g.drawRect(x, y, SE_PANEL_W, SE_WAVE_H, COLOR_RULE);
        bool f = focus == PART_WAVE;
        drawText(g, FONT_LABEL, "WAVE", x + 6, y + 6, COLOR_DIM);
        int mid = y + 17 + (SE_WAVE_H - 24) / 2, amp = (SE_WAVE_H - 26) / 2;
        for (int cx = SE_GX; cx < SE_GX + SE_GW; cx += 2) g.drawPixel(cx, mid, TICK_BAR);
        uint32_t seed = 7; // same noise on every draw
        int prevY = mid;
        for (int cx = 0; cx < SE_GW; cx++) {
            int yy = mid - waveAt(v.wave, cx, seed) * amp / 1000;
            plotCol(g, SE_GX + cx, cx == 0 ? yy : prevY, yy, f ? COLOR_SELECT : COLOR_TEXT);
            prevY = yy;
        }
    });
}

SynthEditPart partAt(const SynthEditView& v, int i) { return i >= 0 && i < v.count ? v.rows[i].part : PART_NONE; }

void seFooter() {
    static const Key r1[] = {{"UD", "move", TONE_TEXT}, {"LEFT", "back", TONE_TEXT}};
    static const Key r2[] = {{"EDIT+LR", "change", TONE_SELECT}, {"EDIT+UD", "fast", TONE_TEXT}};
    drawKeyFooter(r1, 2, r2, 2);
}

} // namespace

void synthPickDraw(const char* const* labels, const uint8_t* families, int count, int cursor, int scrollOffset) {
    g_looperUp = false;
    g_synUp = false;
    Status st;
    st.icon = &ICON_PARISYNTH;
    st.iconColor = COLOR_MIDI;
    st.title = "INSTRUMENTS";
    drawStatus(st);
    TFT_eSPI& s = screen();
    s.fillRect(0, HEADER_H, SCREEN_W, LIST_TOP - HEADER_H, COLOR_BG);
    for (int r = 0; r < SYNTH_PICK_ROWS; r++) {
        int idx = scrollOffset + r;
        pickRow(labels, families, count, idx, scrollOffset, idx == cursor);
    }
    int rowsBottom = LIST_TOP + SYNTH_PICK_ROWS * LIST_ROW_H;
    s.fillRect(0, rowsBottom, SCREEN_W, SCREEN_H - FOOTER_H1 - rowsBottom, COLOR_BG); // one-row footer
    drawScrollbar(LIST_TOP, SYNTH_PICK_ROWS, count, scrollOffset);
    static const Key keys[] = {{"UD", "move", TONE_TEXT}, {"RIGHT/ENT", "open", TONE_SELECT}, {"LEFT", "back", TONE_TEXT}};
    drawKeyFooter(keys, 3);
}

void synthPickCursor(const char* const* labels, const uint8_t* families, int count, int prevCursor,
                     int newCursor, int scrollOffset) {
    pickRow(labels, families, count, prevCursor, scrollOffset, false);
    pickRow(labels, families, count, newCursor, scrollOffset, true);
}

void synthEditDraw(const SynthEditView& v, int cursor) {
    g_looperUp = false;
    g_synUp = false;
    int fam = v.family > 16 ? 16 : v.family;
    Icon icon = {10, 10, FAMILY_ICONS[fam]};
    Status st;
    st.icon = &icon;
    st.iconColor = COLOR_MIDI;
    st.title = "EDIT";
    st.textLast = v.name;
    drawStatus(st);
    TFT_eSPI& s = screen();
    int rowsBottom = SE_Y0 + SYNTH_EDIT_MAX_ROWS * LIST_ROW_H;
    s.fillRect(0, HEADER_H, SCREEN_W, SE_Y0 - HEADER_H, COLOR_BG);
    s.fillRect(SE_ROW_W, SE_Y0, SE_PANEL_X - SE_ROW_W, rowsBottom - SE_Y0, COLOR_BG);
    s.fillRect(SE_PANEL_X + SE_PANEL_W, SE_Y0, SCREEN_W - SE_PANEL_X - SE_PANEL_W, rowsBottom - SE_Y0, COLOR_BG);
    s.fillRect(SE_PANEL_X, SE_ENV_Y + SE_ENV_H, SE_PANEL_W, SE_WAVE_Y - SE_ENV_Y - SE_ENV_H, COLOR_BG);
    s.fillRect(0, rowsBottom, SCREEN_W, BODY_BOTTOM - rowsBottom, COLOR_BG);
    for (int i = 0; i < SYNTH_EDIT_MAX_ROWS; i++) seRow(v, i, i == cursor);
    SynthEditPart focus = partAt(v, cursor);
    seEnv(v, focus);
    seWave(v, focus);
    seFooter();
}

void synthEditCursor(const SynthEditView& v, int prevCursor, int newCursor) {
    seRow(v, prevCursor, false);
    seRow(v, newCursor, true);
    SynthEditPart a = partAt(v, prevCursor), b = partAt(v, newCursor);
    if (a == b) return;
    if (isEnvPart(a) || isEnvPart(b)) seEnv(v, b);
    if (a == PART_WAVE || b == PART_WAVE) seWave(v, b);
}

void synthEditValue(const SynthEditView& v, int idx) {
    seRow(v, idx, true);
    SynthEditPart p = partAt(v, idx);
    if (isEnvPart(p)) seEnv(v, p);
    else if (p == PART_WAVE) seWave(v, p);
}

// =============================================================================
// synth editor bank screens
// =============================================================================

namespace {

const uint16_t BANK_SAVE_ROWS[] = {
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x36C0, // ..##.##.##..
    0x1F80, // ...######...
    0x0F00, // ....####....
    0x0600, // .....##.....
    0x8010, // #..........#
    0x8010, // #..........#
    0xFFF0, // ############
    0x0000, // ............
};
const uint16_t BANK_LOAD_ROWS[] = {
    0x0600, // .....##.....
    0x0F00, // ....####....
    0x1F80, // ...######...
    0x36C0, // ..##.##.##..
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x0600, // .....##.....
    0x8010, // #..........#
    0x8010, // #..........#
    0xFFF0, // ############
    0x0000, // ............
};
const uint16_t BANK_RESET_ROWS[] = {
    0x0000, // ............
    0x1000, // ...#........
    0x3000, // ..##........
    0x7F80, // .########...
    0x3040, // ..##.....#..
    0x1020, // ...#......#.
    0x0020, // ..........#.
    0x0020, // ..........#.
    0x0040, // .........#..
    0x3F80, // ..#######...
    0x0000, // ............
    0x0000, // ............
};
const Icon BANK_ICONS[SYNTH_BANK_ITEMS] = {
    {12, 12, BANK_SAVE_ROWS}, {12, 12, BANK_LOAD_ROWS}, {12, 12, BANK_RESET_ROWS}};
const char* const BANK_LABELS[SYNTH_BANK_ITEMS] = {"SAVE BANK", "LOAD BANK", "RESET"};
const char* const BANK_NOTES[SYNTH_BANK_ITEMS] = {"write to /synth", "from /synth", "all sounds back to built-in"};

void bankStatus(const char* title, const char* bankName) {
    g_looperUp = false;
    g_synUp = false;
    Status st;
    st.icon = &ICON_PARISYNTH;
    st.iconColor = COLOR_MIDI;
    st.title = title;
    if (bankName) {
        if (bankName[0]) st.textLast = bankName;
        else st.text = "built-in";
    }
    drawStatus(st);
}

void bankRow(int i, bool selected) {
    if (i < 0 || i >= SYNTH_BANK_ITEMS) return;
    uint16_t c = i == 0 ? COLOR_ACCENT : i == 1 ? COLOR_SELECT : COLOR_DIM;
    drawIconRow(i, BANK_ICONS[i], c, BANK_LABELS[i], BANK_NOTES[i], selected);
}

// ---- load list ---------------------------------------------------------------

void loadRow(const char* const* names, int count, int idx, int scrollOffset, const char* bankName, bool selected) {
    int row = idx - scrollOffset;
    if (row < 0 || row >= SYNTH_PICK_ROWS) return;
    int y = LIST_TOP + row * LIST_ROW_H;
    if (idx < 0 || idx >= count) {
        band(0, y, LIST_W, LIST_ROW_H, COLOR_BG, [](TFT_eSPI&) {});
        return;
    }
    bool loaded = bankName && bankName[0] && strcmp(names[idx], bankName) == 0;
    listRow(y, LIST_W, selected, [&](TFT_eSPI& g, uint16_t) {
        int right = LIST_W - 8;
        if (loaded) {
            right -= textWidth(FONT_LABEL, "LOADED");
            drawText(g, FONT_LABEL, "LOADED", right, y + (LIST_ROW_H - 5) / 2, COLOR_ACCENT);
            right -= 8;
        }
        char buf[32];
        fitText(FONT_TEXT, names[idx], right - 10, buf, sizeof(buf));
        drawText(g, FONT_TEXT, buf, 10, rowTextY(y), COLOR_TEXT);
    });
}

// ---- name entry ----------------------------------------------------------------
const int NM_LABEL_Y = HEADER_H + 5;
const int NM_CELL_Y = NM_LABEL_Y + 11, NM_CELL_H = 26;
const int NM_CELL_W = 16, NM_PITCH = 18;
const int NM_W = SYNTH_NAME_MAX * NM_PITCH - (NM_PITCH - NM_CELL_W);
const int NM_X0 = (SCREEN_W - NM_W) / 2;
const int KB_Y0 = NM_CELL_Y + NM_CELL_H + 12;
const int KB_KEY_H = 25, KB_STEP = 28;
const int KB_PITCH = 30, KB_WIDE_PITCH = 100;

void nameLabel(const char* name, const char* error) {
    band(0, NM_LABEL_Y - 1, SCREEN_W, 9, COLOR_BG, [&](TFT_eSPI& g) {
        drawText(g, FONT_LABEL, "NAME", NM_X0, NM_LABEL_Y, COLOR_DIM);
        if (error) {
            drawTextRight(g, FONT_TEXT, error, NM_X0 + NM_W, NM_LABEL_Y - 1, COLOR_ERROR);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d/%d", (int)strlen(name), SYNTH_NAME_MAX);
            drawTextRight(g, FONT_LABEL, buf, NM_X0 + NM_W, NM_LABEL_Y, COLOR_DIM);
        }
    });
}

// Cell i: a character, the cursor (the next one to type) or an empty slot.
void nameCell(const char* name, int i) {
    if (i < 0 || i >= SYNTH_NAME_MAX) return;
    int len = (int)strlen(name);
    int x = NM_X0 + i * NM_PITCH, y = NM_CELL_Y;
    bool cursor = i == len;
    band(x, y, NM_CELL_W, NM_CELL_H, cursor ? COLOR_HILITE_BG : COLOR_SURFACE, [&](TFT_eSPI& g) {
        if (cursor) {
            g.drawRect(x, y, NM_CELL_W, NM_CELL_H, COLOR_SELECT);
            g.fillRect(x + 3, y + NM_CELL_H - 5, NM_CELL_W - 6, 2, COLOR_SELECT);
        } else if (i < len) {
            char c[2] = {name[i], '\0'};
            drawTextCentre(g, FONT_TEXT, c, x + NM_CELL_W / 2, y + (NM_CELL_H - 14) / 2, COLOR_TEXT, 2);
        } else {
            g.fillRect(x + 4, y + NM_CELL_H - 5, NM_CELL_W - 8, 1, COLOR_RULE);
        }
    });
}

void keyRect(int row, int col, int& x, int& y, int& w) {
    int pitch = row == KEYBOARD_ROW_COUNT - 1 ? KB_WIDE_PITCH : KB_PITCH;
    x = (SCREEN_W - KEYBOARD_ROW_LENS[row] * pitch) / 2 + col * pitch + 1;
    y = KB_Y0 + row * KB_STEP;
    w = pitch - 2;
}

void nameKey(int row, int col, bool selected) {
    if (row < 0 || row >= KEYBOARD_ROW_COUNT || col < 0 || col >= KEYBOARD_ROW_LENS[row]) return;
    int x, y, w;
    keyRect(row, col, x, y, w);
    const KeyDef& k = KEYBOARD_ROWS[row][col];
    band(x, y, w, KB_KEY_H, selected ? COLOR_HILITE_BG : COLOR_SURFACE, [&](TFT_eSPI& g) {
        g.drawRect(x, y, w, KB_KEY_H, selected ? COLOR_SELECT : COLOR_RULE);
        if (k.kind == KEY_CHAR && k.ch != ' ') {
            drawTextCentre(g, FONT_TEXT, k.label, x + w / 2 + 1, y + (KB_KEY_H - 14) / 2, selected ? COLOR_SELECT : COLOR_TEXT, 2);
        } else {
            uint16_t c = selected ? COLOR_SELECT : k.kind == KEY_OK ? COLOR_ACCENT : k.kind == KEY_DEL ? COLOR_ERROR : COLOR_DIM;
            drawTextCentre(g, FONT_LABEL, k.label, x + w / 2 + 1, y + (KB_KEY_H - 10) / 2, c, 2);
        }
    });
}

} // namespace

void synthBankDraw(const char* bankName, int cursor) {
    bankStatus("BANK", bankName);
    clearModeList();
    for (int i = 0; i < 4; i++) {
        if (i < SYNTH_BANK_ITEMS) bankRow(i, i == cursor);
        else clearModeRow(i);
    }
    static const Key keys[] = {{"UD", "move", TONE_TEXT}, {"ENT", "open", TONE_SELECT}, {"LEFT", "back", TONE_TEXT}};
    drawKeyFooter(keys, 3);
}

void synthBankCursor(int prevCursor, int newCursor) {
    bankRow(prevCursor, false);
    bankRow(newCursor, true);
}

void synthLoadDraw(const char* const* names, int count, int cursor, int scrollOffset, const char* bankName) {
    bankStatus("LOAD BANK", bankName);
    TFT_eSPI& s = screen();
    s.fillRect(0, HEADER_H, SCREEN_W, LIST_TOP - HEADER_H, COLOR_BG);
    for (int r = 0; r < SYNTH_PICK_ROWS; r++) {
        int idx = scrollOffset + r;
        loadRow(names, count, idx, scrollOffset, bankName, idx == cursor);
    }
    int rowsBottom = LIST_TOP + SYNTH_PICK_ROWS * LIST_ROW_H;
    s.fillRect(0, rowsBottom, SCREEN_W, SCREEN_H - FOOTER_H1 - rowsBottom, COLOR_BG);
    drawScrollbar(LIST_TOP, SYNTH_PICK_ROWS, count, scrollOffset);
    if (count > 0) {
        static const Key keys[] = {{"UD", "move", TONE_TEXT}, {"ENT", "load", TONE_SELECT}, {"LEFT", "back", TONE_TEXT}};
        drawKeyFooter(keys, 3);
        return;
    }
    static const Key back[] = {{"LEFT", "back", TONE_TEXT}};
    drawKeyFooter(back, 1);
    Card c;
    c.title = "NO SAVED BANKS";
    c.line1 = "nothing in /synth";
    drawCard(c);
}

void synthLoadCursor(const char* const* names, int count, int prevCursor, int newCursor, int scrollOffset,
                     const char* bankName) {
    loadRow(names, count, prevCursor, scrollOffset, bankName, false);
    loadRow(names, count, newCursor, scrollOffset, bankName, true);
}

void synthNameDraw(const char* name, const char* error, int keyRow, int keyCol) {
    bankStatus("SAVE BANK", nullptr);
    TFT_eSPI& s = screen();
    int bodyBottom = SCREEN_H - FOOTER_H1;
    s.fillRect(0, HEADER_H, SCREEN_W, NM_LABEL_Y - 1 - HEADER_H, COLOR_BG);
    s.fillRect(0, NM_LABEL_Y + 8, SCREEN_W, NM_CELL_Y - NM_LABEL_Y - 8, COLOR_BG);
    nameLabel(name, error);
    s.fillRect(0, NM_CELL_Y, NM_X0, NM_CELL_H, COLOR_BG);
    s.fillRect(NM_X0 + NM_W, NM_CELL_Y, SCREEN_W - NM_X0 - NM_W, NM_CELL_H, COLOR_BG);
    for (int i = 0; i < SYNTH_NAME_MAX; i++) {
        if (i < SYNTH_NAME_MAX - 1) s.fillRect(NM_X0 + i * NM_PITCH + NM_CELL_W, NM_CELL_Y, NM_PITCH - NM_CELL_W, NM_CELL_H, COLOR_BG);
        nameCell(name, i);
    }
    s.fillRect(0, NM_CELL_Y + NM_CELL_H, SCREEN_W, KB_Y0 - NM_CELL_Y - NM_CELL_H, COLOR_BG);
    for (int r = 0; r < KEYBOARD_ROW_COUNT; r++) {
        s.fillRect(0, KB_Y0 + r * KB_STEP, SCREEN_W, KB_STEP, COLOR_BG);
        for (int c = 0; c < KEYBOARD_ROW_LENS[r]; c++) nameKey(r, c, r == keyRow && c == keyCol);
    }
    int kbBottom = KB_Y0 + KEYBOARD_ROW_COUNT * KB_STEP;
    if (bodyBottom > kbBottom) s.fillRect(0, kbBottom, SCREEN_W, bodyBottom - kbBottom, COLOR_BG);
    static const Key keys[] = {{"ARROWS", "move", TONE_TEXT}, {"ENT", "type", TONE_SELECT}, {"NAV", "cancel", TONE_TEXT}};
    drawKeyFooter(keys, 3);
}

void synthNameKey(int prevRow, int prevCol, int newRow, int newCol) {
    nameKey(prevRow, prevCol, false);
    nameKey(newRow, newCol, true);
}

void synthNameText(const char* name, int prevLen) {
    int len = (int)strlen(name);
    int lo = prevLen < len ? prevLen : len, hi = prevLen < len ? len : prevLen;
    for (int i = lo; i <= hi; i++) nameCell(name, i);
    nameLabel(name, nullptr);
}

void synthNameError(const char* name, const char* error) { nameLabel(name, error); }

void synthOverwriteCard(const char* name) {
    Card c;
    c.icon = &ICON_WARNING;
    c.border = COLOR_ACCENT;
    c.title = "OVERWRITE BANK";
    c.line1 = name;
    c.line2 = "this can't be undone";
    static const Key keys[] = {{"NAV", "cancel", TONE_TEXT}, {"ENT", "overwrite", TONE_PLAY}};
    c.buttons = keys;
    c.buttonCount = 2;
    dimBody();
    drawCard(c);
}

void synthResultCard(const char* title, const char* line, bool ok) {
    Card c;
    c.title = title;
    c.line1 = line && line[0] ? line : nullptr;
    if (ok) {
        c.border = COLOR_SELECT;
    } else {
        c.icon = &ICON_WARNING;
        c.border = COLOR_ERROR;
    }
    dimBody();
    drawCard(c);
}

} // namespace UiModes

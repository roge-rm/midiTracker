#include "ui_rec.h"
#include "ui_look.h"
#include "ui_strip.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

using namespace Look;

namespace UiRec {
namespace {

// ---- layout, 320x240 ------------------------------------------------------
const int X0 = 6; // same as the status strip's padding
const int W = SCREEN_W - 2 * X0;
const int XR = X0 + W;
const int BODY_BOTTOM = SCREEN_H - FOOTER_H1;

const int TITLE_Y = 21;   // name at 2x, as the player screens
const int TOP_Y = 40;     // big time at 4x, caps 28 px
const int TOP_H = 28;
const int TIME_W = 130;   // room for "100:00"
const int COL_A = 140;    // count label and 2x value beside the time
const int COL_B = 232;
const int COL_W = 88;
const int VALUE_Y = TOP_Y + 14;

const int RULE1_Y = 72;
const int STATE_Y = 73;   // dot, state label, dropped count
const int STATE_H = 15;
const int RULE2_Y = STATE_Y + STATE_H;

// Recording: the MIDI-in log.
const int LOG_LABEL_Y = RULE2_Y + 6;
const int LOG_Y = LOG_LABEL_Y + 11;
const int LOG_ROW_H = 14;

// Send: progress line, bar, then skipped and junk.
const int PROG_Y = RULE2_Y + 6;
const int BAR_Y = PROG_Y + 13;
const int BAR_H = 8;
const int EXTRA_Y = BAR_Y + BAR_H + 10;
const int EXTRA_H = 12;

const int ERR_LINES = 4;

// Colours from the mockups that aren't theme entries.
const uint16_t UNLIT = rgb(0x1A212B);
const uint16_t REC_QUIET = rgb(0x7A1E1A); // recording, nothing arriving

enum Screen : uint8_t { SCREEN_NONE, SCREEN_REC, SCREEN_SEND, SCREEN_CONV };
Screen g_screen = SCREEN_NONE;

// What each live field shows now, so a tick repaints only changes.
Kind g_kind = KIND_MIDI;
char g_time[12];
char g_a[16], g_b[16];
int8_t g_dot = -1;
uint32_t g_dropped = 0;
uint32_t g_logHash[LOG_ROWS];
bool g_logStale = false;
char g_prog[48];
int g_fill = -1;
bool g_head = false;
uint32_t g_skipped = 0, g_junk = 0;

// ---- small helpers ---------------------------------------------------------

// "12:34", or "17:02:03" from an hour on.
void formatTime(uint32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else
        snprintf(out, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void drawTextRight(TFT_eSPI& g, const PixFont& f, const char* s, int xr, int y, uint16_t c, int scale = 1) {
    drawText(g, f, s, xr - textWidth(f, s, scale), y, c, scale);
}

// Exact below 100000, then "212K", "3.1M".
void formatBytes(uint32_t b, char* out, size_t n) {
    if (b < 100000) snprintf(out, n, "%lu", (unsigned long)b);
    else formatSize(b, out, n);
}

uint32_t hashText(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}

// A 7x7 round dot, hollow for waiting.
void paintDot(TFT_eSPI& g, int x, int y, uint16_t c, bool hollow, uint16_t bg) {
    g.fillRect(x + 2, y, 3, 7, c);
    g.fillRect(x, y + 2, 7, 3, c);
    g.fillRect(x + 1, y + 1, 5, 5, c);
    if (hollow) g.fillRect(x + 2, y + 2, 3, 3, bg);
}

// Name at 2x on the left, an info label on the right.
void paintTitle(TFT_eSPI& g, const char* name, const char* info, uint16_t infoColor) {
    int infoW = info ? textWidth(FONT_LABEL, info) : 0;
    if (info) drawText(g, FONT_LABEL, info, XR - infoW, TITLE_Y + 7, infoColor);
    char buf[64];
    fitText(FONT_TEXT, name ? name : "", (W - infoW - 10) / 2, buf, sizeof(buf));
    drawText(g, FONT_TEXT, buf, X0, TITLE_Y, COLOR_TEXT, 2);
}

void drawTitle(const char* name, const char* info, uint16_t infoColor) {
    band(0, HEADER_H, SCREEN_W, TOP_Y - 2 - HEADER_H, COLOR_BG,
         [&](TFT_eSPI& g) { paintTitle(g, name, info, infoColor); });
}

void drawStatusFor(const Icon& icon, uint16_t iconColor, const char* title) {
    Status st;
    st.icon = &icon;
    st.iconColor = iconColor;
    st.title = title;
    drawStatus(st);
}

// The error heading in red and the message under it at 2x, wrapped at
// spaces to ERR_LINES lines.
void paintError(TFT_eSPI& g, const char* heading, const char* msg) {
    drawText(g, FONT_LABEL, heading, X0, TOP_Y + 2, COLOR_ERROR);
    const char* p = msg ? msg : "";
    int y = TOP_Y + 16;
    for (int line = 0; line < ERR_LINES && *p; line++) {
        while (*p == ' ') p++;
        // Longest run of whole words that fits, or a cut word if none does.
        char buf[64];
        size_t n = 0, lastFit = 0;
        while (p[n] && n + 1 < sizeof(buf)) {
            size_t end = n;
            while (p[end] && p[end] != ' ' && end + 1 < sizeof(buf)) end++;
            if (end == n && p[n] != ' ') break; // buf is full
            memcpy(buf, p, end);
            buf[end] = 0;
            if (textWidth(FONT_TEXT, buf, 2) > W) break;
            lastFit = end;
            n = end;
            if (p[n] == ' ') n++;
        }
        bool last = line == ERR_LINES - 1;
        if (lastFit == 0 || (last && p[lastFit])) {
            fitText(FONT_TEXT, p, W / 2, buf, sizeof(buf));
            drawText(g, FONT_TEXT, buf, X0, y, COLOR_ERROR, 2);
            break;
        }
        memcpy(buf, p, lastFit);
        buf[lastFit] = 0;
        drawText(g, FONT_TEXT, buf, X0, y, COLOR_ERROR, 2);
        p += lastFit;
        y += 17;
    }
}

void drawErrorBody(const char* heading, const char* msg) {
    band(0, TOP_Y - 2, SCREEN_W, BODY_BOTTOM - (TOP_Y - 2), COLOR_BG,
         [&](TFT_eSPI& g) { paintError(g, heading, msg); });
}

// 4x, or smaller when it won't fit TIME_W (past an hour), on the same
// baseline.
void paintTime(TFT_eSPI& g, const char* t, uint16_t c) {
    int scale = 4;
    while (scale > 1 && textWidth(FONT_TEXT, t, scale) > TIME_W) scale--;
    drawText(g, FONT_TEXT, t, X0, TOP_Y + TOP_H - FONT_TEXT.cap * scale, c, scale);
}

void drawTime(const char* t, uint16_t c) {
    band(X0, TOP_Y, TIME_W, TOP_H, COLOR_BG, [&](TFT_eSPI& g) { paintTime(g, t, c); });
}

// A count beside the time: label, value at 2x under it.
void paintCount(TFT_eSPI& g, int x, const char* label, const char* value, uint16_t c) {
    drawText(g, FONT_LABEL, label, x, TOP_Y + 2, COLOR_DIM);
    drawText(g, FONT_TEXT, value, x, VALUE_Y, c, 2);
}

void drawCountValue(int x, const char* value) {
    band(x, VALUE_Y, COL_W, TOP_Y + TOP_H - VALUE_Y, COLOR_BG,
         [&](TFT_eSPI& g) { drawText(g, FONT_TEXT, value, x, VALUE_Y, COLOR_TEXT, 2); });
}

// ---- recording -------------------------------------------------------------

uint16_t recTimeColor(RecState s) { return s == REC_RECORDING ? COLOR_ERROR : COLOR_DIM; }

int recDotState(const RecInfo& r) {
    if (r.state != REC_RECORDING) return 0;
    return r.active ? 2 : 1;
}

const char* recStateLabel(const RecInfo& r) {
    bool midi = r.kind == KIND_MIDI;
    switch (r.state) {
        case REC_WAITING:   return midi ? "WAITING FOR MIDI" : "WAITING FOR SYSEX";
        case REC_RECORDING: return midi ? "RECORDING" : "CAPTURING";
        case REC_ERROR:     return "ERROR";
        default:            return "IDLE";
    }
}

void paintRecState(TFT_eSPI& g, const RecInfo& r, int dot) {
    int ty = STATE_Y + (STATE_H - 5) / 2;
    uint16_t c = r.state == REC_RECORDING ? COLOR_ERROR : COLOR_DIM;
    uint16_t dc = dot == 2 ? COLOR_ERROR : dot == 1 ? REC_QUIET : COLOR_DIM;
    paintDot(g, X0, STATE_Y + (STATE_H - 7) / 2, dc, dot == 0, COLOR_BG);
    drawText(g, FONT_LABEL, recStateLabel(r), X0 + 12, ty, c);
    if (r.dropped) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lu DROPPED", (unsigned long)r.dropped);
        drawTextRight(g, FONT_LABEL, buf, XR, ty, COLOR_ERROR);
    }
}

void drawRecState(const RecInfo& r, int dot) {
    band(0, STATE_Y, SCREEN_W, STATE_H, COLOR_BG, [&](TFT_eSPI& g) { paintRecState(g, r, dot); });
}

// Just the dot, for an activity blip.
void drawRecDot(int dot) {
    int y = STATE_Y + (STATE_H - 7) / 2;
    band(X0, y, 7, 7, COLOR_BG, [&](TFT_eSPI& g) {
        uint16_t dc = dot == 2 ? COLOR_ERROR : dot == 1 ? REC_QUIET : COLOR_DIM;
        paintDot(g, X0, y, dc, dot == 0, COLOR_BG);
    });
}

void recCounts(const RecInfo& r, char* a, char* b) {
    snprintf(a, 16, "%lu", (unsigned long)r.count);
    if (r.kind == KIND_SYSEX) formatBytes(r.bytes, b, 16);
    else b[0] = 0;
}

void paintLogRow(TFT_eSPI& g, int i, const char* text, bool newest) {
    char buf[64];
    fitText(FONT_TEXT, text, W, buf, sizeof(buf));
    drawText(g, FONT_TEXT, buf, X0, LOG_Y + i * LOG_ROW_H + 3, newest ? COLOR_MIDI : COLOR_DIM);
}

// Row i of the log: newest at the bottom row in use, rows above empty
// until the log fills.
const char* logLine(const char* const* log, int count, int i, bool& newest) {
    int shown = count < 0 ? 0 : count > LOG_ROWS ? LOG_ROWS : count;
    int start = count - shown;
    newest = i == shown - 1;
    return i < shown ? log[start + i] : nullptr;
}

void paintRec(TFT_eSPI& g, const RecInfo& r, const char* const* log, int logCount) {
    char a[16], b[16];
    recCounts(r, a, b);
    strcpy(g_a, a);
    strcpy(g_b, b);
    paintTime(g, g_time, recTimeColor(r.state));
    paintCount(g, COL_A, r.kind == KIND_MIDI ? "EVENTS" : "MESSAGES", a, COLOR_TEXT);
    if (r.kind == KIND_SYSEX) paintCount(g, COL_B, "BYTES", b, COLOR_TEXT);
    g.drawFastHLine(X0, RULE1_Y, W, COLOR_RULE);
    paintRecState(g, r, g_dot);
    g.drawFastHLine(X0, RULE2_Y, W, COLOR_RULE);
    drawText(g, FONT_LABEL, r.kind == KIND_MIDI ? "MIDI IN" : "SYSEX IN", X0, LOG_LABEL_Y, COLOR_MIDI);
    for (int i = 0; i < LOG_ROWS; i++) {
        bool newest;
        const char* line = logLine(log, logCount, i, newest);
        g_logHash[i] = line ? hashText(line) ^ (newest ? 0x80000000u : 0) : 0;
        if (line) paintLogRow(g, i, line, newest);
    }
}

// ---- send ------------------------------------------------------------------

uint16_t sendTimeColor(SendState s) {
    return s == SEND_SENDING ? COLOR_ACCENT : s == SEND_DONE ? COLOR_TEXT : COLOR_DIM;
}

void paintSendState(TFT_eSPI& g, SendState s) {
    int ty = STATE_Y + (STATE_H - 5) / 2;
    switch (s) {
        case SEND_SENDING:
            drawIcon(g, ICON_PLAY, X0, STATE_Y + (STATE_H - 7) / 2, COLOR_ACCENT);
            drawText(g, FONT_LABEL, "SENDING", X0 + 12, ty, COLOR_ACCENT);
            break;
        case SEND_DONE:
            g.fillRect(X0, STATE_Y + (STATE_H - 7) / 2, 7, 7, COLOR_TEXT);
            drawText(g, FONT_LABEL, "FINISHED", X0 + 12, ty, COLOR_TEXT);
            break;
        default:
            drawText(g, FONT_LABEL, "IDLE", X0 + 12, ty, COLOR_DIM);
            break;
    }
}

void sendProgText(const SendInfo& s, char* out, size_t n) {
    int pct = s.totalBytes ? (int)(((uint64_t)s.bytesSent * 100) / s.totalBytes) : 0;
    snprintf(out, n, "%lu / %lu bytes %s %d%%", (unsigned long)s.bytesSent, (unsigned long)s.totalBytes, MIDDOT, pct);
}

int sendFill(const SendInfo& s) {
    return s.totalBytes ? (int)(((uint64_t)W * s.bytesSent) / s.totalBytes) : 0;
}

void paintProgLine(TFT_eSPI& g, const char* text) {
    drawText(g, FONT_LABEL, "SENT", X0, PROG_Y + 1, COLOR_DIM);
    drawTextRight(g, FONT_TEXT, text, XR, PROG_Y, COLOR_TEXT);
}

// Purple for what's gone out, an amber head while bytes are going out.
void paintBar(TFT_eSPI& g, int fill, bool head) {
    g.fillRect(X0, BAR_Y, W, BAR_H, UNLIT);
    if (fill > 0) g.fillRect(X0, BAR_Y, fill, BAR_H, COLOR_MIDI);
    if (head && fill > 0) {
        int hx = fill >= 2 ? X0 + fill - 2 : X0;
        g.fillRect(hx, BAR_Y, 2, BAR_H, COLOR_ACCENT);
    }
}

// "SKIPPED 2   JUNK BYTES 5", each only when not 0.
void paintExtra(TFT_eSPI& g, uint32_t skipped, uint32_t junk) {
    char buf[16];
    int x = X0;
    if (skipped) {
        x = drawText(g, FONT_LABEL, "SKIPPED", x, EXTRA_Y + 3, COLOR_DIM) + 4;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)skipped);
        x = drawText(g, FONT_TEXT, buf, x, EXTRA_Y + 2, COLOR_ACCENT) + 14;
    }
    if (junk) {
        x = drawText(g, FONT_LABEL, "JUNK BYTES", x, EXTRA_Y + 3, COLOR_DIM) + 4;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)junk);
        drawText(g, FONT_TEXT, buf, x, EXTRA_Y + 2, COLOR_ACCENT);
    }
}

void drawSendFooter(SendState s) {
    Key k[] = {{"NAV/LEFT", s == SEND_SENDING ? "stop" : "back", TONE_TEXT}};
    drawKeyFooter(k, 1);
}

// ---- convert ----------------------------------------------------------------

uint16_t convTimeColor(ConvState s) {
    return s == CONV_RUNNING ? COLOR_ACCENT : s == CONV_DONE ? COLOR_TEXT : COLOR_DIM;
}

void convLeftText(const ConvInfo& c, char* out, size_t n) {
    if (c.state != CONV_RUNNING) snprintf(out, n, "-");
    else if (c.leftMs == UINT32_MAX) snprintf(out, n, "--:--");
    else formatTime(c.leftMs + 999, out, n); // rounded up, so it ends on 00:01
}

void convPctText(const ConvInfo& c, char* out, size_t n) {
    int p = c.permille < 0 ? 0 : c.permille > 1000 ? 1000 : c.permille;
    snprintf(out, n, "%d%%", p / 10);
}

int convFill(const ConvInfo& c) {
    int p = c.permille < 0 ? 0 : c.permille > 1000 ? 1000 : c.permille;
    return W * p / 1000;
}

void paintConvState(TFT_eSPI& g, const ConvInfo& c) {
    int ty = STATE_Y + (STATE_H - 5) / 2;
    int iy = STATE_Y + (STATE_H - 7) / 2;
    switch (c.state) {
        case CONV_RUNNING:
            drawIcon(g, ICON_PLAY, X0, iy, COLOR_ACCENT);
            drawText(g, FONT_LABEL, "CONVERTING", X0 + 12, ty, COLOR_ACCENT);
            break;
        case CONV_DONE:
            g.fillRect(X0, iy, 7, 7, COLOR_TEXT);
            drawText(g, FONT_LABEL, "FINISHED", X0 + 12, ty, COLOR_TEXT);
            break;
        default: {
            uint16_t col = c.state == CONV_FAILED ? COLOR_ERROR : COLOR_DIM;
            g.fillRect(X0, iy, 7, 7, col);
            char buf[48];
            fitText(FONT_TEXT, c.message ? c.message : "", W - 12, buf, sizeof(buf));
            drawText(g, FONT_TEXT, buf, X0 + 12, STATE_Y + 2, col);
            break;
        }
    }
}

// "TO" and the new file's name over the bar.
void paintConvOut(TFT_eSPI& g, const ConvInfo& c) {
    int x = drawText(g, FONT_LABEL, "TO", X0, PROG_Y + 1, COLOR_DIM) + 6;
    char buf[64];
    fitText(FONT_TEXT, c.outName ? c.outName : "", XR - x, buf, sizeof(buf));
    drawTextRight(g, FONT_TEXT, buf, XR, PROG_Y, COLOR_TEXT);
}

// Green for what's done, an amber head while it runs.
void paintConvBar(TFT_eSPI& g, int fill, bool head) {
    g.fillRect(X0, BAR_Y, W, BAR_H, UNLIT);
    if (fill > 0) g.fillRect(X0, BAR_Y, fill, BAR_H, COLOR_WAV);
    if (head && fill > 0) {
        int hx = fill >= 2 ? X0 + fill - 2 : X0;
        g.fillRect(hx, BAR_Y, 2, BAR_H, COLOR_ACCENT);
    }
}

void drawConvFooter(ConvState s) {
    if (s == CONV_RUNNING) {
        static const Key k[] = {{"NAV/LEFT", "cancel", TONE_ERROR}};
        drawKeyFooter(k, 1);
    } else {
        static const Key k[] = {{"NAV/LEFT", "back", TONE_TEXT}};
        drawKeyFooter(k, 1);
    }
}

} // namespace

// ---- recording and capture ---------------------------------------------------

void recDraw(const RecInfo& r, const char* const* log, int logCount) {
    g_screen = SCREEN_REC;
    g_kind = r.kind;
    bool midi = r.kind == KIND_MIDI;
    drawStatusFor(ICON_REC, COLOR_ERROR, midi ? "RECORD" : "SYSEX CAPTURE");
    drawTitle(r.name, midi ? "MIDI" : "SYSEX", COLOR_MIDI);
    if (r.state == REC_ERROR) {
        drawErrorBody(midi ? "RECORDING ERROR" : "CAPTURE ERROR", r.error);
        static const Key back[] = {{"NAV/EDIT", "back", TONE_TEXT}};
        drawKeyFooter(back, 1); // whatever was saved stays
        return;
    }
    formatTime(r.elapsedMs, g_time, sizeof(g_time));
    g_dot = (int8_t)recDotState(r);
    g_dropped = r.dropped;
    band(0, TOP_Y - 2, SCREEN_W, BODY_BOTTOM - (TOP_Y - 2), COLOR_BG,
         [&](TFT_eSPI& g) { paintRec(g, r, log, logCount); });
    static const Key keys[] = {{"NAV/LEFT", "stop & save", TONE_TEXT}, {"EDIT", "discard", TONE_ERROR}};
    drawKeyFooter(keys, 2);
}

void recLive(const RecInfo& r) {
    if (g_screen != SCREEN_REC || r.state == REC_ERROR) return;
    char t[12];
    formatTime(r.elapsedMs, t, sizeof(t));
    if (strcmp(t, g_time) != 0) {
        strcpy(g_time, t);
        drawTime(t, recTimeColor(r.state));
    }
    char a[16], b[16];
    recCounts(r, a, b);
    if (strcmp(a, g_a) != 0) {
        strcpy(g_a, a);
        drawCountValue(COL_A, a);
    }
    if (strcmp(b, g_b) != 0) {
        strcpy(g_b, b);
        drawCountValue(COL_B, b);
    }
    int dot = recDotState(r);
    if (r.dropped != g_dropped) {
        g_dropped = r.dropped;
        g_dot = (int8_t)dot;
        drawRecState(r, dot);
    } else if (dot != g_dot) {
        g_dot = (int8_t)dot;
        drawRecDot(dot);
    }
}

void recLog(const char* const* log, int logCount) {
    if (g_screen != SCREEN_REC) return;
    for (int i = 0; i < LOG_ROWS; i++) {
        bool newest;
        const char* line = logLine(log, logCount, i, newest);
        uint32_t h = line ? hashText(line) ^ (newest ? 0x80000000u : 0) : 0;
        if (h == g_logHash[i] && !g_logStale) continue;
        g_logHash[i] = h;
        band(0, LOG_Y + i * LOG_ROW_H, SCREEN_W, LOG_ROW_H, COLOR_BG, [&](TFT_eSPI& g) {
            if (line) paintLogRow(g, i, line, newest);
        });
    }
    g_logStale = false;
}

void invalidate() {
    g_time[0] = 0;
    g_a[0] = 0;
    strcpy(g_b, "-"); // so an empty b still repaints
    g_dot = -1;
    g_dropped = UINT32_MAX;
    g_logStale = true;
    g_prog[0] = 0;
    g_fill = -1;
    g_skipped = g_junk = UINT32_MAX;
}

void discardCard(Kind kind, const char* name) {
    Card c;
    c.icon = &ICON_WARNING;
    c.border = COLOR_ERROR;
    c.title = kind == KIND_MIDI ? "DISCARD RECORDING" : "DISCARD CAPTURE";
    c.line1 = name;
    c.line2 = "this can't be undone";
    // "keep" rather than "cancel", which would read as cancelling the take.
    static const Key midi[] = {{"NAV", "keep recording", TONE_TEXT}, {"ENT", "discard", TONE_ERROR}};
    static const Key sysex[] = {{"NAV", "keep capturing", TONE_TEXT}, {"ENT", "discard", TONE_ERROR}};
    c.buttons = kind == KIND_MIDI ? midi : sysex;
    c.buttonCount = 2;
    g_screen = SCREEN_NONE; // the live calls stop until the screen is back
    dimBody();
    drawCard(c);
}

// ---- SysEx send ----------------------------------------------------------------

void sendDraw(const SendInfo& s) {
    g_screen = SCREEN_SEND;
    drawStatusFor(ICON_NOTE, COLOR_MIDI, "SYSEX");
    char size[16];
    formatSize(s.totalBytes, size, sizeof(size));
    drawTitle(s.name, s.state == SEND_ERROR ? nullptr : size, COLOR_DIM);
    if (s.state == SEND_ERROR) {
        drawErrorBody("CAN'T SEND THIS FILE", s.error);
        drawSendFooter(s.state);
        return;
    }
    formatTime(s.elapsedMs, g_time, sizeof(g_time));
    snprintf(g_a, sizeof(g_a), "%lu", (unsigned long)s.messages);
    sendProgText(s, g_prog, sizeof(g_prog));
    g_fill = sendFill(s);
    g_head = s.active && s.state == SEND_SENDING;
    g_skipped = s.skipped;
    g_junk = s.junk;
    band(0, TOP_Y - 2, SCREEN_W, BODY_BOTTOM - (TOP_Y - 2), COLOR_BG, [&](TFT_eSPI& g) {
        paintTime(g, g_time, sendTimeColor(s.state));
        paintCount(g, COL_A, "MESSAGES", g_a, COLOR_TEXT);
        g.drawFastHLine(X0, RULE1_Y, W, COLOR_RULE);
        paintSendState(g, s.state);
        g.drawFastHLine(X0, RULE2_Y, W, COLOR_RULE);
        paintProgLine(g, g_prog);
        paintBar(g, g_fill, g_head);
        paintExtra(g, g_skipped, g_junk);
    });
    drawSendFooter(s.state);
}

void sendLive(const SendInfo& s) {
    if (g_screen != SCREEN_SEND || s.state == SEND_ERROR) return;
    char buf[48];
    formatTime(s.elapsedMs, buf, sizeof(buf));
    if (strcmp(buf, g_time) != 0) {
        strcpy(g_time, buf);
        drawTime(buf, sendTimeColor(s.state));
    }
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.messages);
    if (strcmp(buf, g_a) != 0) {
        strcpy(g_a, buf);
        drawCountValue(COL_A, buf);
    }
    sendProgText(s, buf, sizeof(buf));
    if (strcmp(buf, g_prog) != 0) {
        strcpy(g_prog, buf);
        band(0, PROG_Y, SCREEN_W, 9, COLOR_BG, [&](TFT_eSPI& g) { paintProgLine(g, buf); });
    }
    int fill = sendFill(s);
    bool head = s.active && s.state == SEND_SENDING;
    if (fill != g_fill || head != g_head) {
        g_fill = fill;
        g_head = head;
        band(X0, BAR_Y, W, BAR_H, UNLIT, [&](TFT_eSPI& g) { paintBar(g, fill, head); });
    }
    if (s.skipped != g_skipped || s.junk != g_junk) {
        g_skipped = s.skipped;
        g_junk = s.junk;
        band(0, EXTRA_Y, SCREEN_W, EXTRA_H, COLOR_BG, [&](TFT_eSPI& g) { paintExtra(g, s.skipped, s.junk); });
    }
}

} // namespace UiRec

// ---- convert -------------------------------------------------------------------

namespace UiRec {

void convDraw(const ConvInfo& c) {
    g_screen = SCREEN_CONV;
    drawStatusFor(ICON_WAVE, COLOR_WAV, "CONVERT");
    char target[32];
    snprintf(target, sizeof(target), "%s", c.target ? c.target : "");
    for (char* p = target; *p; p++) *p = (char)toupper((unsigned char)*p); // Silkscreen has capitals only
    drawTitle(c.name, target, COLOR_WAV);
    formatTime(c.elapsedMs, g_time, sizeof(g_time));
    convLeftText(c, g_a, sizeof(g_a));
    convPctText(c, g_b, sizeof(g_b));
    g_fill = convFill(c);
    band(0, TOP_Y - 2, SCREEN_W, BODY_BOTTOM - (TOP_Y - 2), COLOR_BG, [&](TFT_eSPI& g) {
        paintTime(g, g_time, convTimeColor(c.state));
        paintCount(g, COL_A, "TIME LEFT", g_a, COLOR_TEXT);
        paintCount(g, COL_B, "DONE", g_b, COLOR_TEXT);
        g.drawFastHLine(X0, RULE1_Y, W, COLOR_RULE);
        paintConvState(g, c);
        g.drawFastHLine(X0, RULE2_Y, W, COLOR_RULE);
        paintConvOut(g, c);
        paintConvBar(g, g_fill, c.state == CONV_RUNNING);
    });
    drawConvFooter(c.state);
}

void convLive(const ConvInfo& c) {
    if (g_screen != SCREEN_CONV || c.state != CONV_RUNNING) return;
    char buf[16];
    formatTime(c.elapsedMs, buf, sizeof(buf));
    if (strcmp(buf, g_time) != 0) {
        strcpy(g_time, buf);
        drawTime(buf, convTimeColor(c.state));
    }
    convLeftText(c, buf, sizeof(buf));
    if (strcmp(buf, g_a) != 0) {
        strcpy(g_a, buf);
        drawCountValue(COL_A, buf);
    }
    convPctText(c, buf, sizeof(buf));
    if (strcmp(buf, g_b) != 0) {
        strcpy(g_b, buf);
        drawCountValue(COL_B, buf);
    }
    int fill = convFill(c);
    if (fill != g_fill) {
        g_fill = fill;
        band(X0, BAR_Y, W, BAR_H, UNLIT, [&](TFT_eSPI& g) { paintConvBar(g, fill, true); });
    }
}

} // namespace UiRec

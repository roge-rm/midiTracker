#include "ui_transition.h"
#include <TFT_eSPI.h>
#include "ui_strip.h"
#include "ui_look.h"
#include "input.h"
#include "midi_output.h"

namespace {

const int W = 320;
const int H = 240;

// Wipe: 15 bands, at least 3 ms each (45 ms or more in all).
const int WIPE_ROWS = 16;
const uint32_t WIPE_STEP_US = 3000;

// Slide: 20 steps of 16 columns, at least 8 ms each (160 ms or more).
const int SLIDE_COLS = 16;
const uint32_t SLIDE_STEP_US = 8000;

// A step that takes longer than this sends the rest at once.
const uint32_t STEP_LATE_US = 20000;

const uint32_t ARM_MS = 500;

// ST7789 vertical scroll. The panel has 320 lines and all of them scroll,
// with no fixed areas: in landscape the status strip and footer run across
// the scroll axis, so they move with the rest and the new screen brings
// its own.
const uint8_t CMD_VSCRDEF = 0x33;
const uint8_t CMD_VSCRSADD = 0x37;
const int PANEL_LINES = 320;

Ui::TransitionStyle g_style = Ui::TRANSITION_WIPE;
bool g_armed = false;
Ui::TransitionDir g_dir = Ui::TRANSITION_IN;
uint32_t g_armedMs = 0;
bool g_running = false;

void writeScrollArea(TFT_eSPI& tft) {
    tft.writecommand(CMD_VSCRDEF);
    tft.writedata(0);
    tft.writedata(0);
    tft.writedata(PANEL_LINES >> 8);
    tft.writedata(PANEL_LINES & 0xFF);
    tft.writedata(0);
    tft.writedata(0);
}

// The panel line shown first, 0-319.
void writeScroll(TFT_eSPI& tft, int line) {
    tft.writecommand(CMD_VSCRSADD);
    tft.writedata((uint8_t)(line >> 8));
    tft.writedata((uint8_t)(line & 0xFF));
}

// For screens drawn with no pump of their own: MIDI in keeps being read
// and handled, as the main loop does.
void pumpMidiIn() { MidiOutput::update(); }

bool stopEarly() { return Input::anyNewPress() || Ui::ringLow(); }

// Runs the pump at least once, then until stepUs has passed since start.
// False when a key, a low ring or a slow step means the rest should go out
// at once.
bool pace(uint32_t start, uint32_t stepUs) {
    Ui::pumpOnce();
    if (micros() - start > STEP_LATE_US) return false;
    while (micros() - start < stepUs) {
        if (stopEarly()) return false;
        Ui::pumpOnce();
    }
    return !stopEarly();
}

void wipe(Ui::TransitionDir dir, void (*draw)(void*), void* ctx) {
    const int steps = H / WIPE_ROWS;
    for (int i = 0; i < steps; i++) {
        uint32_t t0 = micros();
        int y = dir == Ui::TRANSITION_IN ? i * WIPE_ROWS : H - (i + 1) * WIPE_ROWS;
        Ui::setDrawClip(0, y, W, WIPE_ROWS);
        Ui::setDrawPass(i);
        draw(ctx);
        if (i + 1 < steps && !pace(t0, WIPE_STEP_US)) {
            if (dir == Ui::TRANSITION_IN) Ui::setDrawClip(0, y + WIPE_ROWS, W, H - y - WIPE_ROWS);
            else Ui::setDrawClip(0, 0, W, y);
            draw(ctx);
            break;
        }
    }
    Ui::clearDrawClip();
}

// Each step moves the picture 16 columns further, then draws the new
// screen's columns that the move just brought round to the incoming edge.
// Writes ignore the scroll, so column c of the new screen is drawn at x = c
// and lines up once the scroll is back at 0.
void slide(Ui::TransitionDir dir, void (*draw)(void*), void* ctx) {
    TFT_eSPI& tft = Look::screen();
    // MV puts x on the panel lines. Rotation 3 also sets MY, which runs
    // them from the right, so there a growing start line moves the picture
    // right; in rotation 1 it moves it left.
    bool lineMovesRight = tft.getRotation() == 3;
    writeScrollArea(tft);
    const int steps = W / SLIDE_COLS;
    for (int i = 1; i <= steps; i++) {
        uint32_t t0 = micros();
        int shown = i * SLIDE_COLS; // columns of the new screen in view
        // Going in the picture moves left, going out it moves right.
        int right = dir == Ui::TRANSITION_OUT ? shown : -shown;
        int line = lineMovesRight ? right : -right;
        line = ((line % PANEL_LINES) + PANEL_LINES) % PANEL_LINES;
        writeScroll(tft, line);
        int x = dir == Ui::TRANSITION_IN ? shown - SLIDE_COLS : W - shown;
        Ui::setDrawClip(x, 0, SLIDE_COLS, H);
        Ui::setDrawPass(i - 1);
        draw(ctx);
        if (i < steps && !pace(t0, SLIDE_STEP_US)) {
            // Straight back to 0, then the columns still to come.
            writeScroll(tft, 0);
            if (dir == Ui::TRANSITION_IN) Ui::setDrawClip(shown, 0, W - shown, H);
            else Ui::setDrawClip(0, 0, W - shown, H);
            draw(ctx);
            break;
        }
    }
    writeScroll(tft, 0);
    Ui::clearDrawClip();
}

} // namespace

namespace Ui {

void setTransitionStyle(TransitionStyle style) { g_style = style; }

void transitionCancel() { g_armed = false; }

void transitionNext(TransitionDir dir) {
    g_armed = true;
    g_dir = dir;
    g_armedMs = millis();
}

void transitionDraw(void (*draw)(void*), void* ctx) {
    bool armed = g_armed && millis() - g_armedMs < ARM_MS;
    g_armed = false;
    if (!armed || g_running || g_style == TRANSITION_OFF || ringLow() || screenHeld()) {
        draw(ctx);
        return;
    }
    g_running = true;
    bool ownPump = !hasDrawPump();
    if (ownPump) setDrawPump(pumpMidiIn);
    // The scroll runs along x only in landscape (rotation 1 or 3).
    if (g_style == TRANSITION_SLIDE && (Look::screen().getRotation() & 1)) slide(g_dir, draw, ctx);
    else wipe(g_dir, draw, ctx);
    if (ownPump) setDrawPump(nullptr);
    setDrawPass(0);
    g_running = false;
}

} // namespace Ui

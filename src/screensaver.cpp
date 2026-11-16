#include "screensaver.h"
#include <Arduino.h>
#include <string.h>
#include "input.h"
#include "ui.h"
#include "ui_strip.h"
#include "ui_tap.h"
#include "battery.h"
#include "settings_mode.h"

namespace Screensaver {
namespace {

const uint32_t DARK_PLAYING_MS = 5UL * 60 * 1000;
const uint32_t DARK_IDLE_MS = 60UL * 1000;
const uint32_t MIDI_IN_HOLD_MS = 30000; // MIDI in counts as playing this long after a note
const uint32_t VIEW_HOLD_MS = 4000;     // a view stays up at least this long before another takes over
const int DIM_PERCENT = 20;            // of the Brightness setting

uint32_t g_timeoutMs = 5UL * 60 * 1000;
Style g_style = STYLE_AUTO;

bool g_active = false;
bool g_woke = false;          // woke this loop, the backlight goes back in draw()
uint32_t g_lastInputMs = 0;
uint32_t g_dimAtMs = 0;
int g_backlight = -1;         // percent set while active, -1 = not yet
bool g_needStart = false;

UiSaver::View g_view = UiSaver::VIEW_IDLE;
UiSaver::Source g_src = UiSaver::SRC_NONE;
uint32_t g_frameAt = 0;
uint32_t g_viewAtMs = 0;  // when the view up now started
uint8_t g_frameNo = 0;
bool g_wasLow = false;
bool g_pending = false;

// Battery warning level: 0 fine, 1 at 15% or less, 2 at 5% or less. It
// drops back only 3% above, so a reading that wobbles doesn't wake it again.
int g_battLevel = 0;

// MIDI in.
uint8_t g_hitsSeen[16];
uint32_t g_midiAtMs = 0;
bool g_midiHeard = false;
UiSaver::Source g_lastSrc = UiSaver::SRC_NONE;
uint32_t g_srcSinceMs = 0;

int battLevel() {
    if (Battery::isCharging()) return 0;
    int p = Battery::percentage();
    int level = p <= 5 ? 2 : p <= 15 ? 1 : 0;
    if (level < g_battLevel) {
        int edge = g_battLevel == 2 ? 5 : 15;
        if (p <= edge + 3) level = g_battLevel;
    }
    return level;
}

void setBacklight(int percent) {
    if (percent == g_backlight) return;
    g_backlight = percent;
    Ui::setBacklightBrightness(percent);
}

int dimPercent() {
    int p = SettingsMode::displayBrightness() * DIM_PERCENT / 100;
    return p < 1 ? 1 : p;
}

void wake(uint32_t now) {
    g_active = false;
    g_woke = true;
    g_lastInputMs = now;
    Ui::setScreenHeld(false);
}

void sleep(uint32_t now) {
    g_active = true;
    g_dimAtMs = now;
    g_backlight = -1;
    g_needStart = true;
    g_pending = false;
    Ui::setScreenHeld(true);
}

UiSaver::View pickView(UiSaver::Source s) {
    if (s == UiSaver::SRC_NONE) return UiSaver::VIEW_IDLE;
    switch (g_style) {
        case STYLE_NOW_PLAYING: return UiSaver::VIEW_NOW_PLAYING;
        case STYLE_METERS:      return UiSaver::VIEW_METERS;
        default:
            return s == UiSaver::SRC_TRACKER || s == UiSaver::SRC_MIDI_FILE ? UiSaver::VIEW_RAIN
                                                                             : UiSaver::VIEW_NOW_PLAYING;
    }
}

// Notes while no song plays: MIDI in (pariSynth) and the like.
void watchMidi(uint32_t now, UiSaver::Source src) {
    if (memcmp(g_hitsSeen, UiTap::g_midi.hits, sizeof(g_hitsSeen)) != 0) {
        memcpy(g_hitsSeen, UiTap::g_midi.hits, sizeof(g_hitsSeen));
        if (src == UiSaver::SRC_NONE) {
            g_midiAtMs = now;
            g_midiHeard = true;
        }
    }
}

// Draws with the hold lifted and the player's pump running.
template <class F>
void drawThrough(void (*pump)(), F draw) {
    Ui::holdBypass(true);
    Ui::setDrawPump(pump);
    draw();
    Ui::setDrawPump(nullptr);
    Ui::holdBypass(false);
}

} // namespace

void setTimeoutMinutes(int minutes) { g_timeoutMs = minutes > 0 ? (uint32_t)minutes * 60000UL : 0; }

void setStyle(Style style) { g_style = style; }

bool active() { return g_active; }

bool update(bool allowed) {
    uint32_t now = millis();
    int bl = battLevel();
    bool battWarn = bl > g_battLevel;
    g_battLevel = bl;
    if (Input::anyDown()) {
        g_lastInputMs = now;
        if (g_active) {
            wake(now);
            Input::muteUntilReleased();
            return true;
        }
        return false;
    }
    if (g_active) {
        if (!allowed || battWarn) {
            wake(now);
            return true;
        }
        return false;
    }
    // The time counts from when the screen became one it may start on.
    if (!allowed || g_timeoutMs == 0) {
        g_lastInputMs = now;
        return false;
    }
    if (now - g_lastInputMs >= g_timeoutMs) sleep(now);
    return false;
}

void draw(const UiSaver::Info& in, void (*pump)()) {
    uint32_t now = millis();
    watchMidi(now, in.src);
    if (g_woke) {
        // After the mode has drawn its screen again this loop.
        g_woke = false;
        g_backlight = -1;
        Ui::setBacklightBrightness(SettingsMode::displayBrightness());
        return;
    }
    if (!g_active) return;

    UiSaver::Info info = in;
    if (info.src == UiSaver::SRC_NONE && g_midiHeard && now - g_midiAtMs < MIDI_IN_HOLD_MS) {
        info.src = UiSaver::SRC_MIDI_IN;
        info.name = "MIDI in";
    }
    if (info.src != g_lastSrc) {
        g_lastSrc = info.src;
        g_srcSinceMs = now;
    }
    if (info.src == UiSaver::SRC_LOOPER || info.src == UiSaver::SRC_MIDI_IN) info.elapsedMs = now - g_srcSinceMs;
    info.battery = Battery::percentage();
    info.charging = Battery::isCharging();
    info.low = Battery::isLow();

    bool playing = info.src != UiSaver::SRC_NONE;
    bool dark = g_style == STYLE_BLACK || now - g_dimAtMs >= (playing ? DARK_PLAYING_MS : DARK_IDLE_MS);
    if (dark) {
        // Nothing is drawn in the dark, so coming back starts the view again.
        if (g_backlight != 0) g_needStart = true;
        setBacklight(0);
        return;
    }

    UiSaver::View view = pickView(info.src);
    bool change = view != g_view || info.src != g_src;
    if (g_needStart || (change && now - g_viewAtMs >= VIEW_HOLD_MS)) {
        g_needStart = false;
        g_viewAtMs = now;
        g_view = view;
        g_src = info.src;
        g_pending = false;
        UiSaver::start(view, info);
        // The clear and first frame start on the next loop.
        g_frameAt = now - UiSaver::frameMs(view);
        g_pending = true;
        setBacklight(dimPercent());
        return;
    }
    setBacklight(dimPercent());
    // The source changed too soon after the last switch: the view stays as
    // it is until it may change, finishing only what it had left to draw.
    if (change && !g_pending) return;

    bool due = now - g_frameAt >= UiSaver::frameMs(view);
    if (due) {
        g_frameAt = now;
        // While the ring runs low, only every 2nd frame, then every 4th.
        bool low = Ui::ringLow();
        int every = low ? (g_wasLow ? 4 : 2) : 1;
        g_wasLow = low;
        if (++g_frameNo % every) return;
    } else if (!g_pending || Ui::ringLow()) {
        return;
    }
    if (!Ui::sliceLeft()) return;
    drawThrough(pump, [&] { g_pending = UiSaver::frame(info); });
}

} // namespace Screensaver

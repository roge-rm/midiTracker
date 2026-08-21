#include "settings_mode.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SdFat.h>

#include "input.h"
#include "ui.h"
#include "sd_card.h"
#include "midi_output.h"

namespace SettingsMode
{
    namespace
    {

        const char *SETTINGS_PATH = "/settings.txt";

        enum SettingIndex
        {
            SETTING_BPM,
            SETTING_TIME_SIGNATURE,
            SETTING_CLOCK_SOURCE,
            SETTING_BAR_LENGTH,
            SETTING_SYNC,
            SETTING_METRONOME,
            SETTING_METRONOME_VOLUME,
            SETTING_COUNT_IN,
            SETTING_COUNT_IN_BARS,
            SETTING_MIDI_TRANSPORT,
            SETTING_MIDI_THRU,
            SETTING_COUNT,
        };

        // Same bar-length preset convention LooperMode's own BAR_PRESETS/
        // cycleBarLengthOnSelected() use (0 = Freeform) -- duplicated rather than
        // shared since the two modes otherwise have zero coupling (see
        // looper_mode.h's header comment on why main.cpp mediates instead of
        // modes including each other), and it's a single small constant table.
        const int BAR_PRESET_COUNT = 9;
        const int BAR_PRESETS[BAR_PRESET_COUNT] = {0, 1, 2, 4, 8, 16, 32, 64, 128};

        // Same time-signature preset list/convention as LooperMode's own
        // TIME_SIG_PRESETS (duplicated for the same reason BAR_PRESETS above is).
        struct TimeSig
        {
            int num, den;
        };
        const int TIME_SIG_PRESET_COUNT = 6;
        const TimeSig TIME_SIG_PRESETS[TIME_SIG_PRESET_COUNT] = {
            {4, 4},
            {3, 4},
            {2, 4},
            {6, 8},
            {5, 4},
            {7, 8},
        };

        float g_defaultBpm = 120.0f;
        int g_defaultTimeSigNum = 4;
        int g_defaultTimeSigDen = 4;
        int g_defaultBarLength = 0; // Freeform
        bool g_defaultSync = true;
        bool g_defaultMetronomeOn = false;
        bool g_defaultCountInEnabled = true;
        int g_defaultCountInBars = 1;
        int g_metronomeVolume = 80;
        MidiThruMode g_defaultThruMode = MIDI_THRU_OFF;
        bool g_clockSourceSlave = false;     // false = Internal (preset BPM), true = Slave (follow MIDI clock)
        bool g_midiTransportEnabled = false; // react to incoming Start/Stop/Continue

        const char *thruModeLabel(MidiThruMode mode)
        {
            switch (mode)
            {
            case MIDI_THRU_OFF:
                return "Off";
            case MIDI_THRU_ON:
                return "On";
            case MIDI_THRU_TRS2USB:
                return "TRS>USB";
            case MIDI_THRU_USB2TRS:
                return "USB>TRS";
            case MIDI_THRU_TRS2TRS:
                return "TRS>TRS";
            case MIDI_THRU_USB2USB:
                return "USB>USB";
            default:
                return "Off";
            }
        }

        int timeSigPresetIndex()
        {
            for (int i = 0; i < TIME_SIG_PRESET_COUNT; i++)
            {
                if (TIME_SIG_PRESETS[i].num == g_defaultTimeSigNum && TIME_SIG_PRESETS[i].den == g_defaultTimeSigDen)
                    return i;
            }
            return 0; // falls back to 4/4 -- see LooperMode's timeSigPresetIndex() for why
        }

        int cursor = 0;
        int scrollOffset = 0;
        bool needsRedraw = true;

        const char *itemLabel(int index)
        {
            switch (index)
            {
            case SETTING_BPM:
                return "Default BPM";
            case SETTING_TIME_SIGNATURE:
                return "Time Sig";
            case SETTING_BAR_LENGTH:
                return "Loop Length";
            case SETTING_SYNC:
                return "Sync Mode";
            case SETTING_METRONOME:
                return "Metronome";
            case SETTING_COUNT_IN:
                return "Count In";
            case SETTING_COUNT_IN_BARS:
                return "Count In Bars";
            case SETTING_METRONOME_VOLUME:
                return "Metro Volume";
            case SETTING_MIDI_THRU:
                return "MIDI Thru";
            case SETTING_CLOCK_SOURCE:
                return "Clock Source";
            case SETTING_MIDI_TRANSPORT:
                return "MIDI Transport";
            default:
                return "";
            }
        }

        // Kept separate from drawing so both the full redraw and the single-row
        // partial update after a value change (see updateSettingsValue()) share
        // exactly the same formatting.
        void formatValue(int index, char *out, size_t outSize)
        {
            switch (index)
            {
            case SETTING_BPM:
                snprintf(out, outSize, "%d", (int)(g_defaultBpm + 0.5f));
                break;
            case SETTING_TIME_SIGNATURE:
                snprintf(out, outSize, "%d/%d", g_defaultTimeSigNum, g_defaultTimeSigDen);
                break;
            case SETTING_BAR_LENGTH:
                if (g_defaultBarLength == 0)
                    snprintf(out, outSize, "Free");
                else
                    snprintf(out, outSize, "%d bars", g_defaultBarLength);
                break;
            case SETTING_SYNC:
                snprintf(out, outSize, "%s", g_defaultSync ? "Sync" : "Independent");
                break;
            case SETTING_METRONOME:
                snprintf(out, outSize, "%s", g_defaultMetronomeOn ? "On" : "Off");
                break;
            case SETTING_COUNT_IN:
                snprintf(out, outSize, "%s", g_defaultCountInEnabled ? "On" : "Off");
                break;
            case SETTING_COUNT_IN_BARS:
                snprintf(out, outSize, "%d bar%s", g_defaultCountInBars, g_defaultCountInBars == 1 ? "" : "s");
                break;
            case SETTING_METRONOME_VOLUME:
                snprintf(out, outSize, "%d%%", g_metronomeVolume);
                break;
            case SETTING_MIDI_THRU:
                snprintf(out, outSize, "%s", thruModeLabel(g_defaultThruMode));
                break;
            case SETTING_CLOCK_SOURCE:
                snprintf(out, outSize, "%s", g_clockSourceSlave ? "External" : "Internal");
                break;
            case SETTING_MIDI_TRANSPORT:
                snprintf(out, outSize, "%s", g_midiTransportEnabled ? "On" : "Off");
                break;
            default:
                out[0] = '\0';
                break;
            }
        }

        bool isBoolSetting(int index)
        {
            return index == SETTING_SYNC || index == SETTING_METRONOME || index == SETTING_COUNT_IN ||
                   index == SETTING_CLOCK_SOURCE || index == SETTING_MIDI_TRANSPORT;
        }

        // `direction` is +1 (RIGHT) or -1 (LEFT). Bool items are set directly
        // from direction rather than toggled, so a stray repeat can't flip one
        // twice -- matters less here than for numeric items since bool items are
        // tap-only (see handleAdjust()), but keeps the two paths symmetric.
        void adjustSetting(int index, int direction)
        {
            switch (index)
            {
            case SETTING_BPM:
                g_defaultBpm += (float)direction;
                if (g_defaultBpm < 20.0f)
                    g_defaultBpm = 20.0f;
                if (g_defaultBpm > 300.0f)
                    g_defaultBpm = 300.0f;
                break;
            case SETTING_TIME_SIGNATURE:
            {
                int idx = timeSigPresetIndex() + direction;
                if (idx < 0)
                    idx = 0;
                if (idx >= TIME_SIG_PRESET_COUNT)
                    idx = TIME_SIG_PRESET_COUNT - 1;
                g_defaultTimeSigNum = TIME_SIG_PRESETS[idx].num;
                g_defaultTimeSigDen = TIME_SIG_PRESETS[idx].den;
                break;
            }
            case SETTING_BAR_LENGTH:
            {
                int idx = 0;
                for (int i = 0; i < BAR_PRESET_COUNT; i++)
                {
                    if (BAR_PRESETS[i] == g_defaultBarLength)
                    {
                        idx = i;
                        break;
                    }
                }
                idx += direction;
                if (idx < 0)
                    idx = 0;
                if (idx >= BAR_PRESET_COUNT)
                    idx = BAR_PRESET_COUNT - 1;
                g_defaultBarLength = BAR_PRESETS[idx];
                break;
            }
            case SETTING_SYNC:
                g_defaultSync = (direction > 0);
                break;
            case SETTING_METRONOME:
                g_defaultMetronomeOn = (direction > 0);
                break;
            case SETTING_COUNT_IN:
                g_defaultCountInEnabled = (direction > 0);
                break;
            case SETTING_COUNT_IN_BARS:
                g_defaultCountInBars += direction;
                if (g_defaultCountInBars < 1)
                    g_defaultCountInBars = 1;
                if (g_defaultCountInBars > 8)
                    g_defaultCountInBars = 8;
                break;
            case SETTING_METRONOME_VOLUME:
                g_metronomeVolume += direction * 5;
                if (g_metronomeVolume < 0)
                    g_metronomeVolume = 0;
                if (g_metronomeVolume > 100)
                    g_metronomeVolume = 100;
                break;
            case SETTING_MIDI_THRU:
            {
                int idx = (int)g_defaultThruMode + direction;
                if (idx < 0)
                    idx = 0;
                if (idx > (int)MIDI_THRU_USB2USB)
                    idx = (int)MIDI_THRU_USB2USB;
                g_defaultThruMode = (MidiThruMode)idx;
                MidiOutput::setThruMode(g_defaultThruMode);
                break;
            }
            case SETTING_CLOCK_SOURCE:
                g_clockSourceSlave = (direction > 0);
                break;
            case SETTING_MIDI_TRANSPORT:
                g_midiTransportEnabled = (direction > 0);
                break;
            }
        }

        // Fills caller-owned storage with every item's label/value, and points
        // labelPtrs/valuePtrs at them -- shared by every call site that needs the
        // full list (the initial draw and both cursor-move directions), so
        // there's exactly one place that knows how to assemble it.
        void buildLists(const char *labelPtrs[SETTING_COUNT], char values[SETTING_COUNT][16], const char *valuePtrs[SETTING_COUNT])
        {
            for (int i = 0; i < SETTING_COUNT; i++)
            {
                labelPtrs[i] = itemLabel(i);
                formatValue(i, values[i], 16);
                valuePtrs[i] = values[i];
            }
        }

        // Persists every value to /settings.txt, overwriting it whole -- called
        // once on the way out of this screen (see handleInput()), not after each
        // individual change, so adjusting a value (especially a hold-repeated
        // numeric one) never waits on SD I/O.
        void saveSettings()
        {
            FsFile file;
            if (!file.open(SETTINGS_PATH, O_RDWR | O_CREAT | O_TRUNC))
                return;

            char line[48];
            int n;
            n = snprintf(line, sizeof(line), "bpm=%d\n", (int)(g_defaultBpm + 0.5f));
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "timeSigNum=%d\n", g_defaultTimeSigNum);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "timeSigDen=%d\n", g_defaultTimeSigDen);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "barLength=%d\n", g_defaultBarLength);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "sync=%d\n", g_defaultSync ? 1 : 0);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "metronome=%d\n", g_defaultMetronomeOn ? 1 : 0);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "countIn=%d\n", g_defaultCountInEnabled ? 1 : 0);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "countInBars=%d\n", g_defaultCountInBars);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "metroVolume=%d\n", g_metronomeVolume);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "midiThru=%d\n", (int)g_defaultThruMode);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "clockSlave=%d\n", g_clockSourceSlave ? 1 : 0);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "midiTransport=%d\n", g_midiTransportEnabled ? 1 : 0);
            file.write((const uint8_t *)line, n);

            file.sync();
            file.close();
        }

        void parseSettingsLine(const char *line)
        {
            if (strncmp(line, "bpm=", 4) == 0)
            {
                g_defaultBpm = (float)atoi(line + 4);
                return;
            }
            if (strncmp(line, "timeSigNum=", 11) == 0)
            {
                g_defaultTimeSigNum = atoi(line + 11);
                return;
            }
            if (strncmp(line, "timeSigDen=", 11) == 0)
            {
                g_defaultTimeSigDen = atoi(line + 11);
                return;
            }
            if (strncmp(line, "barLength=", 10) == 0)
            {
                g_defaultBarLength = atoi(line + 10);
                return;
            }
            if (strncmp(line, "sync=", 5) == 0)
            {
                g_defaultSync = atoi(line + 5) != 0;
                return;
            }
            if (strncmp(line, "metronome=", 10) == 0)
            {
                g_defaultMetronomeOn = atoi(line + 10) != 0;
                return;
            }
            if (strncmp(line, "countIn=", 8) == 0)
            {
                g_defaultCountInEnabled = atoi(line + 8) != 0;
                return;
            }
            if (strncmp(line, "countInBars=", 12) == 0)
            {
                g_defaultCountInBars = atoi(line + 12);
                return;
            }
            if (strncmp(line, "metroVolume=", 12) == 0)
            {
                g_metronomeVolume = atoi(line + 12);
                return;
            }
            if (strncmp(line, "midiThru=", 9) == 0)
            {
                int v = atoi(line + 9);
                if (v < 0)
                    v = 0;
                if (v > (int)MIDI_THRU_USB2USB)
                    v = (int)MIDI_THRU_USB2USB;
                g_defaultThruMode = (MidiThruMode)v;
                return;
            }
            if (strncmp(line, "clockSlave=", 11) == 0)
            {
                g_clockSourceSlave = atoi(line + 11) != 0;
                return;
            }
            if (strncmp(line, "midiTransport=", 14) == 0)
            {
                g_midiTransportEnabled = atoi(line + 14) != 0;
                return;
            }
        }

        // Missing file (first ever boot) or a missing/unrecognized key just
        // leaves the hardcoded defaults above untouched -- same graceful-fallback
        // approach LooperMode's readSessionMeta() uses for session.txt.
        void loadSettings()
        {
            FsFile file;
            if (!file.open(SETTINGS_PATH, O_RDONLY))
                return;

            char line[32];
            int lineLen = 0;
            int c;
            while ((c = file.read()) >= 0)
            {
                if (c == '\n')
                {
                    line[lineLen] = '\0';
                    parseSettingsLine(line);
                    lineLen = 0;
                }
                else if (c != '\r')
                {
                    if (lineLen < (int)sizeof(line) - 1)
                        line[lineLen++] = (char)c;
                }
            }
            if (lineLen > 0)
            {
                line[lineLen] = '\0';
                parseSettingsLine(line);
            }
            file.close();
        }

        void refreshValue()
        {
            char valBuf[16];
            formatValue(cursor, valBuf, sizeof(valBuf));
            Ui::updateSettingsValue(itemLabel(cursor), valBuf, cursor, scrollOffset);
        }

        // EDIT held + LEFT/RIGHT: adjusts the selected item. Bool items (Sync/
        // Metronome/Count In) are tap-only -- a hold-repeat toggle would just
        // flicker back and forth, unlike a ranged value. Numeric items use the
        // same hold-to-accelerate pattern as LooperMode's BPM row.
        void handleAdjust()
        {
            if (!Input::isDown(BTN_EDIT))
                return;

            if (isBoolSetting(cursor))
            {
                bool changed = false;
                if (Input::justPressed(BTN_RIGHT))
                {
                    adjustSetting(cursor, 1);
                    changed = true;
                }
                if (Input::justPressed(BTN_LEFT))
                {
                    adjustSetting(cursor, -1);
                    changed = true;
                }
                if (changed)
                    refreshValue();
                return;
            }

            const uint32_t NORMAL_INTERVAL_MS = 120;
            const uint32_t FAST_INTERVAL_MS = 40;
            const uint32_t ACCEL_AFTER_MS = 2000;

            static uint32_t rightPressedAtMs = 0;
            static uint32_t leftPressedAtMs = 0;
            static uint32_t lastRightStep = 0;
            static uint32_t lastLeftStep = 0;
            uint32_t now = millis();

            if (Input::justPressed(BTN_RIGHT))
                rightPressedAtMs = now;
            if (Input::justPressed(BTN_LEFT))
                leftPressedAtMs = now;

            bool changed = false;
            if (Input::isDown(BTN_RIGHT))
            {
                uint32_t interval = (now - rightPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval)
                {
                    adjustSetting(cursor, 1);
                    lastRightStep = now;
                    changed = true;
                }
            }
            if (Input::isDown(BTN_LEFT))
            {
                uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval)
                {
                    adjustSetting(cursor, -1);
                    lastLeftStep = now;
                    changed = true;
                }
            }
            if (changed)
                refreshValue();
        }

        // Keeps `cursor` within the current scroll window, page-snapping
        // scrollOffset to a whole multiple of the visible row count whenever the
        // cursor leaves it -- same convention FilePlayerMode's browser uses (see
        // its ensureSelectionVisible()), so the two scrollable lists in this app
        // behave identically.
        void ensureSettingsVisible()
        {
            int rows = Ui::visibleRows();
            if (rows <= 0)
                return;
            if (cursor < scrollOffset || cursor >= scrollOffset + rows)
            {
                scrollOffset = (cursor / rows) * rows;
            }
            if (scrollOffset < 0)
                scrollOffset = 0;
        }

        // Moves the cursor to `newCursor` and repaints -- just the two affected
        // rows if the scroll window didn't need to change, or the whole screen
        // (via needsRedraw) if it did, same "cheap unless a page flip is
        // unavoidable" reasoning as FilePlayerMode's moveSelection().
        void moveCursor(int newCursor)
        {
            int prevCursor = cursor;
            int prevScroll = scrollOffset;
            cursor = newCursor;
            ensureSettingsVisible();
            if (scrollOffset == prevScroll)
            {
                const char *labelPtrs[SETTING_COUNT];
                char values[SETTING_COUNT][16];
                const char *valuePtrs[SETTING_COUNT];
                buildLists(labelPtrs, values, valuePtrs);
                Ui::updateSettingsSelection(labelPtrs, valuePtrs, SETTING_COUNT, prevCursor, cursor, scrollOffset);
            }
            else
            {
                needsRedraw = true;
            }
        }

        // Returns true if this tick requested backing out to mode select.
        bool handleInput()
        {
            if (Input::isDown(BTN_EDIT))
            {
                handleAdjust();
                return false;
            }

            if (Input::justPressed(BTN_UP))
            {
                moveCursor(cursor > 0 ? cursor - 1 : SETTING_COUNT - 1);
            }
            if (Input::justPressed(BTN_DOWN))
            {
                moveCursor(cursor < SETTING_COUNT - 1 ? cursor + 1 : 0);
            }
            if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT))
            {
                saveSettings();
                return true;
            }
            return false;
        }

    } // namespace

    void begin()
    {
        loadSettings();
        // MIDI Thru has no "session freshness" concept the way LooperMode's
        // defaults do (see this file's header comment) -- it's a device-wide
        // routing behavior with nowhere else to be adjusted, so it's applied
        // live immediately rather than waiting on some other mode to pull it.
        MidiOutput::setThruMode(g_defaultThruMode);
    }

    void enter()
    {
        cursor = 0;
        scrollOffset = 0;
        needsRedraw = true;
    }

    bool update()
    {
        if (handleInput())
            return true;

        if (needsRedraw)
        {
            needsRedraw = false;
            const char *labelPtrs[SETTING_COUNT];
            char values[SETTING_COUNT][16];
            const char *valuePtrs[SETTING_COUNT];
            buildLists(labelPtrs, values, valuePtrs);
            Ui::drawSettings(labelPtrs, valuePtrs, SETTING_COUNT, cursor, scrollOffset);
        }
        return false;
    }

    float defaultBpm() { return g_defaultBpm; }
    int defaultTimeSigNum() { return g_defaultTimeSigNum; }
    int defaultTimeSigDen() { return g_defaultTimeSigDen; }
    int defaultBarLength() { return g_defaultBarLength; }
    bool defaultSyncMode() { return g_defaultSync; }
    bool defaultMetronomeOn() { return g_defaultMetronomeOn; }
    bool defaultCountInEnabled() { return g_defaultCountInEnabled; }
    int defaultCountInBars() { return g_defaultCountInBars; }
    int metronomeVolume() { return g_metronomeVolume; }
    bool clockSourceSlave() { return g_clockSourceSlave; }
    bool midiTransportEnabled() { return g_midiTransportEnabled; }

} // namespace SettingsMode

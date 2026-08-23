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
            SETTING_OUTPUT_LEVEL,
            SETTING_DEFAULT_VOLUME,
            SETTING_REVERB,
            SETTING_REVERB_MIX,
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
            SETTING_REBOOT_BOOTLOADER,
            SETTING_COUNT,
        };

        // Settings is split into a handful of themed pages rather than one long
        // scrolling list -- RIGHT (without EDIT held, so it doesn't collide with
        // EDIT+RIGHT's "adjust value" meaning) advances to the next page,
        // looping back to page 0 from the last one; LEFT steps back a page at
        // a time, and (since page 0 has nowhere further back to go) exits the
        // screen from page 0 itself, same as it always has. BTN_NAV remains an
        // unconditional "exit from anywhere" shortcut regardless of page. See
        // handleInput()/switchPage().
        enum SettingsPage
        {
            PAGE_AUDIO,
            PAGE_LOOPER,
            PAGE_METRONOME,
            PAGE_MIDI_SYSTEM,
            PAGE_COUNT,
        };

        const char *PAGE_TITLES[PAGE_COUNT] = {"Audio", "Looper", "Metronome", "MIDI/System"};

        const SettingIndex PAGE_AUDIO_ITEMS[] = {SETTING_OUTPUT_LEVEL, SETTING_DEFAULT_VOLUME, SETTING_REVERB, SETTING_REVERB_MIX};
        const SettingIndex PAGE_LOOPER_ITEMS[] = {SETTING_BPM, SETTING_TIME_SIGNATURE, SETTING_BAR_LENGTH, SETTING_SYNC};
        const SettingIndex PAGE_METRONOME_ITEMS[] = {SETTING_METRONOME, SETTING_METRONOME_VOLUME, SETTING_COUNT_IN, SETTING_COUNT_IN_BARS};
        const SettingIndex PAGE_MIDI_SYSTEM_ITEMS[] = {SETTING_CLOCK_SOURCE, SETTING_MIDI_TRANSPORT, SETTING_MIDI_THRU, SETTING_REBOOT_BOOTLOADER};

        const SettingIndex *PAGE_ITEMS[PAGE_COUNT] = {PAGE_AUDIO_ITEMS, PAGE_LOOPER_ITEMS, PAGE_METRONOME_ITEMS, PAGE_MIDI_SYSTEM_ITEMS};
        const int PAGE_ITEM_COUNTS[PAGE_COUNT] = {4, 4, 4, 4};

        int currentPage = 0;

        // `cursor` (declared further below) is a position *within the current
        // page* (0..currentPageItemCount()-1), not a raw SettingIndex anymore --
        // these translate between the two everywhere itemLabel()/formatValue()/
        // adjustSetting() etc. need the real SettingIndex.
        SettingIndex currentPageItem(int posInPage) { return PAGE_ITEMS[currentPage][posInPage]; }
        int currentPageItemCount() { return PAGE_ITEM_COUNTS[currentPage]; }

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

        // 0 = Headphone Low, 1 = Headphone High, 2 = Line Level -- see
        // Synth::setOutputLevel()'s comment in synth.h for why this exists
        // and what each level means. Headphone Low (safest/quietest) by
        // default.
        int g_defaultOutputLevel = 0;
        int g_defaultVolume = 75; // percent -- see FilePlayerMode's `volume`
        bool g_reverbEnabled = true;  // see Synth::setReverbEnabled()
        int g_reverbMix = 70;         // percent -- see Synth::setReverbMix()
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

        const char *outputLevelLabel(int level)
        {
            switch (level)
            {
            case 0:
                return "HP Low";
            case 1:
                return "HP High";
            case 2:
                return "Line Level";
            default:
                return "HP Low";
            }
        }

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
            case SETTING_OUTPUT_LEVEL:
                return "Output Level";
            case SETTING_DEFAULT_VOLUME:
                return "Default Volume";
            case SETTING_REVERB:
                return "Reverb";
            case SETTING_REVERB_MIX:
                return "Reverb Mix";
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
            case SETTING_REBOOT_BOOTLOADER:
                return "USB Bootloader";
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
            case SETTING_OUTPUT_LEVEL:
                snprintf(out, outSize, "%s", outputLevelLabel(g_defaultOutputLevel));
                break;
            case SETTING_DEFAULT_VOLUME:
                snprintf(out, outSize, "%d%%", g_defaultVolume);
                break;
            case SETTING_REVERB:
                snprintf(out, outSize, "%s", g_reverbEnabled ? "On" : "Off");
                break;
            case SETTING_REVERB_MIX:
                snprintf(out, outSize, "%d%%", g_reverbMix);
                break;
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
            case SETTING_REBOOT_BOOTLOADER:
                // Not a stored value -- see handleBootloaderHold() for the
                // actual trigger. Static instructional text instead.
                snprintf(out, outSize, "Hold ENTER");
                break;
            default:
                out[0] = '\0';
                break;
            }
        }

        bool isBoolSetting(int index)
        {
            return index == SETTING_SYNC || index == SETTING_METRONOME || index == SETTING_COUNT_IN ||
                   index == SETTING_CLOCK_SOURCE || index == SETTING_MIDI_TRANSPORT || index == SETTING_REVERB;
        }

        // `direction` is +1 (RIGHT) or -1 (LEFT). Bool items are set directly
        // from direction rather than toggled, so a stray repeat can't flip one
        // twice -- matters less here than for numeric items since bool items are
        // tap-only (see handleAdjust()), but keeps the two paths symmetric.
        void adjustSetting(int index, int direction)
        {
            switch (index)
            {
            case SETTING_OUTPUT_LEVEL:
            {
                int idx = g_defaultOutputLevel + direction;
                if (idx < 0)
                    idx = 0;
                if (idx > 2)
                    idx = 2;
                g_defaultOutputLevel = idx;
                break;
            }
            case SETTING_DEFAULT_VOLUME:
                g_defaultVolume += direction * 5;
                if (g_defaultVolume < 0)
                    g_defaultVolume = 0;
                if (g_defaultVolume > 100)
                    g_defaultVolume = 100;
                break;
            case SETTING_REVERB:
                g_reverbEnabled = (direction > 0);
                break;
            case SETTING_REVERB_MIX:
                g_reverbMix += direction * 5;
                if (g_reverbMix < 0)
                    g_reverbMix = 0;
                if (g_reverbMix > 100)
                    g_reverbMix = 100;
                break;
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

        // Fills caller-owned storage with the *current page's* item labels/
        // values, and points labelPtrs/valuePtrs at them -- shared by every call
        // site that needs the list (the initial draw and both cursor-move
        // directions), so there's exactly one place that knows how to assemble
        // it. Buffers stay sized to SETTING_COUNT (a safe upper bound) even
        // though only currentPageItemCount() of each actually gets used.
        void buildLists(const char *labelPtrs[SETTING_COUNT], char values[SETTING_COUNT][16], const char *valuePtrs[SETTING_COUNT])
        {
            int n = currentPageItemCount();
            for (int i = 0; i < n; i++)
            {
                SettingIndex idx = currentPageItem(i);
                labelPtrs[i] = itemLabel(idx);
                formatValue(idx, values[i], 16);
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
            n = snprintf(line, sizeof(line), "outputLevel=%d\n", g_defaultOutputLevel);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "defaultVolume=%d\n", g_defaultVolume);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "reverbEnabled=%d\n", g_reverbEnabled ? 1 : 0);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "reverbMix=%d\n", g_reverbMix);
            file.write((const uint8_t *)line, n);
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
            if (strncmp(line, "outputLevel=", 12) == 0)
            {
                g_defaultOutputLevel = atoi(line + 12);
                if (g_defaultOutputLevel < 0 || g_defaultOutputLevel > 2)
                    g_defaultOutputLevel = 0;
                return;
            }
            if (strncmp(line, "defaultVolume=", 14) == 0)
            {
                g_defaultVolume = atoi(line + 14);
                if (g_defaultVolume < 0)
                    g_defaultVolume = 0;
                if (g_defaultVolume > 100)
                    g_defaultVolume = 100;
                return;
            }
            if (strncmp(line, "reverbEnabled=", 14) == 0)
            {
                g_reverbEnabled = atoi(line + 14) != 0;
                return;
            }
            if (strncmp(line, "reverbMix=", 10) == 0)
            {
                g_reverbMix = atoi(line + 10);
                if (g_reverbMix < 0)
                    g_reverbMix = 0;
                if (g_reverbMix > 100)
                    g_reverbMix = 100;
                return;
            }
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
            SettingIndex idx = currentPageItem(cursor);
            formatValue(idx, valBuf, sizeof(valBuf));
            Ui::updateSettingsValue(itemLabel(idx), valBuf, cursor, scrollOffset);
        }

        // EDIT held + LEFT/RIGHT: adjusts the selected item. Bool items (Sync/
        // Metronome/Count In) are tap-only -- a hold-repeat toggle would just
        // flicker back and forth, unlike a ranged value. Numeric items use the
        // same hold-to-accelerate pattern as LooperMode's BPM row.
        void handleAdjust()
        {
            if (!Input::isDown(BTN_EDIT))
                return;

            SettingIndex idx = currentPageItem(cursor);
            if (isBoolSetting(idx))
            {
                bool changed = false;
                if (Input::justPressed(BTN_RIGHT))
                {
                    adjustSetting(idx, 1);
                    changed = true;
                }
                if (Input::justPressed(BTN_LEFT))
                {
                    adjustSetting(idx, -1);
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
                    adjustSetting(idx, 1);
                    lastRightStep = now;
                    changed = true;
                }
            }
            if (Input::isDown(BTN_LEFT))
            {
                uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval)
                {
                    adjustSetting(idx, -1);
                    lastLeftStep = now;
                    changed = true;
                }
            }
            if (changed)
                refreshValue();
        }

        // Reboots into the RP2040's USB (BOOTSEL) bootloader -- the device
        // stops running this firmware entirely and appears as a mass-
        // storage drive for dragging a new .uf2 onto, until it's power-
        // cycled or reflashed. That's disruptive enough (and far enough
        // from anything else EDIT+LEFT/RIGHT's "adjust a value" semantics
        // cover) that it deserves a deliberate hold rather than a single
        // tap of ENTER a stray press while scrolling through Settings
        // could trigger by accident. rp2040.rebootToBootloader() (Arduino-
        // Pico's wrapper around the pico-sdk's reset_usb_boot()) never
        // returns, so there's nothing to do after the call succeeds.
        void handleBootloaderHold()
        {
            const uint32_t BOOTLOADER_HOLD_MS = 1200;
            static uint32_t enterPressedAtMs = 0;

            if (currentPageItem(cursor) != SETTING_REBOOT_BOOTLOADER || !Input::isDown(BTN_ENTER))
            {
                enterPressedAtMs = 0;
                return;
            }
            if (Input::justPressed(BTN_ENTER))
                enterPressedAtMs = millis();
            if (enterPressedAtMs != 0 && millis() - enterPressedAtMs >= BOOTLOADER_HOLD_MS)
            {
                rp2040.rebootToBootloader();
            }
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
                Ui::updateSettingsSelection(labelPtrs, valuePtrs, currentPageItemCount(), prevCursor, cursor, scrollOffset);
            }
            else
            {
                needsRedraw = true;
            }
        }

        // Switches to `newPage` and resets cursor/scroll -- a full redraw is
        // unavoidable here (the whole item list changes), unlike moveCursor()'s
        // cheap partial-update path within a page.
        void switchPage(int newPage)
        {
            currentPage = newPage;
            cursor = 0;
            scrollOffset = 0;
            needsRedraw = true;
        }

        // Returns true if this tick requested backing out to mode select.
        bool handleInput()
        {
            handleBootloaderHold();

            if (Input::isDown(BTN_EDIT))
            {
                handleAdjust();
                return false;
            }

            if (Input::justPressed(BTN_UP))
            {
                moveCursor(cursor > 0 ? cursor - 1 : currentPageItemCount() - 1);
            }
            if (Input::justPressed(BTN_DOWN))
            {
                moveCursor(cursor < currentPageItemCount() - 1 ? cursor + 1 : 0);
            }
            // RIGHT/LEFT (without EDIT, which handleAdjust() above already
            // claimed) page-navigate instead of adjusting a value: RIGHT
            // advances a page and loops back to page 0 from the last page;
            // LEFT steps back a page at a time, and -- since page 0 has
            // nowhere further back to go -- exits the whole screen from
            // page 0 itself, the same behavior LEFT always had before there
            // were multiple pages. BTN_NAV is the unconditional "exit from
            // anywhere" shortcut regardless of page.
            if (Input::justPressed(BTN_RIGHT))
            {
                switchPage((currentPage + 1) % PAGE_COUNT);
            }
            if (Input::justPressed(BTN_LEFT))
            {
                if (currentPage > 0)
                {
                    switchPage(currentPage - 1);
                }
                else
                {
                    saveSettings();
                    return true;
                }
            }
            if (Input::justPressed(BTN_NAV))
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
        currentPage = 0;
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
            Ui::drawSettings(labelPtrs, valuePtrs, currentPageItemCount(), cursor, scrollOffset, PAGE_TITLES[currentPage]);
        }
        return false;
    }

    int defaultOutputLevel() { return g_defaultOutputLevel; }
    int defaultVolume() { return g_defaultVolume; }
    bool reverbEnabled() { return g_reverbEnabled; }
    int reverbMix() { return g_reverbMix; }
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

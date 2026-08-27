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
#include "keyboard_layout.h" // Theme page's "Save Theme" name entry -- see handleThemeSaveNameInput()

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
            SETTING_REVERB_TYPE,
            SETTING_SYNTH_AUDIO,
            SETTING_LFO_RATE,
            SETTING_LFO_VOICES,
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
            SETTING_BRIGHTNESS,
            SETTING_REBOOT_BOOTLOADER,
            SETTING_THEME_EDITOR, // ENTER opens the Theme editor (see beginThemeEditor()) -- not adjusted with EDIT+LEFT/RIGHT like every other row here
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

        const SettingIndex PAGE_AUDIO_ITEMS[] = {SETTING_OUTPUT_LEVEL, SETTING_DEFAULT_VOLUME, SETTING_REVERB, SETTING_REVERB_MIX, SETTING_REVERB_TYPE, SETTING_SYNTH_AUDIO, SETTING_LFO_RATE, SETTING_LFO_VOICES};
        const SettingIndex PAGE_LOOPER_ITEMS[] = {SETTING_BPM, SETTING_TIME_SIGNATURE, SETTING_BAR_LENGTH, SETTING_SYNC};
        const SettingIndex PAGE_METRONOME_ITEMS[] = {SETTING_METRONOME, SETTING_METRONOME_VOLUME, SETTING_COUNT_IN, SETTING_COUNT_IN_BARS};
        const SettingIndex PAGE_MIDI_SYSTEM_ITEMS[] = {SETTING_CLOCK_SOURCE, SETTING_MIDI_TRANSPORT, SETTING_MIDI_THRU, SETTING_BRIGHTNESS, SETTING_THEME_EDITOR, SETTING_REBOOT_BOOTLOADER};

        const SettingIndex *PAGE_ITEMS[PAGE_COUNT] = {PAGE_AUDIO_ITEMS, PAGE_LOOPER_ITEMS, PAGE_METRONOME_ITEMS, PAGE_MIDI_SYSTEM_ITEMS};
        const int PAGE_ITEM_COUNTS[PAGE_COUNT] = {8, 4, 4, 6};

        int currentPage = 0;

        // -- Theme editor state --------------------------------------------------
        // Entirely separate from `cursor`/`scrollOffset` below (the generic
        // settings list's own state) -- opened via ENTER on the "Theme" row in
        // PAGE_MIDI_SYSTEM (see SETTING_THEME_EDITOR/beginThemeEditor()), not a
        // page of its own: it needs a row cursor *and* a focused R/G/B field
        // within that row, plus a small save/load/reset sub-flow, none of which
        // fits the single-value-per-row model every other page uses. `inThemeEditor`
        // gates handleInput()/update() into handleThemeInput()/the Theme draw
        // dispatch instead of the generic per-page logic while it's open.
        bool inThemeEditor = false;
        enum ThemeScreen
        {
            THEME_LIST,           // the R/G/B color list itself
            THEME_MENU,           // Save Theme / Load Theme / Reset to Default
            THEME_SAVE_NAME,      // on-screen keyboard, naming a theme to save
            THEME_SAVE_OVERWRITE, // "overwrite theme X?" (see FilePlayerMode's own analog)
            THEME_LOAD_PICK,      // list of saved .thm files to load
            THEME_FLASH,          // brief result message, then back to THEME_LIST
        };
        ThemeScreen themeScreen = THEME_LIST;

        int themeColorCursor = 0;   // which color row (0..Ui::themeColorCount()-1)
        int themeChannelCursor = 0; // which field within that row: 0=R, 1=G, 2=B
        int themeScrollOffset = 0;

        const char *THEMES_ROOT = "/themes";
        const int THEME_NAME_MAX_LEN = 16;
        char themeNameBuf[THEME_NAME_MAX_LEN + 1] = {0};
        int themeNameLen = 0;
        int themeKeyRow = 1, themeKeyCol = 0; // on-screen keyboard cursor, see keyboard_layout.h
        char themeNameError[40] = {0};
        char themePendingOverwritePath[64] = {0}; // THEME_SAVE_OVERWRITE only

        const char *const THEME_MENU_LABELS[] = {"Save Theme", "Load Theme", "Reset to Default"};
        const int THEME_MENU_COUNT = 3;
        int themeMenuCursor = 0;

        const int MAX_SAVED_THEMES = 64; // comfortably above realistic use -- same "bump the constant" precedent MIDI_MAX_TRACKS documents
        char themeLoadNames[MAX_SAVED_THEMES][THEME_NAME_MAX_LEN + 1];
        int themeLoadCount = 0;
        int themeLoadCursor = 0;
        int themeLoadScrollOffset = 0; // see ensureThemeLoadVisible()

        char themeFlashMsg[32] = {0};
        uint32_t themeFlashUntilMs = 0;

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
        int g_reverbType = 0;         // 0=Lo-fi, 1=Lush, 2=Shimmer -- see Synth::setReverbType()
        bool g_synthAudioEnabled = false; // see Synth::setSynthAudioEnabled() -- off by default

        // See g_lfoRateTenthsHz/g_lfoVoices below for what these bound.
        const int LFO_RATE_MIN_TENTHS = 5;   // 0.5Hz
        const int LFO_RATE_MAX_TENTHS = 100; // 10.0Hz
        const int LFO_RATE_STEP_TENTHS = 5;  // 0.5Hz per EDIT+LEFT/RIGHT
        const int LFO_VOICES_MAX = 12;

        // Tenths of a Hz (e.g. 55 == 5.5Hz) -- see Synth::setLfoRateTenthsHz().
        // 55 matches this synth's original, previously-fixed vibrato rate.
        // Bounds picked as a reasonable vibrato/tremolo/PWM range: LFO_RATE_MIN_TENTHS
        // (0.5Hz, a slow chorus-like drift) to LFO_RATE_MAX_TENTHS (10.0Hz, a
        // brisk trill) in LFO_RATE_STEP_TENTHS (0.5Hz) steps; 0 (below the
        // minimum) is the "Off" sentinel, at the very bottom of the range.
        int g_lfoRateTenthsHz = 55;
        // See Synth::setLfoVoices()/g_maxModulatedVoices' own comment in
        // synth.cpp. 0 (Off, at the very bottom) means no voice ever
        // modulates; LFO_VOICES_MAX is a practical ceiling, not a hardware
        // one -- the melodic voice pool is larger, but capping this high
        // would defeat the setting's own "keeps it from sounding busy"
        // purpose.
        int g_lfoVoices = 3;
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
        int g_displayBrightness = 100; // percent -- see Ui::setBacklightBrightness()
        // Empty = the hardcoded default palette (Ui's own THEME_COLORS defaults,
        // never touched unless a theme's explicitly loaded). Set whenever a
        // theme is saved or loaded (see finishThemeSaveName()/
        // handleThemeSaveOverwriteInput()/handleThemeLoadPickInput()), cleared
        // by "Reset to Default" -- begin() re-applies whichever one this names
        // on every boot, so the picked theme survives a power cycle without
        // having to reload it by hand every time.
        char g_activeThemeName[THEME_NAME_MAX_LEN + 1] = {0};

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

        const char *reverbTypeLabel(int type)
        {
            switch (type)
            {
            case 0:
                return "Lo-fi";
            case 1:
                return "Lush";
            case 2:
                return "Shimmer";
            default:
                return "Lo-fi";
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
            case SETTING_REVERB_TYPE:
                return "Reverb Type";
            case SETTING_SYNTH_AUDIO:
                return "Synth Audio";
            case SETTING_LFO_RATE:
                return "LFO Rate";
            case SETTING_LFO_VOICES:
                return "LFO Voices";
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
            case SETTING_BRIGHTNESS:
                return "Brightness";
            case SETTING_REBOOT_BOOTLOADER:
                return "USB Bootloader";
            case SETTING_THEME_EDITOR:
                return "Theme";
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
            case SETTING_REVERB_TYPE:
                snprintf(out, outSize, "%s", reverbTypeLabel(g_reverbType));
                break;
            case SETTING_SYNTH_AUDIO:
                snprintf(out, outSize, "%s", g_synthAudioEnabled ? "On" : "Off");
                break;
            case SETTING_LFO_RATE:
                // Manual integer/fractional split, not %f -- kept consistent
                // with SETTING_BPM's own int-only formatting elsewhere in
                // this function.
                if (g_lfoRateTenthsHz <= 0)
                    snprintf(out, outSize, "Off");
                else
                    snprintf(out, outSize, "%d.%dHz", g_lfoRateTenthsHz / 10, g_lfoRateTenthsHz % 10);
                break;
            case SETTING_LFO_VOICES:
                if (g_lfoVoices <= 0)
                    snprintf(out, outSize, "Off");
                else
                    snprintf(out, outSize, "%d", g_lfoVoices);
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
            case SETTING_BRIGHTNESS:
                snprintf(out, outSize, "%d%%", g_displayBrightness);
                break;
            case SETTING_REBOOT_BOOTLOADER:
                // Not a stored value -- see handleBootloaderHold() for the
                // actual trigger. Static instructional text instead.
                snprintf(out, outSize, "Hold ENTER");
                break;
            case SETTING_THEME_EDITOR:
                // Not a stored value either -- see beginThemeEditor(), triggered
                // by a tap of ENTER on this row (handleInput()'s own comment).
                snprintf(out, outSize, "ENTER to edit");
                break;
            default:
                out[0] = '\0';
                break;
            }
        }

        bool isBoolSetting(int index)
        {
            return index == SETTING_SYNC || index == SETTING_METRONOME || index == SETTING_COUNT_IN ||
                   index == SETTING_CLOCK_SOURCE || index == SETTING_MIDI_TRANSPORT || index == SETTING_REVERB ||
                   index == SETTING_SYNTH_AUDIO;
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
            case SETTING_SYNTH_AUDIO:
                g_synthAudioEnabled = (direction > 0);
                break;
            case SETTING_REVERB_MIX:
                g_reverbMix += direction * 5;
                if (g_reverbMix < 0)
                    g_reverbMix = 0;
                if (g_reverbMix > 100)
                    g_reverbMix = 100;
                break;
            case SETTING_REVERB_TYPE:
            {
                int idx = g_reverbType + direction;
                if (idx < 0)
                    idx = 0;
                if (idx > 2)
                    idx = 2;
                g_reverbType = idx;
                break;
            }
            case SETTING_LFO_RATE:
                g_lfoRateTenthsHz += direction * LFO_RATE_STEP_TENTHS;
                if (g_lfoRateTenthsHz < 0)
                    g_lfoRateTenthsHz = 0; // Off, at the very bottom
                if (g_lfoRateTenthsHz > 0 && g_lfoRateTenthsHz < LFO_RATE_MIN_TENTHS)
                    g_lfoRateTenthsHz = LFO_RATE_MIN_TENTHS;
                if (g_lfoRateTenthsHz > LFO_RATE_MAX_TENTHS)
                    g_lfoRateTenthsHz = LFO_RATE_MAX_TENTHS;
                break;
            case SETTING_LFO_VOICES:
                g_lfoVoices += direction;
                if (g_lfoVoices < 0)
                    g_lfoVoices = 0; // Off, at the very bottom
                if (g_lfoVoices > LFO_VOICES_MAX)
                    g_lfoVoices = LFO_VOICES_MAX;
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
            case SETTING_BRIGHTNESS:
                g_displayBrightness += direction * 5;
                // Floor of 10, not 0 -- 0% would black out the screen with
                // no visual feedback left to see the value climb back up.
                if (g_displayBrightness < 10)
                    g_displayBrightness = 10;
                if (g_displayBrightness > 100)
                    g_displayBrightness = 100;
                Ui::setBacklightBrightness(g_displayBrightness); // live-applied immediately, same as MIDI Thru above
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
            n = snprintf(line, sizeof(line), "reverbType=%d\n", g_reverbType);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "synthAudio=%d\n", g_synthAudioEnabled ? 1 : 0);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "lfoRateTenthsHz=%d\n", g_lfoRateTenthsHz);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "lfoVoices=%d\n", g_lfoVoices);
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
            n = snprintf(line, sizeof(line), "brightness=%d\n", g_displayBrightness);
            file.write((const uint8_t *)line, n);
            n = snprintf(line, sizeof(line), "activeTheme=%s\n", g_activeThemeName);
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
            if (strncmp(line, "reverbType=", 11) == 0)
            {
                g_reverbType = atoi(line + 11);
                if (g_reverbType < 0 || g_reverbType > 2)
                    g_reverbType = 0;
                return;
            }
            if (strncmp(line, "synthAudio=", 11) == 0)
            {
                g_synthAudioEnabled = atoi(line + 11) != 0;
                return;
            }
            if (strncmp(line, "lfoRateTenthsHz=", 16) == 0)
            {
                g_lfoRateTenthsHz = atoi(line + 16);
                if (g_lfoRateTenthsHz < 0)
                    g_lfoRateTenthsHz = 0;
                if (g_lfoRateTenthsHz > 0 && g_lfoRateTenthsHz < LFO_RATE_MIN_TENTHS)
                    g_lfoRateTenthsHz = LFO_RATE_MIN_TENTHS;
                if (g_lfoRateTenthsHz > LFO_RATE_MAX_TENTHS)
                    g_lfoRateTenthsHz = LFO_RATE_MAX_TENTHS;
                return;
            }
            if (strncmp(line, "lfoVoices=", 10) == 0)
            {
                g_lfoVoices = atoi(line + 10);
                if (g_lfoVoices < 0)
                    g_lfoVoices = 0;
                if (g_lfoVoices > LFO_VOICES_MAX)
                    g_lfoVoices = LFO_VOICES_MAX;
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
            if (strncmp(line, "brightness=", 11) == 0)
            {
                g_displayBrightness = atoi(line + 11);
                if (g_displayBrightness < 10)
                    g_displayBrightness = 10;
                if (g_displayBrightness > 100)
                    g_displayBrightness = 100;
                return;
            }
            if (strncmp(line, "activeTheme=", 12) == 0)
            {
                strncpy(g_activeThemeName, line + 12, THEME_NAME_MAX_LEN);
                g_activeThemeName[THEME_NAME_MAX_LEN] = '\0';
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

        // Opens the Theme editor from a tap of ENTER on its row (see
        // handleInput()) -- always starts at the top of the color list, never
        // mid-save/load from a previous visit.
        void beginThemeEditor()
        {
            inThemeEditor = true;
            themeScreen = THEME_LIST;
            themeColorCursor = 0;
            themeChannelCursor = 0;
            themeScrollOffset = 0;
            needsRedraw = true;
        }

        // -- Theme editor --------------------------------------------------------

        void ensureThemeVisible()
        {
            int rows = Ui::visibleRows();
            if (rows <= 0)
                return;
            if (themeColorCursor < themeScrollOffset || themeColorCursor >= themeScrollOffset + rows)
            {
                themeScrollOffset = (themeColorCursor / rows) * rows;
            }
            if (themeScrollOffset < 0)
                themeScrollOffset = 0;
        }

        void moveThemeCursor(int newCursor)
        {
            int prevCursor = themeColorCursor;
            int prevScroll = themeScrollOffset;
            themeColorCursor = newCursor;
            ensureThemeVisible();
            if (themeScrollOffset == prevScroll)
            {
                Ui::updateThemeSelection(prevCursor, themeColorCursor, themeChannelCursor, themeScrollOffset);
            }
            else
            {
                needsRedraw = true;
            }
        }

        void adjustThemeChannel(int delta)
        {
            uint8_t r, g, b;
            Ui::getThemeColorRGB(themeColorCursor, r, g, b);
            uint8_t *chan = (themeChannelCursor == 0) ? &r : (themeChannelCursor == 1) ? &g
                                                                                        : &b;
            int v = (int)*chan + delta;
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            *chan = (uint8_t)v;
            Ui::setThemeColorRGB(themeColorCursor, r, g, b);
            Ui::updateThemeRow(themeColorCursor, themeChannelCursor, true, themeScrollOffset);
        }

        // EDIT held + UP/DOWN/LEFT/RIGHT: adjusts the focused channel
        // (themeChannelCursor) of the selected color row -- UP/DOWN by 10 (coarse),
        // LEFT/RIGHT by 1 (fine), each repeating while held with its own
        // hold-to-accelerate timing, same shape as LooperMode's BPM value
        // (handleBpmValueAdjust()). Bare UP/DOWN/LEFT/RIGHT (EDIT not held) mean
        // row/field navigation instead (see handleThemeInput()), so this only
        // ever runs while EDIT is down.
        void handleThemeValueHold()
        {
            if (!Input::isDown(BTN_EDIT))
                return;

            const uint32_t NORMAL_INTERVAL_MS = 120;
            const uint32_t FAST_INTERVAL_MS = 40;
            const uint32_t ACCEL_AFTER_MS = 1500;
            uint32_t now = millis();

            static uint32_t upPressedAtMs = 0, downPressedAtMs = 0;
            static uint32_t rightPressedAtMs = 0, leftPressedAtMs = 0;
            static uint32_t lastUpStep = 0, lastDownStep = 0;
            static uint32_t lastRightStep = 0, lastLeftStep = 0;

            if (Input::justPressed(BTN_UP))
                upPressedAtMs = now;
            if (Input::justPressed(BTN_DOWN))
                downPressedAtMs = now;
            if (Input::justPressed(BTN_RIGHT))
                rightPressedAtMs = now;
            if (Input::justPressed(BTN_LEFT))
                leftPressedAtMs = now;

            if (Input::isDown(BTN_UP))
            {
                uint32_t interval = (now - upPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_UP) || now - lastUpStep >= interval)
                {
                    adjustThemeChannel(10);
                    lastUpStep = now;
                }
            }
            if (Input::isDown(BTN_DOWN))
            {
                uint32_t interval = (now - downPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_DOWN) || now - lastDownStep >= interval)
                {
                    adjustThemeChannel(-10);
                    lastDownStep = now;
                }
            }
            if (Input::isDown(BTN_RIGHT))
            {
                uint32_t interval = (now - rightPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval)
                {
                    adjustThemeChannel(1);
                    lastRightStep = now;
                }
            }
            if (Input::isDown(BTN_LEFT))
            {
                uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
                if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval)
                {
                    adjustThemeChannel(-1);
                    lastLeftStep = now;
                }
            }
        }

        // Shows `msg` for a bit, then returns to THEME_LIST -- same idea as
        // LooperMode's showFlashMessage(), just local to this page since
        // nothing else in SettingsMode currently needs transient messages.
        void showThemeFlash(const char *msg)
        {
            strncpy(themeFlashMsg, msg, sizeof(themeFlashMsg) - 1);
            themeFlashMsg[sizeof(themeFlashMsg) - 1] = '\0';
            themeFlashUntilMs = millis() + 1200;
            themeScreen = THEME_FLASH;
            needsRedraw = true;
        }

        void beginThemeMenu()
        {
            themeMenuCursor = 0;
            themeScreen = THEME_MENU;
            Ui::drawEntryMenu("Theme", THEME_MENU_LABELS, THEME_MENU_COUNT, themeMenuCursor);
        }

        // Writes every themeable color as "Label=R,G,B\n" -- plain text (hence
        // the user-facing ".thm" extension is cosmetic, same spirit as
        // LooperMode's own .mid session files), one line per Ui::themeColorLabel()
        // entry so the file stays readable/hand-editable and immune to
        // THEME_COLORS being reordered later (see loadThemeFromFile()'s
        // matching-by-label, not position).
        bool saveThemeToFile(const char *path)
        {
            FsFile file;
            if (!file.open(path, O_RDWR | O_CREAT | O_TRUNC))
                return false;
            char line[48];
            for (int i = 0; i < Ui::themeColorCount(); i++)
            {
                uint8_t r, g, b;
                Ui::getThemeColorRGB(i, r, g, b);
                int n = snprintf(line, sizeof(line), "%s=%d,%d,%d\n", Ui::themeColorLabel(i), r, g, b);
                file.write((const uint8_t *)line, n);
            }
            file.sync();
            file.close();
            return true;
        }

        // Reverses saveThemeToFile() -- unrecognized labels (an older/newer
        // firmware's theme file, or a hand-edited typo) are silently skipped
        // rather than failing the whole load, same graceful-fallback approach
        // SettingsMode's own loadSettings()/LooperMode's readSessionMeta() use
        // for their own text files.
        bool loadThemeFromFile(const char *path)
        {
            FsFile file;
            if (!file.open(path, O_RDONLY))
                return false;

            char line[48];
            int lineLen = 0;
            int c;
            bool any = false;
            while ((c = file.read()) >= 0)
            {
                if (c == '\n')
                {
                    line[lineLen] = '\0';
                    char *eq = strchr(line, '=');
                    if (eq)
                    {
                        *eq = '\0';
                        int r, g, b;
                        if (sscanf(eq + 1, "%d,%d,%d", &r, &g, &b) == 3)
                        {
                            for (int i = 0; i < Ui::themeColorCount(); i++)
                            {
                                if (strcmp(line, Ui::themeColorLabel(i)) == 0)
                                {
                                    Ui::setThemeColorRGB(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
                                    any = true;
                                    break;
                                }
                            }
                        }
                    }
                    lineLen = 0;
                }
                else if (c != '\r' && lineLen < (int)sizeof(line) - 1)
                {
                    line[lineLen++] = (char)c;
                }
            }
            file.close();
            return any;
        }

        // Populates themeLoadNames[]/themeLoadCount from THEMES_ROOT's *.thm
        // files, sorted alphabetically -- same insertion-sort-while-scanning
        // approach LooperMode's buildSavedSessionList() uses (MAX_SAVED_THEMES
        // is small enough that an O(n^2) insertion sort is cheap either way).
        void scanThemesFolder()
        {
            themeLoadCount = 0;
            FsFile dir = sd.open(THEMES_ROOT);
            if (!dir || !dir.isDir())
                return;

            FsFile entry;
            char name[32];
            while (entry.openNext(&dir, O_RDONLY) && themeLoadCount < MAX_SAVED_THEMES)
            {
                if (!entry.isDir())
                {
                    entry.getName(name, sizeof(name));
                    size_t len = strlen(name);
                    if (len > 4 && strcasecmp(name + len - 4, ".thm") == 0)
                    {
                        name[len - 4] = '\0';
                        int insertAt = themeLoadCount;
                        while (insertAt > 0 && strcasecmp(themeLoadNames[insertAt - 1], name) > 0)
                        {
                            strncpy(themeLoadNames[insertAt], themeLoadNames[insertAt - 1], THEME_NAME_MAX_LEN);
                            insertAt--;
                        }
                        strncpy(themeLoadNames[insertAt], name, THEME_NAME_MAX_LEN);
                        themeLoadNames[insertAt][THEME_NAME_MAX_LEN] = '\0';
                        themeLoadCount++;
                    }
                }
                entry.close();
            }
            if (dir)
                dir.close();
        }

        // Same page-snap scroll-window convention as ensureThemeVisible() --
        // keeps themeLoadCursor's row inside the visible window as it moves.
        void ensureThemeLoadVisible()
        {
            int rows = Ui::visibleRows();
            if (rows <= 0)
                return;
            if (themeLoadCursor < themeLoadScrollOffset || themeLoadCursor >= themeLoadScrollOffset + rows)
            {
                themeLoadScrollOffset = (themeLoadCursor / rows) * rows;
            }
            if (themeLoadScrollOffset < 0)
                themeLoadScrollOffset = 0;
        }

        void beginThemeLoadPick()
        {
            scanThemesFolder();
            themeLoadCursor = 0;
            themeLoadScrollOffset = 0;
            themeScreen = THEME_LOAD_PICK;
            needsRedraw = true;
        }

        void beginThemeSaveName()
        {
            themeNameBuf[0] = '\0';
            themeNameLen = 0;
            themeNameError[0] = '\0';
            themeKeyRow = 1; // top-left letter key ('Q') -- no auto-generated
            themeKeyCol = 0; // default to accept here, unlike recordings/captures
            themeScreen = THEME_SAVE_NAME;
            needsRedraw = true;
        }

        // Trims trailing spaces, then either saves directly or -- if the name
        // collides with an existing .thm -- detours through THEME_SAVE_OVERWRITE
        // first, same "already exists" handling FilePlayerMode's own
        // finishNameEntry() uses for recordings/captures/renames.
        void finishThemeSaveName()
        {
            char trimmed[THEME_NAME_MAX_LEN + 1];
            strncpy(trimmed, themeNameBuf, sizeof(trimmed));
            trimmed[THEME_NAME_MAX_LEN] = '\0';
            int len = (int)strlen(trimmed);
            while (len > 0 && trimmed[len - 1] == ' ')
                trimmed[--len] = '\0';
            if (len == 0)
                return; // nothing typed -- stay on the naming screen

            if (!sd.exists(THEMES_ROOT))
                sd.mkdir(THEMES_ROOT);

            char path[64];
            snprintf(path, sizeof(path), "%s/%s.thm", THEMES_ROOT, trimmed);

            if (sd.exists(path))
            {
                strncpy(themePendingOverwritePath, path, sizeof(themePendingOverwritePath) - 1);
                themePendingOverwritePath[sizeof(themePendingOverwritePath) - 1] = '\0';
                themeScreen = THEME_SAVE_OVERWRITE;
                needsRedraw = true;
                return;
            }

            if (saveThemeToFile(path))
            {
                strncpy(g_activeThemeName, trimmed, THEME_NAME_MAX_LEN);
                g_activeThemeName[THEME_NAME_MAX_LEN] = '\0';
                showThemeFlash("Theme saved");
            }
            else
            {
                strncpy(themeNameError, "save failed", sizeof(themeNameError) - 1);
                Ui::updateNameEntryError(themeNameError);
            }
        }

        void handleThemeSaveNameInput()
        {
            int dRow = 0, dCol = 0;
            if (Input::justPressed(BTN_UP))
                dRow = -1;
            if (Input::justPressed(BTN_DOWN))
                dRow = 1;
            if (Input::justPressed(BTN_LEFT))
                dCol = -1;
            if (Input::justPressed(BTN_RIGHT))
                dCol = 1;
            if (dRow != 0 || dCol != 0)
            {
                int newRow = themeKeyRow + dRow;
                if (newRow < 0)
                    newRow = 0;
                if (newRow >= KEYBOARD_ROW_COUNT)
                    newRow = KEYBOARD_ROW_COUNT - 1;
                int newCol = (dRow != 0) ? themeKeyCol : themeKeyCol + dCol;
                if (newCol < 0)
                    newCol = 0;
                if (newCol >= KEYBOARD_ROW_LENS[newRow])
                    newCol = KEYBOARD_ROW_LENS[newRow] - 1;
                if (newRow != themeKeyRow || newCol != themeKeyCol)
                {
                    int prevRow = themeKeyRow, prevCol = themeKeyCol;
                    themeKeyRow = newRow;
                    themeKeyCol = newCol;
                    Ui::updateNameEntryKey(prevRow, prevCol, themeKeyRow, themeKeyCol);
                }
            }

            if (Input::justPressed(BTN_NAV))
            {
                themeScreen = THEME_LIST;
                needsRedraw = true;
                return;
            }

            if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY))
            {
                const KeyDef &key = KEYBOARD_ROWS[themeKeyRow][themeKeyCol];
                bool hadError = (themeNameError[0] != '\0');
                themeNameError[0] = '\0';
                switch (key.kind)
                {
                case KEY_CHAR:
                    if (themeNameLen < THEME_NAME_MAX_LEN)
                    {
                        themeNameBuf[themeNameLen++] = key.ch;
                        themeNameBuf[themeNameLen] = '\0';
                        Ui::updateNameEntryPreview(themeNameBuf, "");
                        if (hadError)
                            Ui::updateNameEntryError(nullptr);
                    }
                    break;
                case KEY_DEL:
                    if (themeNameLen > 0)
                    {
                        themeNameBuf[--themeNameLen] = '\0';
                        Ui::updateNameEntryPreview(themeNameBuf, "");
                        if (hadError)
                            Ui::updateNameEntryError(nullptr);
                    }
                    break;
                case KEY_OK:
                    finishThemeSaveName();
                    break;
                }
            }
        }

        void handleThemeSaveOverwriteInput()
        {
            if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT))
            {
                themeScreen = THEME_SAVE_NAME;
                needsRedraw = true;
                return;
            }
            if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY))
            {
                sd.remove(themePendingOverwritePath);
                if (saveThemeToFile(themePendingOverwritePath))
                {
                    strncpy(g_activeThemeName, themeNameBuf, THEME_NAME_MAX_LEN);
                    g_activeThemeName[THEME_NAME_MAX_LEN] = '\0';
                    showThemeFlash("Theme saved");
                }
                else
                {
                    strncpy(themeNameError, "save failed", sizeof(themeNameError) - 1);
                    themeScreen = THEME_SAVE_NAME;
                    needsRedraw = true;
                }
            }
        }

        void handleThemeMenuInput()
        {
            if (Input::justPressed(BTN_UP))
            {
                int prev = themeMenuCursor;
                themeMenuCursor = (themeMenuCursor > 0) ? themeMenuCursor - 1 : THEME_MENU_COUNT - 1;
                Ui::updateEntryMenuSelection(THEME_MENU_LABELS, THEME_MENU_COUNT, prev, themeMenuCursor);
            }
            if (Input::justPressed(BTN_DOWN))
            {
                int prev = themeMenuCursor;
                themeMenuCursor = (themeMenuCursor < THEME_MENU_COUNT - 1) ? themeMenuCursor + 1 : 0;
                Ui::updateEntryMenuSelection(THEME_MENU_LABELS, THEME_MENU_COUNT, prev, themeMenuCursor);
            }
            if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT))
            {
                themeScreen = THEME_LIST;
                needsRedraw = true;
                return;
            }
            if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY))
            {
                if (themeMenuCursor == 0)
                    beginThemeSaveName();
                else if (themeMenuCursor == 1)
                    beginThemeLoadPick();
                else
                {
                    Ui::resetThemeColorsToDefault();
                    g_activeThemeName[0] = '\0';
                    showThemeFlash("Reset to default");
                }
            }
        }

        void handleThemeLoadPickInput()
        {
            if (Input::justPressed(BTN_UP) && themeLoadCount > 0)
            {
                themeLoadCursor = (themeLoadCursor > 0) ? themeLoadCursor - 1 : themeLoadCount - 1;
                ensureThemeLoadVisible();
                needsRedraw = true;
            }
            if (Input::justPressed(BTN_DOWN) && themeLoadCount > 0)
            {
                themeLoadCursor = (themeLoadCursor < themeLoadCount - 1) ? themeLoadCursor + 1 : 0;
                ensureThemeLoadVisible();
                needsRedraw = true;
            }
            if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT))
            {
                themeScreen = THEME_MENU;
                Ui::drawEntryMenu("Theme", THEME_MENU_LABELS, THEME_MENU_COUNT, themeMenuCursor);
                return;
            }
            if ((Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) && themeLoadCount > 0)
            {
                char path[64];
                snprintf(path, sizeof(path), "%s/%s.thm", THEMES_ROOT, themeLoadNames[themeLoadCursor]);
                if (loadThemeFromFile(path))
                {
                    strncpy(g_activeThemeName, themeLoadNames[themeLoadCursor], THEME_NAME_MAX_LEN);
                    g_activeThemeName[THEME_NAME_MAX_LEN] = '\0';
                    showThemeFlash("Theme loaded");
                }
                else
                    showThemeFlash("Load failed");
            }
        }

        // Top-level entry while inThemeEditor is true, called from handleInput()
        // before any of the generic (SettingIndex-based) page logic runs -- the
        // editor's row model (a color list with a focused R/G/B field, plus its
        // own save/load/reset sub-screens) doesn't fit that shape, so it's
        // entirely self-contained here instead.
        bool handleThemeInput()
        {
            if (themeScreen == THEME_FLASH)
            {
                if (millis() >= themeFlashUntilMs)
                {
                    themeScreen = THEME_LIST;
                    needsRedraw = true;
                }
                return false;
            }
            if (themeScreen == THEME_MENU)
            {
                handleThemeMenuInput();
                return false;
            }
            if (themeScreen == THEME_SAVE_NAME)
            {
                handleThemeSaveNameInput();
                return false;
            }
            if (themeScreen == THEME_SAVE_OVERWRITE)
            {
                handleThemeSaveOverwriteInput();
                return false;
            }
            if (themeScreen == THEME_LOAD_PICK)
            {
                handleThemeLoadPickInput();
                return false;
            }

            // THEME_LIST. No handleBootloaderHold() call here -- it indexes
            // currentPageItem(cursor), which is only valid for the generic
            // (SettingIndex-based) pages this editor isn't one of.
            //
            // EDIT held repurposes all four arrow keys into value-adjust (see
            // handleThemeValueHold()) instead of their bare navigation meaning
            // below -- same "EDIT is a pure modifier, never a standalone action"
            // convention used everywhere else in this app (e.g. LooperMode's
            // ALT). Checked first so navigation never fires alongside it.
            if (Input::isDown(BTN_EDIT))
            {
                handleThemeValueHold();
                return false;
            }

            // Bare arrow keys freely navigate: UP/DOWN moves between colors,
            // LEFT/RIGHT moves between that color's R/G/B fields -- both wrap.
            // Nothing here claims LEFT for "back"/page-nav the way the generic
            // pages' LEFT does; NAV is this editor's only exit, precisely so
            // these four keys stay free to just be a 2D cursor.
            if (Input::justPressed(BTN_UP))
            {
                moveThemeCursor(themeColorCursor > 0 ? themeColorCursor - 1 : Ui::themeColorCount() - 1);
            }
            if (Input::justPressed(BTN_DOWN))
            {
                moveThemeCursor(themeColorCursor < Ui::themeColorCount() - 1 ? themeColorCursor + 1 : 0);
            }
            if (Input::justPressed(BTN_RIGHT))
            {
                themeChannelCursor = (themeChannelCursor + 1) % 3;
                Ui::updateThemeRow(themeColorCursor, themeChannelCursor, true, themeScrollOffset);
            }
            if (Input::justPressed(BTN_LEFT))
            {
                themeChannelCursor = (themeChannelCursor + 2) % 3; // -1 mod 3
                Ui::updateThemeRow(themeColorCursor, themeChannelCursor, true, themeScrollOffset);
            }
            if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY))
            {
                beginThemeMenu();
            }
            if (Input::justPressed(BTN_NAV))
            {
                inThemeEditor = false; // back to the MIDI/System row list, not out of Settings entirely
                needsRedraw = true;
                return false;
            }
            return false;
        }

        // Returns true if this tick requested backing out to mode select.
        bool handleInput()
        {
            if (inThemeEditor)
            {
                return handleThemeInput();
            }

            handleBootloaderHold();

            // A tap (not hold -- that's handleBootloaderHold()'s own row,
            // mutually exclusive since they're different SettingIndex values)
            // of ENTER on the Theme row opens the editor, same "ENTER acts on
            // the selected row" idea as everywhere else this app uses ENTER
            // for a non-adjustable action.
            if (Input::justPressed(BTN_ENTER) && currentPageItem(cursor) == SETTING_THEME_EDITOR)
            {
                beginThemeEditor();
                return false;
            }

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
        // Same reasoning for Brightness -- Ui::begin() already set the
        // backlight to a hardcoded 100% before settings had even loaded
        // (it runs first, for the splash screen), so this is what actually
        // brings it to the saved value.
        Ui::setBacklightBrightness(g_displayBrightness);
        // And again for whichever theme was last active (empty = the
        // hardcoded default palette Ui's own THEME_COLORS already start
        // at, so there's nothing to do then) -- a silent no-op if the file
        // was since deleted from the SD card by hand, same graceful-
        // fallback reasoning loadThemeFromFile() itself already documents.
        if (g_activeThemeName[0] != '\0')
        {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s.thm", THEMES_ROOT, g_activeThemeName);
            loadThemeFromFile(path);
        }
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
            if (inThemeEditor)
            {
                switch (themeScreen)
                {
                case THEME_LIST:
                    Ui::drawThemePage(themeColorCursor, themeChannelCursor, themeScrollOffset);
                    break;
                case THEME_MENU:
                    Ui::drawEntryMenu("Theme", THEME_MENU_LABELS, THEME_MENU_COUNT, themeMenuCursor);
                    break;
                case THEME_SAVE_NAME:
                    Ui::drawNameEntry("Save Theme", themeNameBuf, "",
                                       themeNameError[0] ? themeNameError : nullptr,
                                       themeKeyRow, themeKeyCol);
                    break;
                case THEME_SAVE_OVERWRITE:
                {
                    const char *slash = strrchr(themePendingOverwritePath, '/');
                    const char *name = slash ? slash + 1 : themePendingOverwritePath;
                    Ui::drawConfirmOverwrite(name, false);
                    break;
                }
                case THEME_LOAD_PICK:
                {
                    if (themeLoadCount > 0)
                    {
                        const char *labels[MAX_SAVED_THEMES];
                        for (int i = 0; i < themeLoadCount; i++)
                            labels[i] = themeLoadNames[i];
                        Ui::drawEntryMenu("Load Theme", labels, themeLoadCount, themeLoadCursor, themeLoadScrollOffset);
                    }
                    else
                    {
                        Ui::drawMessage("No saved themes", "NAV back");
                    }
                    break;
                }
                case THEME_FLASH:
                    Ui::drawMessage(themeFlashMsg, nullptr);
                    break;
                }
            }
            else
            {
                const char *labelPtrs[SETTING_COUNT];
                char values[SETTING_COUNT][16];
                const char *valuePtrs[SETTING_COUNT];
                buildLists(labelPtrs, values, valuePtrs);
                Ui::drawSettings(labelPtrs, valuePtrs, currentPageItemCount(), cursor, scrollOffset, PAGE_TITLES[currentPage]);
            }
        }
        return false;
    }

    int defaultOutputLevel() { return g_defaultOutputLevel; }
    int defaultVolume() { return g_defaultVolume; }
    bool reverbEnabled() { return g_reverbEnabled; }
    int reverbMix() { return g_reverbMix; }
    int reverbType() { return g_reverbType; }
    bool synthAudioEnabled() { return g_synthAudioEnabled; }
    int lfoRateTenthsHz() { return g_lfoRateTenthsHz; }
    int lfoVoices() { return g_lfoVoices; }
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

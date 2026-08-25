#include "synth_editor.h"

#include <stdio.h>
#include <string.h>
#include <SdFat.h>

#include "input.h"
#include "ui.h"
#include "sd_card.h"
#include "synth.h"
#include "keyboard_layout.h" // bank-name save screen's on-screen keyboard -- see handleBankSaveNameInput()

namespace SynthEditor {
namespace {

const long SYNTH_SAMPLE_RATE = 44100L; // matches synth.cpp's SAMPLE_RATE -- for ms<->sample display conversion only

enum Screen {
    SCREEN_PICKER,         // "Edit Instrument" pick list (16 families + 8 drum types)
    SCREEN_EDITOR,          // one instrument/drum's field list
    SCREEN_EDITOR_MENU,     // Save Bank / Load Bank / Reset to Default
    SCREEN_SAVE_NAME,       // on-screen keyboard, naming a bank to save
    SCREEN_SAVE_OVERWRITE,  // "overwrite bank X?"
    SCREEN_LOAD_PICK,       // list of saved .syn files to load
    SCREEN_FLASH,           // brief result message, then back to SCREEN_EDITOR
};
Screen screen = SCREEN_PICKER;
bool needsRedraw = true;

// -- Instrument picker state ------------------------------------------------
// Melodic entries (0-15) point straight at Synth::instrumentFamilyName()'s
// own static strings; drum entries (16-23) need a "Drum:" prefix built
// once here since Synth::drumPresetName() alone doesn't carry that.
const int PICKER_COUNT = Synth::INSTRUMENT_FAMILY_COUNT + Synth::DRUM_PRESET_COUNT;
char pickerDrumLabels[Synth::DRUM_PRESET_COUNT][20];
const char* pickerLabelPtrs[PICKER_COUNT];
int pickerCursor = 0;
int pickerScrollOffset = 0; // see ensurePickerVisible()

void populatePickerLabels() {
    for (int i = 0; i < Synth::INSTRUMENT_FAMILY_COUNT; i++) {
        pickerLabelPtrs[i] = Synth::instrumentFamilyName((uint8_t)i);
    }
    for (int i = 0; i < Synth::DRUM_PRESET_COUNT; i++) {
        snprintf(pickerDrumLabels[i], sizeof(pickerDrumLabels[i]), "Drum:%s", Synth::drumPresetName((uint8_t)i));
        pickerLabelPtrs[Synth::INSTRUMENT_FAMILY_COUNT + i] = pickerDrumLabels[i];
    }
}

// Same page-snap scroll-window convention as SettingsMode's
// ensureThemeVisible().
void ensurePickerVisible() {
    int rows = Ui::visibleRows();
    if (rows <= 0) return;
    if (pickerCursor < pickerScrollOffset || pickerCursor >= pickerScrollOffset + rows) {
        pickerScrollOffset = (pickerCursor / rows) * rows;
    }
    if (pickerScrollOffset < 0) pickerScrollOffset = 0;
}

// -- Instrument/drum field editor state --------------------------------
enum EditTarget { EDIT_MELODIC, EDIT_DRUM };
EditTarget editTarget = EDIT_MELODIC;
int editIndex = 0; // family 0-15 (EDIT_MELODIC) or drum type 0-7 (EDIT_DRUM)
int fieldCursor = 0;
int scrollOffset = 0;

// Working copies of the record currently open in the editor -- fetched via
// Synth::getInstrumentPreset()/getDrumPreset() in beginEditor()/
// refreshEditingFromSynth(), written back via Synth::setInstrumentPreset()/
// setDrumPreset() on every single field adjustment (see applyEditingToSynth()) --
// same "adjust, then immediately push" flow SettingsMode's own ranged
// settings (e.g. Reverb Mix) already use, so a change is heard right away,
// not just on some later "commit" step.
Synth::InstrumentPresetParams editingInst;
Synth::DrumPresetParams editingDrum;

enum InstField { FIELD_WAVEFORM, FIELD_ATTACK, FIELD_DECAY, FIELD_SUSTAIN, FIELD_RELEASE,
                  FIELD_CUTOFF, FIELD_VIBRATO, FIELD_TREMOLO, FIELD_PWM, INST_FIELD_COUNT };
enum DrumField { DFIELD_WAVEFORM, DFIELD_BASE_PITCH, DFIELD_DECAY, DFIELD_PITCHDROP_AMT,
                  DFIELD_PITCHDROP_TIME, DFIELD_CUTOFF, DRUM_FIELD_COUNT };
const int MAX_FIELD_COUNT = INST_FIELD_COUNT; // >= DRUM_FIELD_COUNT, sizes the shared scratch buffers below

int currentFieldCount() { return editTarget == EDIT_MELODIC ? (int)INST_FIELD_COUNT : (int)DRUM_FIELD_COUNT; }

const char* waveformLabel(Synth::SynthWaveform w) {
    switch (w) {
        case Synth::SYNTH_WAVE_SINE:     return "Sine";
        case Synth::SYNTH_WAVE_TRIANGLE: return "Triangle";
        case Synth::SYNTH_WAVE_SAW:      return "Saw";
        case Synth::SYNTH_WAVE_SQUARE:   return "Square";
        case Synth::SYNTH_WAVE_NOISE:    return "Noise";
        default:                          return "?";
    }
}

const char* fieldLabel(int index) {
    if (editTarget == EDIT_DRUM) {
        switch (index) {
            case DFIELD_WAVEFORM:       return "Waveform";
            case DFIELD_BASE_PITCH:     return "Base Pitch";
            case DFIELD_DECAY:          return "Decay";
            case DFIELD_PITCHDROP_AMT:  return "PitchDrop Amt";
            case DFIELD_PITCHDROP_TIME: return "PitchDrop Time";
            case DFIELD_CUTOFF:         return "Cutoff";
            default:                    return "";
        }
    }
    switch (index) {
        case FIELD_WAVEFORM: return "Waveform";
        case FIELD_ATTACK:   return "Attack";
        case FIELD_DECAY:    return "Decay";
        case FIELD_SUSTAIN:  return "Sustain";
        case FIELD_RELEASE:  return "Release";
        case FIELD_CUTOFF:   return "Cutoff";
        case FIELD_VIBRATO:  return "Vibrato";
        case FIELD_TREMOLO:  return "Tremolo";
        case FIELD_PWM:      return "PWM(Square)";
        default:             return "";
    }
}

// This build's snprintf doesn't render %f-style conversions (confirmed on
// real hardware: "%.1f%%" printed as a bare "%", the literal escape with
// nothing where the number should be) -- every field here is displayed as
// a manually-computed integer/tenths split instead, same reasoning
// FIELD_CUTOFF/DFIELD_BASE_PITCH/etc. already use their own (int) casts
// for. Vibrato/Tremolo/PWM step in 0.5 increments (see adjustField()), so
// one decimal digit is enough precision to show.
void formatPercentTenths(float v, char* out, size_t outSize) {
    long tenths = (long)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    snprintf(out, outSize, "%ld.%ld%%", tenths / 10, tenths % 10);
}

void formatFieldValue(int index, char* out, size_t outSize) {
    if (editTarget == EDIT_DRUM) {
        switch (index) {
            case DFIELD_WAVEFORM:       snprintf(out, outSize, "%s", waveformLabel(editingDrum.waveform)); break;
            case DFIELD_BASE_PITCH:     snprintf(out, outSize, "%dHz", (int)(editingDrum.basePitchHz + 0.5f)); break;
            case DFIELD_DECAY:          snprintf(out, outSize, "%dms", (int)(((long)editingDrum.decaySamples * 1000L) / SYNTH_SAMPLE_RATE)); break;
            case DFIELD_PITCHDROP_AMT:  snprintf(out, outSize, "%dHz", (int)(editingDrum.pitchDropStartHz + 0.5f)); break;
            case DFIELD_PITCHDROP_TIME: snprintf(out, outSize, "%dms", (int)(((long)editingDrum.pitchDropSamples * 1000L) / SYNTH_SAMPLE_RATE)); break;
            case DFIELD_CUTOFF:         snprintf(out, outSize, "%dHz", (int)(editingDrum.cutoffHz + 0.5f)); break;
            default: out[0] = '\0'; break;
        }
        return;
    }
    switch (index) {
        case FIELD_WAVEFORM: snprintf(out, outSize, "%s", waveformLabel(editingInst.waveform)); break;
        case FIELD_ATTACK:   snprintf(out, outSize, "%dms", (int)(((long)editingInst.attackSamples * 1000L) / SYNTH_SAMPLE_RATE)); break;
        case FIELD_DECAY:    snprintf(out, outSize, "%dms", (int)(((long)editingInst.decaySamples * 1000L) / SYNTH_SAMPLE_RATE)); break;
        case FIELD_SUSTAIN:  snprintf(out, outSize, "%d%%", editingInst.sustainPercent); break;
        case FIELD_RELEASE:  snprintf(out, outSize, "%dms", (int)(((long)editingInst.releaseSamples * 1000L) / SYNTH_SAMPLE_RATE)); break;
        case FIELD_CUTOFF:   snprintf(out, outSize, "%dHz", (int)(editingInst.cutoffHz + 0.5f)); break;
        case FIELD_VIBRATO:  formatPercentTenths(editingInst.vibratoDepthPercent, out, outSize); break;
        case FIELD_TREMOLO:  formatPercentTenths(editingInst.tremoloDepthPercent, out, outSize); break;
        case FIELD_PWM:      formatPercentTenths(editingInst.pwmDepthPercent, out, outSize); break;
        default: out[0] = '\0'; break;
    }
}

long clampL(long v, long lo, long hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
float clampF(float v, float lo, float hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void applyEditingToSynth() {
    if (editTarget == EDIT_MELODIC) Synth::setInstrumentPreset((uint8_t)editIndex, editingInst);
    else Synth::setDrumPreset((uint8_t)editIndex, editingDrum);
}

// `direction` is +1 (RIGHT/UP) or -1 (LEFT/DOWN), same convention as
// SettingsMode's own adjustSetting(). `multiplier` scales the step size --
// EDIT+UP/DOWN passes FIELD_FAST_MULTIPLIER (see handleFieldAdjust()),
// EDIT+LEFT/RIGHT leaves it at the default of 1. Every field here is
// ranged or a small cycled enum (waveform) -- none are boolean, unlike
// Settings, so there's no isBoolSetting() split to make.
void adjustField(int index, int direction, long multiplier = 1) {
    long dir = (long)direction * multiplier;
    if (editTarget == EDIT_DRUM) {
        switch (index) {
            case DFIELD_WAVEFORM: {
                int w = clampL((long)editingDrum.waveform + dir, 0, (long)Synth::SYNTH_WAVE_NOISE);
                editingDrum.waveform = (Synth::SynthWaveform)w;
                break;
            }
            case DFIELD_BASE_PITCH:     editingDrum.basePitchHz = clampF(editingDrum.basePitchHz + dir * 5.0f, 20.0f, 4000.0f); break;
            case DFIELD_DECAY:          editingDrum.decaySamples = (uint16_t)clampL((long)editingDrum.decaySamples + dir * 441L, 0, 65535); break;
            case DFIELD_PITCHDROP_AMT:  editingDrum.pitchDropStartHz = clampF(editingDrum.pitchDropStartHz + dir * 5.0f, 0.0f, 4000.0f); break;
            case DFIELD_PITCHDROP_TIME: editingDrum.pitchDropSamples = (uint16_t)clampL((long)editingDrum.pitchDropSamples + dir * 441L, 0, 65535); break;
            case DFIELD_CUTOFF:         editingDrum.cutoffHz = clampF(editingDrum.cutoffHz + dir * 100.0f, 100.0f, 20000.0f); break;
        }
        applyEditingToSynth();
        return;
    }
    switch (index) {
        case FIELD_WAVEFORM: {
            int w = clampL((long)editingInst.waveform + dir, 0, (long)Synth::SYNTH_WAVE_NOISE);
            editingInst.waveform = (Synth::SynthWaveform)w;
            break;
        }
        case FIELD_ATTACK:   editingInst.attackSamples = (uint16_t)clampL((long)editingInst.attackSamples + dir * 441L, 0, 65535); break;
        case FIELD_DECAY:    editingInst.decaySamples = (uint16_t)clampL((long)editingInst.decaySamples + dir * 441L, 0, 65535); break;
        case FIELD_SUSTAIN:  editingInst.sustainPercent = (uint8_t)clampL((long)editingInst.sustainPercent + dir * 5L, 0, 100); break;
        case FIELD_RELEASE:  editingInst.releaseSamples = (uint16_t)clampL((long)editingInst.releaseSamples + dir * 441L, 0, 65535); break;
        case FIELD_CUTOFF:   editingInst.cutoffHz = clampF(editingInst.cutoffHz + dir * 100.0f, 100.0f, 20000.0f); break;
        case FIELD_VIBRATO:  editingInst.vibratoDepthPercent = clampF(editingInst.vibratoDepthPercent + dir * 0.5f, 0.0f, 30.0f); break;
        case FIELD_TREMOLO:  editingInst.tremoloDepthPercent = clampF(editingInst.tremoloDepthPercent + dir * 0.5f, 0.0f, 30.0f); break;
        case FIELD_PWM:      editingInst.pwmDepthPercent = clampF(editingInst.pwmDepthPercent + dir * 0.5f, 0.0f, 30.0f); break;
    }
    applyEditingToSynth();
}

void editorPageTitle(char* out, size_t outSize) {
    if (editTarget == EDIT_DRUM) snprintf(out, outSize, "Drum:%s", Synth::drumPresetName((uint8_t)editIndex));
    else snprintf(out, outSize, "%s", Synth::instrumentFamilyName((uint8_t)editIndex));
}

void buildFieldLists(const char* labelPtrs[MAX_FIELD_COUNT], char values[MAX_FIELD_COUNT][16], const char* valuePtrs[MAX_FIELD_COUNT]) {
    int count = currentFieldCount();
    for (int i = 0; i < count; i++) {
        labelPtrs[i] = fieldLabel(i);
        formatFieldValue(i, values[i], sizeof(values[i]));
        valuePtrs[i] = values[i];
    }
}

void refreshEditingFromSynth() {
    if (editTarget == EDIT_MELODIC) Synth::getInstrumentPreset((uint8_t)editIndex, editingInst);
    else Synth::getDrumPreset((uint8_t)editIndex, editingDrum);
}

void beginEditor(EditTarget target, int index) {
    editTarget = target;
    editIndex = index;
    refreshEditingFromSynth();
    fieldCursor = 0;
    scrollOffset = 0;
    screen = SCREEN_EDITOR;
    needsRedraw = true;
}

void ensureFieldVisible() {
    int rows = Ui::visibleRows();
    if (rows <= 0) return;
    if (fieldCursor < scrollOffset || fieldCursor >= scrollOffset + rows) {
        scrollOffset = (fieldCursor / rows) * rows;
    }
    if (scrollOffset < 0) scrollOffset = 0;
}

void moveFieldCursor(int newCursor) {
    int prevCursor = fieldCursor;
    int prevScroll = scrollOffset;
    fieldCursor = newCursor;
    ensureFieldVisible();
    if (scrollOffset == prevScroll) {
        const char* labelPtrs[MAX_FIELD_COUNT];
        char values[MAX_FIELD_COUNT][16];
        const char* valuePtrs[MAX_FIELD_COUNT];
        buildFieldLists(labelPtrs, values, valuePtrs);
        Ui::updateSettingsSelection(labelPtrs, valuePtrs, currentFieldCount(), prevCursor, fieldCursor, scrollOffset);
    } else {
        needsRedraw = true;
    }
}

void refreshFieldValue() {
    char valBuf[16];
    formatFieldValue(fieldCursor, valBuf, sizeof(valBuf));
    Ui::updateSettingsValue(fieldLabel(fieldCursor), valBuf, fieldCursor, scrollOffset);
}

// How much bigger a single EDIT+UP/DOWN step is than EDIT+LEFT/RIGHT's --
// applied as a flat multiplier on top of whatever step size the field
// itself already uses (see adjustField()), rather than a separate
// hardcoded "fast" constant per field. No convention existed for this
// before; 10x was chosen as a simple, uniform starting point -- large
// enough to meaningfully skip ahead on wide-range fields (Cutoff Hz,
// Attack/Decay/Release) without needing per-field tuning, and it falls
// out of the same clamping every field already does that a 5-way enum
// field (Waveform) sensibly become "jump to the nearest extreme" rather
// than needing its own special case.
const long FIELD_FAST_MULTIPLIER = 10;

// EDIT held + LEFT/RIGHT/UP/DOWN: adjusts the selected field, UP/DOWN at
// FIELD_FAST_MULTIPLIER the rate LEFT/RIGHT does. Same hold-to-accelerate
// shape as SettingsMode's handleAdjust() for its ranged items (120ms
// repeat, dropping to 40ms after 2s held), independently timed per button
// pair so holding one doesn't affect the other's own ramp-up -- every
// field here is ranged/cycled, so there's no bool-item tap-only branch to
// mirror.
void handleFieldAdjust() {
    if (!Input::isDown(BTN_EDIT)) return;

    const uint32_t NORMAL_INTERVAL_MS = 120;
    const uint32_t FAST_INTERVAL_MS = 40;
    const uint32_t ACCEL_AFTER_MS = 2000;

    static uint32_t rightPressedAtMs = 0, leftPressedAtMs = 0;
    static uint32_t lastRightStep = 0, lastLeftStep = 0;
    static uint32_t upPressedAtMs = 0, downPressedAtMs = 0;
    static uint32_t lastUpStep = 0, lastDownStep = 0;
    uint32_t now = millis();

    if (Input::justPressed(BTN_RIGHT)) rightPressedAtMs = now;
    if (Input::justPressed(BTN_LEFT)) leftPressedAtMs = now;
    if (Input::justPressed(BTN_UP)) upPressedAtMs = now;
    if (Input::justPressed(BTN_DOWN)) downPressedAtMs = now;

    bool changed = false;
    if (Input::isDown(BTN_RIGHT)) {
        uint32_t interval = (now - rightPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_RIGHT) || now - lastRightStep >= interval) {
            adjustField(fieldCursor, 1);
            lastRightStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_LEFT)) {
        uint32_t interval = (now - leftPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_LEFT) || now - lastLeftStep >= interval) {
            adjustField(fieldCursor, -1);
            lastLeftStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_UP)) {
        uint32_t interval = (now - upPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_UP) || now - lastUpStep >= interval) {
            adjustField(fieldCursor, 1, FIELD_FAST_MULTIPLIER);
            lastUpStep = now;
            changed = true;
        }
    }
    if (Input::isDown(BTN_DOWN)) {
        uint32_t interval = (now - downPressedAtMs >= ACCEL_AFTER_MS) ? FAST_INTERVAL_MS : NORMAL_INTERVAL_MS;
        if (Input::justPressed(BTN_DOWN) || now - lastDownStep >= interval) {
            adjustField(fieldCursor, -1, FIELD_FAST_MULTIPLIER);
            lastDownStep = now;
            changed = true;
        }
    }
    if (changed) refreshFieldValue();
}

// -- Bank save/load/reset (mirrors SettingsMode's Theme editor Save/Load/
// Reset flow almost line for line -- same file-per-set-of-records pattern,
// same on-screen-keyboard naming + overwrite-confirm, same folder scan) --

char g_flashMsg[32] = {0};
uint32_t g_flashUntilMs = 0;

// Shows `msg` for a bit, then returns to SCREEN_EDITOR_MENU (see
// handleInput()'s SCREEN_FLASH case) -- same idea as SettingsMode's own
// showThemeFlash().
void showFlash(const char* msg) {
    strncpy(g_flashMsg, msg, sizeof(g_flashMsg) - 1);
    g_flashMsg[sizeof(g_flashMsg) - 1] = '\0';
    g_flashUntilMs = millis() + 1200;
    screen = SCREEN_FLASH;
    needsRedraw = true;
}

const char* const EDITOR_MENU_LABELS[] = {"Save Bank", "Load Bank", "Reset to Default"};
const int EDITOR_MENU_COUNT = 3;
int editorMenuCursor = 0;

const char* SYNTH_BANKS_ROOT = "/synth";
const int BANK_NAME_MAX_LEN = 16;
char bankNameBuf[BANK_NAME_MAX_LEN + 1] = {0};
int bankNameLen = 0;
int bankKeyRow = 1, bankKeyCol = 0; // on-screen keyboard cursor, see keyboard_layout.h
char bankNameError[40] = {0};
char pendingOverwritePath[64] = {0}; // SCREEN_SAVE_OVERWRITE only

const int MAX_SAVED_BANKS = 64; // comfortably above realistic use -- same "bump the constant" precedent MIDI_MAX_TRACKS documents
char bankLoadNames[MAX_SAVED_BANKS][BANK_NAME_MAX_LEN + 1];
int bankLoadCount = 0;
int bankLoadCursor = 0;
int bankLoadScrollOffset = 0; // see ensureBankLoadVisible()

const char* ACTIVE_BANK_PATH = "/parisynth.txt";
char activeBankName[BANK_NAME_MAX_LEN + 1] = {0};

void persistActiveBankName() {
    FsFile file;
    if (!file.open(ACTIVE_BANK_PATH, O_RDWR | O_CREAT | O_TRUNC)) return;
    char line[BANK_NAME_MAX_LEN + 16];
    int n = snprintf(line, sizeof(line), "activeBank=%s\n", activeBankName);
    file.write((const uint8_t*)line, n);
    file.sync();
    file.close();
}

void loadActiveBankNameFromDisk() {
    FsFile file;
    if (!file.open(ACTIVE_BANK_PATH, O_RDONLY)) return;
    char line[64];
    int lineLen = 0;
    int c;
    while ((c = file.read()) >= 0) {
        if (c == '\n') {
            line[lineLen] = '\0';
            if (strncmp(line, "activeBank=", 11) == 0) {
                strncpy(activeBankName, line + 11, BANK_NAME_MAX_LEN);
                activeBankName[BANK_NAME_MAX_LEN] = '\0';
            }
            lineLen = 0;
        } else if (c != '\r' && lineLen < (int)sizeof(line) - 1) {
            line[lineLen++] = (char)c;
        }
    }
    file.close();
}

// Fixed-point encode/decode for every float preset field, used by both
// directions of the .syn file format below -- snprintf's %f doesn't render
// on this build (see formatPercentTenths()'s own comment on the identical
// on-screen-text limitation), and sscanf's %f is the same C library
// function family, so it can't be trusted to parse a decimal literal back
// either even if one somehow ended up in a file. Every float is written/
// read as its value x100, rounded to the nearest integer, via plain %ld/
// %ld instead -- two decimal digits of precision, comfortably more than
// any field here actually steps by (vibrato/tremolo/PWM move in 0.5
// increments, cutoff/pitch fields in whole Hz).
long floatToHundredths(float v) {
    return (long)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
}
float hundredthsToFloat(long h) {
    return (float)h / 100.0f;
}

// Writes all 16 melodic + 8 drum presets as "Label=v1,v2,...\n" -- one line
// per record, labeled by name (family name, or "Drum:<name>") rather than
// position, so a future field addition/reorder doesn't corrupt old banks
// (same reasoning SettingsMode's saveThemeToFile() documents).
bool saveBankToFile(const char* path) {
    FsFile file;
    if (!file.open(path, O_RDWR | O_CREAT | O_TRUNC)) return false;
    char line[96];
    for (int i = 0; i < Synth::INSTRUMENT_FAMILY_COUNT; i++) {
        Synth::InstrumentPresetParams p;
        Synth::getInstrumentPreset((uint8_t)i, p);
        int n = snprintf(line, sizeof(line), "%s=%d,%d,%d,%d,%d,%ld,%ld,%ld,%ld\n",
                          Synth::instrumentFamilyName((uint8_t)i), (int)p.waveform,
                          (int)p.attackSamples, (int)p.decaySamples, (int)p.sustainPercent, (int)p.releaseSamples,
                          floatToHundredths(p.cutoffHz), floatToHundredths(p.vibratoDepthPercent),
                          floatToHundredths(p.tremoloDepthPercent), floatToHundredths(p.pwmDepthPercent));
        file.write((const uint8_t*)line, n);
    }
    for (int i = 0; i < Synth::DRUM_PRESET_COUNT; i++) {
        Synth::DrumPresetParams p;
        Synth::getDrumPreset((uint8_t)i, p);
        char label[24];
        snprintf(label, sizeof(label), "Drum:%s", Synth::drumPresetName((uint8_t)i));
        int n = snprintf(line, sizeof(line), "%s=%d,%ld,%d,%ld,%d,%ld\n",
                          label, (int)p.waveform, floatToHundredths(p.basePitchHz), (int)p.decaySamples,
                          floatToHundredths(p.pitchDropStartHz), (int)p.pitchDropSamples, floatToHundredths(p.cutoffHz));
        file.write((const uint8_t*)line, n);
    }
    file.sync();
    file.close();
    return true;
}

// Reverses saveBankToFile() -- unrecognized labels (older/newer firmware,
// hand-edited typo) are silently skipped rather than failing the whole
// load, same graceful-fallback approach SettingsMode's loadThemeFromFile()
// uses for its own text files.
bool loadBankFromFile(const char* path) {
    FsFile file;
    if (!file.open(path, O_RDONLY)) return false;

    char line[96];
    int lineLen = 0;
    int c;
    bool any = false;
    while ((c = file.read()) >= 0) {
        if (c == '\n') {
            line[lineLen] = '\0';
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                const char* label = line;
                const char* rest = eq + 1;
                bool matched = false;
                for (int i = 0; i < Synth::INSTRUMENT_FAMILY_COUNT && !matched; i++) {
                    if (strcmp(label, Synth::instrumentFamilyName((uint8_t)i)) == 0) {
                        matched = true;
                        int wf, a, d, s, r;
                        long cf, vib, trem, pwm;
                        if (sscanf(rest, "%d,%d,%d,%d,%d,%ld,%ld,%ld,%ld", &wf, &a, &d, &s, &r, &cf, &vib, &trem, &pwm) == 9) {
                            Synth::InstrumentPresetParams p;
                            p.waveform = (Synth::SynthWaveform)wf;
                            p.attackSamples = (uint16_t)a;
                            p.decaySamples = (uint16_t)d;
                            p.sustainPercent = (uint8_t)s;
                            p.releaseSamples = (uint16_t)r;
                            p.cutoffHz = hundredthsToFloat(cf);
                            p.vibratoDepthPercent = hundredthsToFloat(vib);
                            p.tremoloDepthPercent = hundredthsToFloat(trem);
                            p.pwmDepthPercent = hundredthsToFloat(pwm);
                            Synth::setInstrumentPreset((uint8_t)i, p);
                            any = true;
                        }
                    }
                }
                for (int i = 0; i < Synth::DRUM_PRESET_COUNT && !matched; i++) {
                    char want[24];
                    snprintf(want, sizeof(want), "Drum:%s", Synth::drumPresetName((uint8_t)i));
                    if (strcmp(label, want) == 0) {
                        matched = true;
                        int wf, dec, pdt;
                        long bp, pds, cf;
                        if (sscanf(rest, "%d,%ld,%d,%ld,%d,%ld", &wf, &bp, &dec, &pds, &pdt, &cf) == 6) {
                            Synth::DrumPresetParams p;
                            p.waveform = (Synth::SynthWaveform)wf;
                            p.basePitchHz = hundredthsToFloat(bp);
                            p.decaySamples = (uint16_t)dec;
                            p.pitchDropStartHz = hundredthsToFloat(pds);
                            p.pitchDropSamples = (uint16_t)pdt;
                            p.cutoffHz = hundredthsToFloat(cf);
                            Synth::setDrumPreset((uint8_t)i, p);
                            any = true;
                        }
                    }
                }
            }
            lineLen = 0;
        } else if (c != '\r' && lineLen < (int)sizeof(line) - 1) {
            line[lineLen++] = (char)c;
        }
    }
    file.close();
    return any;
}

// Populates bankLoadNames[]/bankLoadCount from SYNTH_BANKS_ROOT's *.syn
// files, sorted alphabetically -- same insertion-sort-while-scanning
// approach SettingsMode's scanThemesFolder() uses.
void scanBanksFolder() {
    bankLoadCount = 0;
    FsFile dir = sd.open(SYNTH_BANKS_ROOT);
    if (!dir || !dir.isDir()) return;

    FsFile entry;
    char name[32];
    while (entry.openNext(&dir, O_RDONLY) && bankLoadCount < MAX_SAVED_BANKS) {
        if (!entry.isDir()) {
            entry.getName(name, sizeof(name));
            size_t len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".syn") == 0) {
                name[len - 4] = '\0';
                int insertAt = bankLoadCount;
                while (insertAt > 0 && strcasecmp(bankLoadNames[insertAt - 1], name) > 0) {
                    strncpy(bankLoadNames[insertAt], bankLoadNames[insertAt - 1], BANK_NAME_MAX_LEN);
                    insertAt--;
                }
                strncpy(bankLoadNames[insertAt], name, BANK_NAME_MAX_LEN);
                bankLoadNames[insertAt][BANK_NAME_MAX_LEN] = '\0';
                bankLoadCount++;
            }
        }
        entry.close();
    }
    if (dir) dir.close();
}

// Same page-snap scroll-window convention as ensurePickerVisible()/
// SettingsMode's ensureThemeVisible().
void ensureBankLoadVisible() {
    int rows = Ui::visibleRows();
    if (rows <= 0) return;
    if (bankLoadCursor < bankLoadScrollOffset || bankLoadCursor >= bankLoadScrollOffset + rows) {
        bankLoadScrollOffset = (bankLoadCursor / rows) * rows;
    }
    if (bankLoadScrollOffset < 0) bankLoadScrollOffset = 0;
}

void beginEditorMenu() {
    editorMenuCursor = 0;
    screen = SCREEN_EDITOR_MENU;
    Ui::drawEntryMenu("Bank", EDITOR_MENU_LABELS, EDITOR_MENU_COUNT, editorMenuCursor);
}

void beginBankLoadPick() {
    scanBanksFolder();
    bankLoadCursor = 0;
    bankLoadScrollOffset = 0;
    screen = SCREEN_LOAD_PICK;
    needsRedraw = true;
}

void beginBankSaveName() {
    bankNameBuf[0] = '\0';
    bankNameLen = 0;
    bankNameError[0] = '\0';
    bankKeyRow = 1; // top-left letter key ('Q')
    bankKeyCol = 0;
    screen = SCREEN_SAVE_NAME;
    needsRedraw = true;
}

void finishBankSaveName() {
    char trimmed[BANK_NAME_MAX_LEN + 1];
    strncpy(trimmed, bankNameBuf, sizeof(trimmed));
    trimmed[BANK_NAME_MAX_LEN] = '\0';
    int len = (int)strlen(trimmed);
    while (len > 0 && trimmed[len - 1] == ' ') trimmed[--len] = '\0';
    if (len == 0) return; // nothing typed -- stay on the naming screen

    if (!sd.exists(SYNTH_BANKS_ROOT)) sd.mkdir(SYNTH_BANKS_ROOT);

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.syn", SYNTH_BANKS_ROOT, trimmed);

    if (sd.exists(path)) {
        strncpy(pendingOverwritePath, path, sizeof(pendingOverwritePath) - 1);
        pendingOverwritePath[sizeof(pendingOverwritePath) - 1] = '\0';
        screen = SCREEN_SAVE_OVERWRITE;
        needsRedraw = true;
        return;
    }

    if (saveBankToFile(path)) {
        strncpy(activeBankName, trimmed, BANK_NAME_MAX_LEN);
        activeBankName[BANK_NAME_MAX_LEN] = '\0';
        persistActiveBankName();
        showFlash("Bank saved");
    } else {
        strncpy(bankNameError, "save failed", sizeof(bankNameError) - 1);
        Ui::updateNameEntryError(bankNameError);
    }
}

void handleBankSaveNameInput() {
    int dRow = 0, dCol = 0;
    if (Input::justPressed(BTN_UP)) dRow = -1;
    if (Input::justPressed(BTN_DOWN)) dRow = 1;
    if (Input::justPressed(BTN_LEFT)) dCol = -1;
    if (Input::justPressed(BTN_RIGHT)) dCol = 1;
    if (dRow != 0 || dCol != 0) {
        int newRow = bankKeyRow + dRow;
        if (newRow < 0) newRow = 0;
        if (newRow >= KEYBOARD_ROW_COUNT) newRow = KEYBOARD_ROW_COUNT - 1;
        int newCol = (dRow != 0) ? bankKeyCol : bankKeyCol + dCol;
        if (newCol < 0) newCol = 0;
        if (newCol >= KEYBOARD_ROW_LENS[newRow]) newCol = KEYBOARD_ROW_LENS[newRow] - 1;
        if (newRow != bankKeyRow || newCol != bankKeyCol) {
            int prevRow = bankKeyRow, prevCol = bankKeyCol;
            bankKeyRow = newRow;
            bankKeyCol = newCol;
            Ui::updateNameEntryKey(prevRow, prevCol, bankKeyRow, bankKeyCol);
        }
    }

    if (Input::justPressed(BTN_NAV)) {
        screen = SCREEN_EDITOR_MENU;
        needsRedraw = true;
        return;
    }

    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        const KeyDef& key = KEYBOARD_ROWS[bankKeyRow][bankKeyCol];
        bool hadError = (bankNameError[0] != '\0');
        bankNameError[0] = '\0';
        switch (key.kind) {
            case KEY_CHAR:
                if (bankNameLen < BANK_NAME_MAX_LEN) {
                    bankNameBuf[bankNameLen++] = key.ch;
                    bankNameBuf[bankNameLen] = '\0';
                    Ui::updateNameEntryPreview(bankNameBuf, "");
                    if (hadError) Ui::updateNameEntryError(nullptr);
                }
                break;
            case KEY_DEL:
                if (bankNameLen > 0) {
                    bankNameBuf[--bankNameLen] = '\0';
                    Ui::updateNameEntryPreview(bankNameBuf, "");
                    if (hadError) Ui::updateNameEntryError(nullptr);
                }
                break;
            case KEY_OK:
                finishBankSaveName();
                break;
        }
    }
}

void handleBankSaveOverwriteInput() {
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_SAVE_NAME;
        needsRedraw = true;
        return;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        sd.remove(pendingOverwritePath);
        if (saveBankToFile(pendingOverwritePath)) {
            strncpy(activeBankName, bankNameBuf, BANK_NAME_MAX_LEN);
            activeBankName[BANK_NAME_MAX_LEN] = '\0';
            persistActiveBankName();
            showFlash("Bank saved");
        } else {
            strncpy(bankNameError, "save failed", sizeof(bankNameError) - 1);
            screen = SCREEN_SAVE_NAME;
            needsRedraw = true;
        }
    }
}

// Returns true when the user backs all the way out (NAV/LEFT) -- this menu
// is now only ever reached directly (SynthEditor::openBankMenu(), see
// synth_editor.h), never via the field editor, so backing out exits
// SynthEditor entirely rather than landing on a field editor screen that
// has nothing to do with how this menu was opened.
bool handleEditorMenuInput() {
    if (Input::justPressed(BTN_UP)) {
        int prev = editorMenuCursor;
        editorMenuCursor = (editorMenuCursor > 0) ? editorMenuCursor - 1 : EDITOR_MENU_COUNT - 1;
        Ui::updateEntryMenuSelection(EDITOR_MENU_LABELS, EDITOR_MENU_COUNT, prev, editorMenuCursor);
    }
    if (Input::justPressed(BTN_DOWN)) {
        int prev = editorMenuCursor;
        editorMenuCursor = (editorMenuCursor < EDITOR_MENU_COUNT - 1) ? editorMenuCursor + 1 : 0;
        Ui::updateEntryMenuSelection(EDITOR_MENU_LABELS, EDITOR_MENU_COUNT, prev, editorMenuCursor);
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        return true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (editorMenuCursor == 0) {
            beginBankSaveName();
        } else if (editorMenuCursor == 1) {
            beginBankLoadPick();
        } else {
            Synth::resetAllPresetsToDefault();
            activeBankName[0] = '\0';
            persistActiveBankName();
            refreshEditingFromSynth();
            showFlash("Reset to default");
        }
    }
    return false;
}

void handleBankLoadPickInput() {
    if (Input::justPressed(BTN_UP) && bankLoadCount > 0) {
        bankLoadCursor = (bankLoadCursor > 0) ? bankLoadCursor - 1 : bankLoadCount - 1;
        ensureBankLoadVisible();
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_DOWN) && bankLoadCount > 0) {
        bankLoadCursor = (bankLoadCursor < bankLoadCount - 1) ? bankLoadCursor + 1 : 0;
        ensureBankLoadVisible();
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        screen = SCREEN_EDITOR_MENU;
        Ui::drawEntryMenu("Bank", EDITOR_MENU_LABELS, EDITOR_MENU_COUNT, editorMenuCursor);
        return;
    }
    if ((Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) && bankLoadCount > 0) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s.syn", SYNTH_BANKS_ROOT, bankLoadNames[bankLoadCursor]);
        if (loadBankFromFile(path)) {
            strncpy(activeBankName, bankLoadNames[bankLoadCursor], BANK_NAME_MAX_LEN);
            activeBankName[BANK_NAME_MAX_LEN] = '\0';
            persistActiveBankName();
            refreshEditingFromSynth();
            showFlash("Bank loaded");
        } else {
            showFlash("Load failed");
        }
    }
}

// Returns true when the user backs all the way out to the host (NAV/LEFT
// from the field editor -- see synth_editor.h's comment on why this
// doesn't distinguish "reached via the picker" from "opened directly").
bool handleEditorInput() {
    if (Input::isDown(BTN_EDIT)) {
        handleFieldAdjust();
        return false;
    }
    if (Input::justPressed(BTN_UP)) {
        moveFieldCursor(fieldCursor > 0 ? fieldCursor - 1 : currentFieldCount() - 1);
    }
    if (Input::justPressed(BTN_DOWN)) {
        moveFieldCursor(fieldCursor < currentFieldCount() - 1 ? fieldCursor + 1 : 0);
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        return true;
    }
    return false;
}

// -- Instrument picker ----------------------------------------------------

// Returns true when the user backs out of the picker with nothing opened
// into the editor (NAV/LEFT). Cursor moves always trigger a full redraw
// (not a cheap partial update) -- same convention SettingsMode's own
// Theme/Bank load-pick lists already use for a scrollable entry menu,
// since a move can cross a scroll-window boundary and there's no user-
// visible cost to always repainting a list this short a screenful of.
bool handlePickerInput() {
    if (Input::justPressed(BTN_UP)) {
        pickerCursor = (pickerCursor > 0) ? pickerCursor - 1 : PICKER_COUNT - 1;
        ensurePickerVisible();
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_DOWN)) {
        pickerCursor = (pickerCursor < PICKER_COUNT - 1) ? pickerCursor + 1 : 0;
        ensurePickerVisible();
        needsRedraw = true;
    }
    if (Input::justPressed(BTN_NAV) || Input::justPressed(BTN_LEFT)) {
        return true;
    }
    if (Input::justPressed(BTN_ENTER) || Input::justPressed(BTN_PLAY)) {
        if (pickerCursor < Synth::INSTRUMENT_FAMILY_COUNT) beginEditor(EDIT_MELODIC, pickerCursor);
        else beginEditor(EDIT_DRUM, pickerCursor - Synth::INSTRUMENT_FAMILY_COUNT);
    }
    return false;
}

bool handleInput() {
    switch (screen) {
        case SCREEN_PICKER:
            return handlePickerInput();
        case SCREEN_EDITOR:
            return handleEditorInput();
        case SCREEN_EDITOR_MENU:
            return handleEditorMenuInput();
        case SCREEN_SAVE_NAME:
            handleBankSaveNameInput();
            return false;
        case SCREEN_SAVE_OVERWRITE:
            handleBankSaveOverwriteInput();
            return false;
        case SCREEN_LOAD_PICK:
            handleBankLoadPickInput();
            return false;
        case SCREEN_FLASH:
            // Back to the bank menu, not the field editor -- see
            // handleEditorMenuInput()'s own comment on why this whole
            // chain no longer has any field editor screen to return to.
            if (millis() >= g_flashUntilMs) {
                screen = SCREEN_EDITOR_MENU;
                needsRedraw = true;
            }
            return false;
    }
    return false;
}

} // namespace

void begin() {
    populatePickerLabels();
    loadActiveBankNameFromDisk();
    if (activeBankName[0] != '\0') {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s.syn", SYNTH_BANKS_ROOT, activeBankName);
        loadBankFromFile(path);
    }
}

void openPicker(int startCursor) {
    pickerCursor = (startCursor >= 0 && startCursor < PICKER_COUNT) ? startCursor : 0;
    pickerScrollOffset = 0;
    ensurePickerVisible();
    screen = SCREEN_PICKER;
    needsRedraw = true;
}

void openForInstrument(uint8_t family) {
    beginEditor(EDIT_MELODIC, family);
}

void openForDrum(uint8_t drumType) {
    beginEditor(EDIT_DRUM, drumType);
}

void openBankMenu() {
    beginEditorMenu();
}

bool update() {
    if (handleInput()) return true;

    if (needsRedraw) {
        needsRedraw = false;
        switch (screen) {
            case SCREEN_PICKER:
                Ui::drawEntryMenu("Edit Instrument", pickerLabelPtrs, PICKER_COUNT, pickerCursor, pickerScrollOffset);
                break;
            case SCREEN_EDITOR: {
                char title[24];
                editorPageTitle(title, sizeof(title));
                const char* labelPtrs[MAX_FIELD_COUNT];
                char values[MAX_FIELD_COUNT][16];
                const char* valuePtrs[MAX_FIELD_COUNT];
                buildFieldLists(labelPtrs, values, valuePtrs);
                // Own footer text, not drawSettings()'s SettingsMode-flavored
                // default -- this screen doesn't page (LEFT/RIGHT do nothing
                // here, unlike Settings).
                Ui::drawSettings(labelPtrs, valuePtrs, currentFieldCount(), fieldCursor, scrollOffset, title,
                                  "UD move  EDIT+LR change", "EDIT+UD fast change");
                break;
            }
            case SCREEN_EDITOR_MENU:
                Ui::drawEntryMenu("Bank", EDITOR_MENU_LABELS, EDITOR_MENU_COUNT, editorMenuCursor);
                break;
            case SCREEN_SAVE_NAME:
                Ui::drawNameEntry("Save Bank", bankNameBuf, "",
                                   bankNameError[0] ? bankNameError : nullptr,
                                   bankKeyRow, bankKeyCol);
                break;
            case SCREEN_SAVE_OVERWRITE: {
                const char* slash = strrchr(pendingOverwritePath, '/');
                const char* name = slash ? slash + 1 : pendingOverwritePath;
                Ui::drawConfirmOverwrite(name, false);
                break;
            }
            case SCREEN_LOAD_PICK:
                if (bankLoadCount > 0) {
                    const char* labels[MAX_SAVED_BANKS];
                    for (int i = 0; i < bankLoadCount; i++) labels[i] = bankLoadNames[i];
                    Ui::drawEntryMenu("Load Bank", labels, bankLoadCount, bankLoadCursor, bankLoadScrollOffset);
                } else {
                    Ui::drawMessage("No saved banks", "NAV back");
                }
                break;
            case SCREEN_FLASH:
                Ui::drawMessage(g_flashMsg, nullptr);
                break;
        }
    }
    return false;
}

} // namespace SynthEditor

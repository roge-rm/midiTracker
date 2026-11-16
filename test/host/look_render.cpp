// Host render of the M12 look: draws the stage 2 and 4 screens with the real
// ui_look.cpp widgets into a framebuffer and writes PNGs, 1x and 2x.
//   make -C test/host look OUT_DIR=~/debug/midiTracker/m12-2
// The screens are put together here the way ui.cpp does it, with made-up
// data, so the layout can be checked without the device.
#include "ui_look.h"
#include "ui_player.h"
#include "ui_modes.h"
#include "ui_rec.h"
#include "ui_tap.h"
#include "ui_saver.h"
#include "synth.h"
#include "midi_output.h"
#include "host_snap.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <vector>

uint16_t g_hostFrame[240][320];
uint32_t g_hostMs = 1000;

namespace {
TFT_eSPI g_tft;
TFT_eSprite g_sprite(&g_tft);
}

namespace Ui {
TFT_eSprite& stripBegin(int x, int y, int w, int h, uint16_t bg) {
    g_sprite.ox = x;
    g_sprite.oy = y;
    g_sprite.ow = w;
    g_sprite.oh = h;
    g_sprite.setViewport(0, 0, w, h, true);
    g_sprite.fillRect(0, 0, w, h, bg);
    return g_sprite;
}
void stripSend() {}
bool sliceAllows(DrawKind) { return true; }
bool sliceLeft() { return true; }
bool ringLow() { return false; }
void sliceCharge(uint32_t) {}
bool clipRect(int&, int&, int&, int&) { return true; }
int drawPass() { return 0; }
} // namespace Ui

// ---- the player screens' sources, played back from host renders ----------

namespace {
std::vector<int16_t> g_raw; // stereo frames of a render
uint32_t g_played = 0;
const HostSnap::MidiFrame* g_midiFrame = nullptr;
}

namespace Synth {
uint32_t wavStreamWritten() { return (uint32_t)(g_raw.size() / 2); }
uint32_t wavStreamPlayed() { return g_played; }
size_t wavStreamPeek(int16_t* out, size_t frames) {
    size_t have = g_raw.size() / 2 > g_played ? g_raw.size() / 2 - g_played : 0;
    size_t n = frames < have ? frames : have;
    memcpy(out, g_raw.data() + (size_t)g_played * 2, n * 4);
    return n;
}
}

namespace MidiOutput {
bool isNoteActive(uint8_t n) { return n < 128 && g_midiFrame && g_midiFrame->noteMask[n]; }
bool isNotePercussion(uint8_t n) { return n < 128 && g_midiFrame && (g_midiFrame->noteMask[n] & (1u << 9)); }
uint8_t noteVelocity(uint8_t n) { return n < 128 && g_midiFrame ? g_midiFrame->noteVel[n] : 0; }
uint16_t activeChannelMask() {
    uint16_t m = 0;
    for (int n = 0; g_midiFrame && n < 128; n++) m |= g_midiFrame->noteMask[n];
    return m;
}
}

using namespace Look;

namespace {

// ---- PNG, stored deflate ----------------------------------------------------

uint32_t crcTable[256];
void crcInit() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}
uint32_t crc(const std::vector<uint8_t>& d, size_t from) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = from; i < d.size(); i++) c = crcTable[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc(out, start));
}
void writePng(const std::string& path, int scale) {
    int W = 320 * scale, H = 240 * scale;
    std::vector<uint8_t> raw;
    for (int y = 0; y < H; y++) {
        raw.push_back(0);
        for (int x = 0; x < W; x++) {
            uint16_t c = g_hostFrame[y / scale][x / scale];
            uint8_t r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
            raw.push_back((r5 << 3) | (r5 >> 2));
            raw.push_back((g6 << 2) | (g6 >> 4));
            raw.push_back((b5 << 3) | (b5 >> 2));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, W); be32(ihdr, H);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); exit(1); }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

std::string g_outDir;
void save(const char* name) {
    writePng(g_outDir + "/" + name + ".png", 1);
    writePng(g_outDir + "/" + name + "@2x.png", 2);
    printf("%s/%s.png\n", g_outDir.c_str(), name);
}

// ---- screens, as ui.cpp builds them ----------------------------------------

const int ROWS = 10;

void browserScreen() {
    g_tft.fillScreen(COLOR_BG);
    Status st;
    st.icon = &ICON_FOLDER;
    st.iconColor = COLOR_ACCENT;
    st.title = "FILES";
    st.text = "/ music / ";
    st.textLast = "mods";
    st.rightLabel = "OUT";
    st.rightValue = "HW+USB";
    drawStatus(st);
    FileRow rows[] = {
        {"chiptune", ROW_DIR, nullptr, 0},
        {"skyscraper.xm", ROW_TRACKER, "XM", 212 * 1024},
        {"the reflex.s3m", ROW_TRACKER, "S3M", 488 * 1024},
        {"lucky star.it", ROW_TRACKER, "IT", 3250000},
        {"reflex.mid", ROW_MIDI, "MID", 38 * 1024},
        {"loop01.wav", ROW_WAV, "WAV", 1260000},
        {"axelf.mod", ROW_TRACKER, "MOD", 61 * 1024},
        {"juno patch.syx", ROW_SYX, "SYX", 4 * 1024},
        {"night drive.mp3", ROW_WAV, "MP3", 7340000},
        {"a very long file name that will not fit in the row.mid", ROW_MIDI, "MID", 1200},
    };
    for (int i = 0; i < ROWS; i++) drawFileRow(LIST_TOP + i * LIST_ROW_H, &rows[i], i == 1);
    drawScrollbar(LIST_TOP, ROWS, 34, 6);
    static const Key row1[] = {
        {"UP", "up", TONE_TEXT}, {"PLAY", "play", TONE_PLAY}, {"EDIT", "menu", TONE_TEXT},
        {"ALT", "out", TONE_TEXT}, {"NAV", "back", TONE_TEXT},
    };
    static const Key row2[] = {
        {"LEFT", "parent", TONE_TEXT}, {"DOWN", "down", TONE_TEXT},
        {"RIGHT", "open", TONE_TEXT}, {"ENT", "open", TONE_SELECT},
    };
    drawKeyFooter(row1, 5, row2, 4);
}

void modeScreen(bool ledIn, bool ledOut) {
    g_tft.fillScreen(COLOR_BG);
    statusLeds(ledIn, ledOut);
    statusBattery(90, true, false);
    Status st;
    st.title = "MIDITRACKER";
    st.titleColor = COLOR_ACCENT;
    st.note = "0.6.0";
    drawStatus(st);
    clearModeList();
    const char* labels[] = {"FILES", "LOOPER", "PARISYNTH", "SETTINGS"};
    const char* notes[] = {"play and record", "4 tracks", "16 channels", "midi \x7f synth \x7f theme"};
    for (int i = 0; i < 4; i++) drawModeRow(i, labels[i], notes[i], i == 0);
    static const Key keys[] = {{"UD", "move", TONE_TEXT}, {"RIGHT/ENT", "open", TONE_SELECT}};
    drawKeyFooter(keys, 2);
}

void menuScreen() {
    g_tft.fillScreen(COLOR_BG);
    Status st;
    st.text = "skyscraper.xm";
    drawStatus(st);
    const char* items[] = {"Play", "Rename", "Delete", "Load into Looper", "New Folder", "New Recording", "Capture SysEx"};
    for (int i = 0; i < ROWS; i++)
        drawTextRow(LIST_TOP + i * LIST_ROW_H, i < 7 ? items[i] : nullptr, nullptr, 0, i == 2);
    drawScrollbar(LIST_TOP, ROWS, 7, 0);
    drawHintFooter("UP/DN move  ENTER select", nullptr);
}

void settingsScreen() {
    g_tft.fillScreen(COLOR_BG);
    Status st;
    st.icon = &ICON_SLIDERS;
    st.iconColor = COLOR_DIM;
    st.title = "SETTINGS";
    st.text = "MIDI/System";
    drawStatus(st);
    drawIcon(g_tft, ICON_KNOBS, 6, (STATUS_H - 12) / 2, COLOR_TEXT);
    const char* labels[] = {"Clock Source", "MIDI Transport", "MIDI Thru", "Brightness", "Transitions",
                            "Screensaver", "Screensaver Style", "Theme", "USB Drive", "USB Bootloader"};
    const char* values[] = {"Internal", "Off", "Off", "80%", "Wipe", "5 min", "Auto", "ENTER to edit",
                            "ENTER to start", "Hold ENTER"};
    int y0 = HEADER_H + 4;
    for (int i = 0; i < ROWS; i++)
        drawTextRow(y0 + i * LIST_ROW_H, labels[i], values[i], 180, i == 5);
    drawScrollbar(y0, ROWS, 10, 0);
    drawHintFooter("UD move  LEFT back", "EDIT+LR change");
}

void settingsListScreen() {
    g_tft.fillScreen(COLOR_BG);
    Status st;
    st.icon = &ICON_SLIDERS;
    st.iconColor = COLOR_DIM;
    st.title = "SETTINGS";
    drawStatus(st);
    drawIcon(g_tft, ICON_KNOBS, 6, (STATUS_H - 12) / 2, COLOR_TEXT);
    clearModeList();
    const char* labels[] = {"AUDIO", "LOOPER", "METRONOME", "MIDI/SYSTEM"};
    const char* notes[] = {"level \x7f reverb \x7f LFO \x7f stereo", "tempo \x7f time signature \x7f bars \x7f sync",
                           "click \x7f volume \x7f count-in", "clock \x7f thru \x7f display \x7f theme \x7f USB"};
    const Icon* icons[] = {&ICON_WAVE, &ICON_LOOPER, &ICON_METRONOME, &ICON_NOTE};
    uint16_t colors[] = {COLOR_SELECT, COLOR_ACCENT, COLOR_TEXT, COLOR_MIDI};
    for (int i = 0; i < 4; i++) drawIconRow(i, *icons[i], colors[i], labels[i], notes[i], i == 3);
    static const Key keys[] = {{"UD", "move", TONE_TEXT}, {"RIGHT/ENT", "open", TONE_SELECT}};
    drawKeyFooter(keys, 2);
}

void dialogScreen() {
    browserScreen();
    dimBody();
    Card c;
    c.icon = &ICON_WARNING;
    c.border = COLOR_ERROR;
    c.title = "DELETE FILE";
    c.line1 = "skyscraper.xm";
    c.line2 = "this can't be undone";
    static const Key keys[] = {{"NAV", "cancel", TONE_TEXT}, {"ENT", "delete", TONE_ERROR}};
    c.buttons = keys;
    c.buttonCount = 2;
    drawCard(c);
}

void messageScreen() {
    dialogScreen();
    dimBody();
    Card c;
    c.line1 = "Discard this recording?";
    static const Key keys[] = {{"ENTER", "discard", TONE_SELECT}, {"NAV", "keep recording", TONE_TEXT}};
    c.buttons = keys;
    c.buttonCount = 2;
    drawCard(c);
}

// A player screen keeps its stage 1 body; this shows the shared strip and
// a hint footer from its old hint lines.
void playerChrome() {
    g_tft.fillScreen(COLOR_BG);
    statusLeds(false, true);
    statusBattery(55, false, false);
    Status st;
    st.icon = &ICON_TRACKER;
    st.iconColor = COLOR_ACCENT;
    st.title = "TRACKER";
    drawStatus(st);
    drawHintFooter("EDIT+UD vol  PLAY tog", "NAV stop");
}

// ---- looper and pariSynth (stage 4) -------------------------------------------

using namespace UiModes;

// Made-up notes for a lane: a phrase that repeats every bar.
void fakeDots(int track, uint32_t len, int bars, int perBar, int seed) {
    laneDotsBegin(track, len);
    for (int b = 0; b < bars; b++)
        for (int k = 0; k < perBar; k++) {
            uint32_t at = (uint32_t)((b * perBar + k) * (uint64_t)len / (bars * perBar));
            at += (uint32_t)((k * 37 + seed) % 60);
            laneDot(at, (uint8_t)(48 + (k * 7 + seed + b * 3) % 30));
        }
    laneDotsEnd();
}

LoopBpmRow g_lrow;
LoopTrackView g_ltr[4];

void looperSetup() {
    g_lrow = LoopBpmRow();
    g_lrow.bpm = 120;
    g_lrow.sync = true;
    g_lrow.metroOn = false;
    g_lrow.countIn = true;
    g_lrow.barMs = 2000;
    for (auto& t : g_ltr) t = LoopTrackView();
    g_ltr[0].channelIn = 16; g_ltr[0].channelOut = 2; g_ltr[0].state = LOOP_TRACK_RECORDING;
    g_ltr[0].barLength = 4; g_ltr[0].lengthMs = 8000; g_ltr[0].recordActive = true;
    g_ltr[1].channelIn = 4; g_ltr[1].channelOut = 16; g_ltr[1].state = LOOP_TRACK_PLAYING;
    g_ltr[1].lengthMs = 7300; g_ltr[1].playActive = true;
    g_ltr[2].channelIn = 9; g_ltr[2].channelOut = 9; g_ltr[2].state = LOOP_TRACK_MUTED;
    g_ltr[2].barLength = 2; g_ltr[2].lengthMs = 4000;
    g_ltr[3].channelIn = 16; g_ltr[3].channelOut = 3;
    fakeDots(0, 8000, 4, 3, 3);
    fakeDots(1, 7300, 4, 4, 11);
    fakeDots(2, 4000, 2, 4, 5);
    fakeDots(3, 0, 0, 0, 0);
}

// Moves the playheads from pos to pos2 in live steps, as the device would.
void looperRun(int selected, uint16_t from, uint16_t to) {
    for (int k = 0; k <= 8; k++) {
        uint16_t p = (uint16_t)(from + (to - from) * k / 8);
        g_ltr[0].pos = p;
        g_ltr[1].pos = (uint16_t)(p * 3 / 4 + 9000);
        g_hostMs += 40;
        looperLive(g_ltr, selected);
    }
}

void looperScreens() {
    looperSetup();
    g_tft.fillScreen(COLOR_BG);
    statusLeds(true, true);
    looperDraw(g_ltr, 0, g_lrow, true, 2);
    looperBarBeat(3, 2);
    looperRun(0, 20000, 38000);
    save("looper");

    // The BPM row with METRO focused, and a beat flash on the downbeat.
    g_lrow.selected = true;
    g_lrow.focus = 2;
    g_lrow.metroOn = true;
    looperSelection(g_ltr, 0, false, -1, true, g_lrow);
    looperBeats(true, 0, 4);
    looperBarBeat(4, 1);
    looperBeatFlash(true, true);
    looperRun(-1, 38000, 60000);
    save("looper-bpm");

    // EDIT held on METRO: the volume shows instead.
    g_lrow.showMetroVolume = true;
    g_lrow.metroVolume = 80;
    looperBpm(g_lrow);
    looperBeatFlash(false, false);
    // A track wraps and T4 starts its first pass, 2 bars set.
    g_ltr[3].state = LOOP_TRACK_RECORDING;
    g_ltr[3].barLength = 2;
    g_ltr[3].pos = 12000;
    looperTrackRow(g_ltr[3], 3, false);
    looperRun(-1, 60000, 9000);
    save("looper-wrap");

    g_tft.fillScreen(COLOR_BG);
    looperCountIn(3);
    save("looper-countin");
}

const char* const FAMILY[16] = {"Piano", "Chrom.Percus.", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
                                "Reed", "Pipe", "SynthLead", "SynthPad", "SynthFX", "Ethnic", "Percussive", "SoundFX"};
const uint8_t PARI_FAMILY[16] = {0, 4, 3, 5, 2, 7, 8, 9, 6, 16, 10, 11, 1, 13, 14, 15};

void synthScreen(bool volume) {
    static HostSnap::MidiFrame f;
    memset(&f, 0, sizeof(f));
    // A chord on 1, bass on 2, a pad on 12, drums on 10.
    const struct { int ch, note, vel; } held[] = {{0, 60, 100}, {0, 64, 90}, {0, 67, 96}, {1, 36, 120},
                                                 {11, 72, 70}, {11, 76, 64}, {9, 36, 127}, {9, 42, 90}};
    UiTap::g_midi = UiTap::Midi();
    g_midiFrame = &f;
    SynthCell cells[16];
    for (int ch = 0; ch < 16; ch++) {
        cells[ch].family = PARI_FAMILY[ch];
        cells[ch].name = ch == 9 ? "Drums" : FAMILY[PARI_FAMILY[ch]];
    }
    g_tft.fillScreen(COLOR_BG);
    statusLeds(true, false);
    synthDraw(cells, 2, 0);
    for (auto& h : held) {
        f.noteMask[h.note] |= (uint16_t)(1u << h.ch);
        f.noteVel[h.note] = (uint8_t)h.vel;
        UiTap::midiNoteOn((uint8_t)h.ch, (uint8_t)h.note, (uint8_t)h.vel);
    }
    uint16_t mask = MidiOutput::activeChannelMask();
    for (int ch = 0; ch < 16; ch++)
        if ((mask >> ch) & 1) synthCell(ch, cells[ch], ch == 2, true);
    for (int k = 0; k < 3; k++) {
        g_hostMs += 40;
        synthLive();
    }
    if (volume) synthVolume(75);
    g_midiFrame = nullptr;
}


// ---- synth editor ----------------------------------------------------------

void synthPicker() {
    g_tft.fillScreen(COLOR_BG);
    statusLeds(false, false);
    static const char* labels[24];
    static char drums[8][20];
    static uint8_t fams[24];
    const char* drumNames[8] = {"Kick", "Snare", "Closed Hat", "Open Hat", "Tom", "Clap", "Cymbal", "Perc"};
    for (int i = 0; i < 16; i++) {
        labels[i] = FAMILY[i];
        fams[i] = (uint8_t)i;
    }
    for (int i = 0; i < 8; i++) {
        snprintf(drums[i], sizeof(drums[i]), "Drum:%s", drumNames[i]);
        labels[16 + i] = drums[i];
        fams[16 + i] = 16;
    }
    synthPickDraw(labels, fams, 24, 12, 10);
}

void synthEditScreen(bool drum, int cursor) {
    g_tft.fillScreen(COLOR_BG);
    statusLeds(false, true);
    SynthEditView v;
    struct R { const char* label; const char* value; int fill; SynthEditPart part; };
    if (!drum) {
        const R rows[] = {{"Waveform", "Saw", -1, PART_WAVE}, {"Attack", "20ms", 76, PART_ATTACK},
                          {"Decay", "300ms", 450, PART_DECAY}, {"Sustain", "60%", 600, PART_SUSTAIN},
                          {"Release", "450ms", 552, PART_RELEASE}, {"Cutoff", "2400Hz", 600, PART_NONE},
                          {"Vibrato", "2.5%", 83, PART_NONE}, {"Tremolo", "0.0%", 0, PART_NONE},
                          {"PWM(Square)", "10.0%", 333, PART_NONE}};
        v.name = "SynthLead";
        v.family = 10;
        v.drum = false;
        v.wave = 2;
        v.attack = 882;
        v.decay = 13230;
        v.sustain = 60;
        v.release = 19845;
        v.count = 9;
        for (int i = 0; i < 9; i++) {
            v.rows[i].label = rows[i].label;
            snprintf(v.rows[i].value, sizeof(v.rows[i].value), "%s", rows[i].value);
            v.rows[i].fill = (int16_t)rows[i].fill;
            v.rows[i].part = rows[i].part;
        }
    } else {
        const R rows[] = {{"Waveform", "Sine", -1, PART_WAVE}, {"Base Pitch", "55Hz", 191, PART_PITCH},
                          {"Decay", "350ms", 486, PART_AMP}, {"PitchDrop Amt", "180Hz", 627, PART_PITCH},
                          {"PitchDrop Time", "60ms", 191, PART_PITCH}, {"Cutoff", "8000Hz", 827, PART_NONE}};
        v.name = "Drum:Kick";
        v.family = 16;
        v.drum = true;
        v.wave = 0;
        v.decay = 15435;
        v.dropTime = 2646;
        v.pitchTo = 169;   // 55 Hz on 20 Hz-8 kHz
        v.pitchFrom = 478; // 235 Hz
        v.count = 6;
        for (int i = 0; i < 6; i++) {
            v.rows[i].label = rows[i].label;
            snprintf(v.rows[i].value, sizeof(v.rows[i].value), "%s", rows[i].value);
            v.rows[i].fill = (int16_t)rows[i].fill;
            v.rows[i].part = rows[i].part;
        }
    }
    synthEditDraw(v, 0);
    if (cursor != 0) synthEditCursor(v, 0, cursor);
}

// The bank screens, as synth_editor.cpp calls them.
void synthBank(int cursor) {
    g_tft.fillScreen(COLOR_BG);
    statusLeds(false, false);
    synthBankDraw("my bank", 0);
    if (cursor != 0) synthBankCursor(0, cursor);
}

void synthName(const char* error) {
    g_tft.fillScreen(COLOR_BG);
    synthNameDraw("PADS", nullptr, 1, 2);
    synthNameText("PADS_2", 4); // typed two more
    synthNameKey(1, 2, 2, 5);
    if (error) synthNameError("PADS_2", error);
}

const char* const BANKS[] = {"bright leads", "default copy", "drums 808", "my bank", "organ set", "pads",
                             "PADS_2", "piano and strings", "a very long bank n", "test", "zz last"};

void synthLoad(int count, int cursor) {
    g_tft.fillScreen(COLOR_BG);
    synthLoadDraw(BANKS, count, 0, 0, "my bank");
    if (cursor != 0) synthLoadCursor(BANKS, count, 0, cursor, 0, "my bank");
}

void fontSheet() {
    g_tft.fillScreen(COLOR_BG);
    char line[40];
    int y = 6;
    for (int c = 0x20; c <= 0x7F; c += 32) {
        int n = 0;
        for (int k = c; k < c + 32 && k <= 0x7F; k++) line[n++] = (char)k;
        line[n] = 0;
        drawText(g_tft, FONT_TEXT, line, 6, y, COLOR_TEXT);
        drawText(g_tft, FONT_LABEL, line, 6, y + 60, COLOR_TEXT);
        y += 14;
    }
    drawText(g_tft, FONT_TEXT, "The quick brown fox jumps over the lazy dog.", 6, 130, COLOR_TEXT);
    drawText(g_tft, FONT_TEXT, "03:41 125 BPM", 6, 150, COLOR_ACCENT, 2);
    drawText(g_tft, FONT_TEXT, "03:41", 6, 175, COLOR_ACCENT, 4);
    drawText(g_tft, FONT_LABEL, "MIDITRACKER", 150, 175, COLOR_ACCENT, 2);
}


// ---- player screens from host renders ----------------------------------------

template <class T>
bool readFile(const std::string& path, std::vector<T>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { perror(path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n / sizeof(T));
    size_t got = fread(out.data(), sizeof(T), out.size(), f);
    fclose(f);
    return got == out.size();
}

const uint32_t RATE = 44100;
const uint32_t STEP_MS = 20; // the live update rate file_player_mode uses

// The tracker screen as it looks at the snapshot: drawn 600 ms before it,
// then the live updates run up to it, so the meters have their history.
bool trackerScreen(const std::string& dir, const char* base, const char* name, int volume) {
    std::vector<uint8_t> buf;
    if (!readFile(dir + "/" + base + ".snap", buf) || buf.size() < sizeof(HostSnap::Tracker)) return false;
    HostSnap::Tracker s;
    memcpy(&s, buf.data(), sizeof(s));
    if (!readFile(dir + "/" + base + ".raw", g_raw)) return false;
    UiTap::g_tracker = s.tap;
    const char* dot = strrchr(name, '.');
    char fmt[6] = {0};
    for (int i = 0; dot && dot[i + 1] && i < 5; i++) fmt[i] = (char)toupper((unsigned char)dot[i + 1]);
    UiPlayer::TrackerInfo t = {};
    t.name = name;
    t.format = fmt;
    t.channels = s.channels;
    t.instruments = s.instruments;
    t.songLength = s.songLength;
    t.order = s.order;
    t.row = s.row;
    t.speed = s.speed;
    t.tempo = s.tempo;
    t.state = UiPlayer::PS_PLAYING;
    t.volume = volume;
    g_played = s.written - RATE * 600 / 1000;
    t.elapsedMs = (uint32_t)((uint64_t)g_played * 1000 / RATE);
    g_tft.fillScreen(COLOR_BG);
    UiPlayer::drawTracker(t);
    for (uint32_t ms = 0; ms < 600; ms += STEP_MS) {
        g_hostMs += STEP_MS;
        g_played += RATE * STEP_MS / 1000;
        t.elapsedMs = (uint32_t)((uint64_t)g_played * 1000 / RATE);
        UiPlayer::trackerLive(t);
    }
    return true;
}

bool midiScreen(const std::string& dir, const char* base, const char* name) {
    std::vector<uint8_t> buf;
    if (!readFile(dir + "/" + base + ".snap", buf) || buf.size() < sizeof(HostSnap::Midi)) return false;
    static HostSnap::Midi s;
    memcpy(&s, buf.data(), sizeof(s));
    UiPlayer::MidiInfo m = {};
    m.name = name;
    m.format = s.format;
    m.tracks = s.tracks;
    m.tempoPct = 100;
    m.state = UiPlayer::PS_PLAYING;
    m.volume = 80;
    m.out = "HW+USB";
    m.audioOn = true;
    g_tft.fillScreen(COLOR_BG);
    for (int i = 0; i < HostSnap::Midi::FRAMES; i++) {
        g_midiFrame = &s.frames[i];
        UiTap::g_midi = s.frames[i].tap;
        m.elapsedMs = s.frames[i].elapsedMs;
        m.bpm = s.frames[i].bpm;
        g_hostMs += 40;
        if (i == 0) UiPlayer::drawMidi(m);
        else UiPlayer::midiLive(m);
    }
    return true;
}

// The WAV screen with a render as the file, played to 60% so the waveform
// has filled in that far.
// type and format as WavPlayer or Mp3Player give them.
bool wavScreen(const std::string& dir, const char* base, const char* name, const char* type = "WAV",
               const char* format = "16BIT") {
    if (!readFile(dir + "/" + base + ".raw", g_raw)) return false;
    UiPlayer::WavInfo w = {};
    w.name = name;
    w.fileKey = 1;
    w.totalMs = (uint32_t)((uint64_t)(g_raw.size() / 2) * 1000 / RATE);
    w.sampleRate = RATE;
    w.type = type;
    w.format = format;
    w.channels = 2;
    w.state = UiPlayer::PS_PLAYING;
    w.volume = 80;
    g_played = 0;
    g_tft.fillScreen(COLOR_BG);
    UiPlayer::drawWav(w);
    for (uint32_t ms = 0; ms < w.totalMs * 6 / 10; ms += STEP_MS) {
        g_hostMs += STEP_MS;
        w.elapsedMs = ms;
        g_played = (uint32_t)((uint64_t)ms * RATE / 1000);
        UiPlayer::wavLive(w);
    }
    return true;
}

// The paused state, and an error.
void stateScreens(const std::string& dir) {
    if (trackerScreen(dir, "s3m", "CMD - The Reflex.s3m", 80)) {
        UiPlayer::trackerState(UiPlayer::PS_PAUSED);
        save("tracker-paused");
    }
    UiPlayer::TrackerInfo t = {};
    t.name = "broken.xm";
    t.format = "XM";
    t.state = UiPlayer::PS_ERROR;
    t.error = "Not an XM file (bad header)";
    g_tft.fillScreen(COLOR_BG);
    UiPlayer::drawTracker(t);
    save("tracker-error");
}


// ---- screensaver views from the same snapshots -------------------------------

// Runs a view for ms, a frame at its rate, then saves it.
void saverRun(UiSaver::View v, UiSaver::Info& info, uint32_t ms, const char* name, bool ringMoves) {
    g_tft.fillScreen(rgb(0xFF00FF)); // anything start() doesn't clear shows up
    UiSaver::start(v, info);
    uint32_t step = UiSaver::frameMs(v);
    for (uint32_t t = 0; t < ms; t += step) {
        g_hostMs += step;
        if (ringMoves) g_played += RATE * step / 1000;
        info.elapsedMs += step;
        UiSaver::frame(info);
    }
    save(name);
}

bool loadTracker(const std::string& dir, const char* base, HostSnap::Tracker& s) {
    std::vector<uint8_t> buf;
    if (!readFile(dir + "/" + base + ".snap", buf) || buf.size() < sizeof(HostSnap::Tracker)) return false;
    memcpy(&s, buf.data(), sizeof(s));
    if (!readFile(dir + "/" + base + ".raw", g_raw)) return false;
    UiTap::g_tracker = s.tap;
    return true;
}

int saverScreens(const std::string& dir) {
    struct { const char* base; const char* name; } trackers[] = {
        {"mod", "AXELF.MOD"},
        {"xm", "Kenetic & Seenix - Our Little Fantasy.xm"},
        {"it", "Katie Cadet - Lucky Star.it"},
    };
    HostSnap::Tracker s;
    for (auto& tr : trackers) {
        if (!loadTracker(dir, tr.base, s)) return 1;
        UiSaver::Info info;
        info.src = UiSaver::SRC_TRACKER;
        info.name = tr.name;
        info.channels = s.channels;
        info.battery = 70;
        // Starts at the oldest row kept and plays up to the snapshot, so the
        // bar has stepped down and wrapped.
        const UiTap::Tracker& tk = UiTap::g_tracker;
        uint32_t oldest = tk.rowCount > (uint32_t)UiTap::ROW_RING ? tk.rowCount - UiTap::ROW_RING : 0;
        g_played = tk.rows[oldest % UiTap::ROW_RING].at;
        uint32_t ms = (uint32_t)((uint64_t)(s.written - g_played) * 1000 / RATE);
        info.elapsedMs = (uint32_t)((uint64_t)g_played * 1000 / RATE);
        saverRun(UiSaver::VIEW_RAIN, info, ms, (std::string("saver-rain-") + tr.base).c_str(), true);
    }

    // Now playing and Meters on the XM.
    if (!loadTracker(dir, "xm", s)) return 1;
    UiSaver::Info info;
    info.src = UiSaver::SRC_TRACKER;
    info.name = "Kenetic & Seenix - Our Little Fantasy.xm";
    info.channels = s.channels;
    info.progress = s.songLength ? s.order * 1000 / s.songLength : -1;
    info.battery = 70;
    g_played = s.written - RATE * 2;
    info.elapsedMs = (uint32_t)((uint64_t)g_played * 1000 / RATE);
    saverRun(UiSaver::VIEW_NOW_PLAYING, info, 1500, "saver-nowplaying-xm", true);
    g_played = s.written - RATE * 2;
    saverRun(UiSaver::VIEW_METERS, info, 1500, "saver-meters-xm", true);

    // Now playing on a WAV (Auto's choice for one), 60% in.
    if (!readFile(dir + "/s3m.raw", g_raw)) return 1;
    info = UiSaver::Info();
    info.src = UiSaver::SRC_WAV;
    info.name = "the reflex render.wav";
    uint32_t total = (uint32_t)((uint64_t)(g_raw.size() / 2) * 1000 / RATE);
    info.elapsedMs = total * 6 / 10;
    info.progress = 600;
    info.battery = 70;
    g_played = (uint32_t)((uint64_t)info.elapsedMs * RATE / 1000);
    saverRun(UiSaver::VIEW_NOW_PLAYING, info, 1000, "saver-nowplaying-wav", true);

    // .mid: rain and Meters from the note-ons, 8 frames 40 ms apart, each
    // one a rain line.
    std::vector<uint8_t> buf;
    if (!readFile(dir + "/mid.snap", buf) || buf.size() < sizeof(HostSnap::Midi)) return 1;
    static HostSnap::Midi m;
    memcpy(&m, buf.data(), sizeof(m));
    info = UiSaver::Info();
    info.src = UiSaver::SRC_MIDI_FILE;
    info.name = "busy_schedule.mid";
    info.battery = 70;
    UiSaver::View views[] = {UiSaver::VIEW_RAIN, UiSaver::VIEW_METERS};
    const char* names[] = {"saver-rain-mid", "saver-meters-mid"};
    for (int v = 0; v < 2; v++) {
        UiTap::g_midi = UiTap::Midi();
        g_tft.fillScreen(rgb(0xFF00FF));
        UiSaver::start(views[v], info);
        for (int i = 0; i < HostSnap::Midi::FRAMES; i++) {
            UiTap::g_midi = m.frames[i].tap;
            info.elapsedMs = m.frames[i].elapsedMs;
            g_hostMs += 250;
            UiSaver::frame(info);
        }
        save(names[v]);
    }

    // The idle card, and with the battery low.
    info = UiSaver::Info();
    info.battery = 70;
    saverRun(UiSaver::VIEW_IDLE, info, 20000, "saver-idle", false);
    info.battery = 12;
    info.low = true;
    saverRun(UiSaver::VIEW_IDLE, info, 4000, "saver-idle-low", false);
    return 0;
}

int playerScreens(const std::string& dir) {
    struct { const char* base; const char* name; } trackers[] = {
        {"mod", "AXELF.MOD"},
        {"s3m", "CMD - The Reflex.s3m"},
        {"xm", "Kenetic & Seenix - Our Little Fantasy.xm"},
        {"it", "Katie Cadet - Lucky Star.it"},
    };
    for (auto& tr : trackers) {
        if (!trackerScreen(dir, tr.base, tr.name, 80)) return 1;
        save((std::string("tracker-") + tr.base).c_str());
    }
    if (!midiScreen(dir, "mid", "busy_schedule.mid")) return 1;
    save("midi");
    if (!wavScreen(dir, "s3m", "the reflex render.wav")) return 1;
    save("wav");
    if (!wavScreen(dir, "s3m", "the reflex render.mp3", "MP3", "245K VBR")) return 1;
    save("mp3");
    UiPlayer::wavState(UiPlayer::PS_PAUSED);
    save("mp3-paused");
    UiPlayer::WavInfo e = {};
    e.name = "old radio show.mp3";
    e.type = "MP3";
    e.format = "0K CBR";
    e.state = UiPlayer::PS_ERROR;
    e.error = "layer I/II not supported";
    e.volume = 80;
    g_tft.fillScreen(COLOR_BG);
    UiPlayer::drawWav(e);
    save("mp3-error");
    stateScreens(dir);
    return saverScreens(dir);
}

// ---- boot animation -------------------------------------------------------------

// Boot animation frames at 0 ms, part way and at the end.
void bootScreens() {
    g_tft.fillScreen(rgb(0xFF00FF)); // anything the animation doesn't draw shows up
    uint32_t t0 = g_hostMs;
    UiSaver::bootStart("0.6.0", t0);
    save("boot-0");
    struct { uint32_t ms; const char* name; } shots[] = {{80, "boot-in"}, {240, "boot-rain"}, {420, "boot-mid"}, {700, "boot-late"}};
    for (auto& s : shots) {
        UiSaver::bootFrame(t0 + s.ms);
        save(s.name);
    }
    while (UiSaver::bootFrame(t0 + 2000)) {}
    save("boot-end");
    g_hostMs += 2000;
}

// Every frame at 20 ms steps, as boot-NNN.png in the out dir, 2x.
int bootFrames() {
    g_tft.fillScreen(rgb(0xFF00FF));
    uint32_t t0 = g_hostMs;
    UiSaver::bootStart("0.6.0", t0);
    int n = 0;
    char name[32];
    for (bool more = true; more; n++) {
        snprintf(name, sizeof(name), "/boot-%03d.png", n);
        writePng(g_outDir + name, 2);
        more = UiSaver::bootFrame(t0 + (n + 1) * 20);
    }
    snprintf(name, sizeof(name), "/boot-%03d.png", n);
    writePng(g_outDir + name, 2);
    printf("%d frames in %s\n", n + 1, g_outDir.c_str());
    return 0;
}

} // namespace

// look_render <out dir> [--players <snapshot dir> | --boot-frames]
// ---- recording, SysEx capture and send ------------------------------------------

void recChrome() {
    g_tft.fillScreen(COLOR_BG);
    statusLeds(true, false);
    statusBattery(70, false, false);
}

const char* const REC_LOG[] = {
    "Ch1  Note On   C4  v100", "Ch1  Note Off  C4  v64", "Ch10 Note On   D2  v127",
    "Ch2  CC 74  = 90",        "Ch1  Bend      +1024",   "Ch3  Prog Chg  12",
    "Ch10 Note Off  D2  v0",   "Ch1  Note On   G#5 v88",
};
const char* const SYX_LOG[] = {"SysEx  9 bytes", "SysEx  266 bytes", "SysEx  266 bytes", "SysEx  11 bytes"};

void recScreens() {
    using namespace UiRec;
    RecInfo r;
    r.kind = KIND_MIDI;
    r.name = "REC004.mid";
    r.state = REC_WAITING;
    recChrome();
    recDraw(r, nullptr, 0);
    save("rec-midi-waiting");

    r.name = "a much longer take name for the jam.mid";
    r.state = REC_RECORDING;
    r.elapsedMs = 754000;
    r.count = 1288;
    r.active = true;
    recChrome();
    recDraw(r, REC_LOG, 8);
    save("rec-midi-recording");
    r.elapsedMs = 755000;
    r.count = 1302;
    r.active = false;
    recLive(r);
    recLog(REC_LOG + 2, 6);
    save("rec-midi-live");

    discardCard(KIND_MIDI, r.name);
    save("rec-midi-discard");

    // Past an hour the time shrinks to fit beside the count, and a live
    // tick after invalidate() repaints every field.
    r.elapsedMs = 17UL * 3600000 + 2 * 60000 + 3000;
    r.count = 4123456;
    recChrome();
    recDraw(r, REC_LOG, 8);
    invalidate();
    r.elapsedMs += 1000;
    recLive(r);
    recLog(REC_LOG, 8);
    save("rec-midi-17h");
    r.elapsedMs = 100UL * 3600000;
    recLive(r);
    save("rec-midi-100h");

    r.state = REC_ERROR;
    r.error = "Old file locked, saved as REC002.mid";
    recChrome();
    recDraw(r, nullptr, 0);
    save("rec-midi-error");
    r.error = "Card full or write failed, the take up to 12:34 is kept";
    recChrome();
    recDraw(r, nullptr, 0);
    save("rec-midi-error-long");

    RecInfo c;
    c.kind = KIND_SYSEX;
    c.name = "DUMP001.syx";
    c.state = REC_WAITING;
    recChrome();
    recDraw(c, nullptr, 0);
    save("sysex-capture-waiting");
    c.state = REC_RECORDING;
    c.elapsedMs = 42000;
    c.count = 128;
    c.bytes = 34048;
    c.dropped = 2;
    c.active = true;
    recChrome();
    recDraw(c, SYX_LOG, 4);
    save("sysex-capture");
    c.bytes = 1250000;
    c.count = 4700;
    c.dropped = 3;
    c.active = false;
    recLive(c);
    save("sysex-capture-live");
    discardCard(KIND_SYSEX, c.name);
    save("sysex-capture-discard");
    c.state = REC_ERROR;
    c.error = "Sync failed, kept the messages before it";
    recChrome();
    recDraw(c, nullptr, 0);
    save("sysex-capture-error");

    SendInfo s;
    s.name = "Roland JV-1080 full bank backup, patches and performances.syx";
    s.state = SEND_SENDING;
    s.elapsedMs = 18000;
    s.bytesSent = 12345;
    s.totalBytes = 40000;
    s.messages = 46;
    s.active = true;
    recChrome();
    sendDraw(s);
    save("sysex-send");
    s.bytesSent = 20480;
    s.messages = 77;
    s.skipped = 2;
    s.active = false;
    s.elapsedMs = 29000;
    sendLive(s);
    save("sysex-send-live");
    s.state = SEND_DONE;
    s.bytesSent = 40000;
    s.messages = 150;
    s.junk = 5;
    recChrome();
    sendDraw(s);
    save("sysex-send-done");
    s.state = SEND_ERROR;
    s.error = "Can't open the file";
    recChrome();
    sendDraw(s);
    save("sysex-send-error");
}

// ---- convert --------------------------------------------------------------------

void convertScreens() {
    // The target list, as Ui::drawEntryMenu() draws it.
    g_tft.fillScreen(COLOR_BG);
    Status st;
    st.text = "Convert Night Drive (2019 remaster).mp3";
    drawStatus(st);
    const char* items[] = {"WAV 16-bit", "WAV 16-bit 44.1k mono"};
    for (int i = 0; i < ROWS; i++) drawTextRow(LIST_TOP + i * LIST_ROW_H, i < 2 ? items[i] : nullptr, nullptr, 0, i == 0);
    drawHintFooter("UP/DN move  ENTER select", nullptr);
    save("convert-targets");

    using namespace UiRec;
    ConvInfo c;
    c.name = "Night Drive (2019 remaster).mp3";
    c.target = "WAV 16-bit";
    c.outName = "Night Drive (2019 remaster).wav";
    c.state = CONV_RUNNING;
    c.elapsedMs = 400;
    c.permille = 3;
    recChrome();
    convDraw(c);
    save("convert-start");
    c.elapsedMs = 61000;
    c.leftMs = 83500;
    c.permille = 422;
    convLive(c);
    save("convert-running");
    c.state = CONV_DONE;
    c.elapsedMs = 144000;
    c.permille = 1000;
    recChrome();
    convDraw(c);
    save("convert-done");
    c.state = CONV_FAILED;
    c.message = "Card full or write failed";
    c.elapsedMs = 98000;
    c.permille = 610;
    c.target = "WAV 16-bit 44.1k mono";
    c.outName = "Night Drive (2019 remaster)-2.wav";
    recChrome();
    convDraw(c);
    save("convert-failed");
    c.state = CONV_CANCELLED;
    c.message = "Cancelled";
    recChrome();
    convDraw(c);
    save("convert-cancelled");
}

int main(int argc, char** argv) {
    g_outDir = argc > 1 ? argv[1] : ".";
    crcInit();
    setScreen(&g_tft);
    statusBattery(70, false, false);
    if (argc > 3 && strcmp(argv[2], "--players") == 0) return playerScreens(argv[3]);
    if (argc > 2 && strcmp(argv[2], "--boot-frames") == 0) return bootFrames();

    bootScreens();
    modeScreen(false, true);
    save("modes");
    browserScreen();
    save("browser");
    dialogScreen();
    save("dialog-delete");
    messageScreen();
    save("dialog-message");
    menuScreen();
    save("menu");
    settingsListScreen();
    save("settings-list");
    settingsScreen();
    save("settings");
    playerChrome();
    save("tracker-chrome");
    looperScreens();
    synthScreen(false);
    save("parisynth");
    synthScreen(true);
    save("parisynth-vol");
    synthPicker();
    save("synthedit-picker");
    synthEditScreen(false, 2);
    save("synthedit");
    synthEditScreen(false, 0);
    save("synthedit-wave");
    synthEditScreen(true, 3);
    save("synthedit-drum");
    synthBank(1);
    save("synthbank");
    synthName(nullptr);
    save("synthbank-name");
    synthName("save failed");
    save("synthbank-name-error");
    synthName(nullptr);
    synthOverwriteCard("PADS_2");
    save("synthbank-overwrite");
    synthLoad(11, 3);
    save("synthbank-load");
    synthLoad(0, 0);
    save("synthbank-load-empty");
    synthLoad(11, 3);
    synthResultCard("BANK LOADED", "my bank", true);
    save("synthbank-loaded");
    synthLoad(11, 3);
    synthResultCard("LOAD FAILED", "drums 808", false);
    save("synthbank-load-failed");
    synthBank(2);
    synthResultCard("RESET", "all sounds back to built-in", true);
    save("synthbank-reset");
    recScreens();
    convertScreens();
    fontSheet();
    save("fonts");
    return 0;
}

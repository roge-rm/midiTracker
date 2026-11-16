// Host render of the M12 look: draws the stage 2 screens with the real
// ui_look.cpp widgets into a framebuffer and writes PNGs, 1x and 2x.
//   make -C test/host look OUT_DIR=~/debug/midiTracker/m12-2
// The screens are put together here the way ui.cpp does it, with made-up
// data, so the layout can be checked without the device.
#include "ui_look.h"
#include "ui_player.h"
#include "ui_tap.h"
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
void sliceCharge(uint32_t) {}
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
        {"ping pong.mod", ROW_TRACKER, "MOD", 92 * 1024},
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
    statusBattery(90, true);
    Status st;
    st.title = "MIDITRACKER";
    st.titleColor = COLOR_ACCENT;
    st.note = "0.6.0";
    drawStatus(st);
    clearModeGrid();
    const char* labels[] = {"FILES", "LOOPER", "PARISYNTH", "SETTINGS"};
    const char* notes[] = {"play and record", "4 tracks", "16 channels", "midi \x7f synth \x7f theme"};
    for (int i = 0; i < 4; i++) drawModeTile(i, labels[i], notes[i], i == 0);
    static const Key keys[] = {{"ARROWS", "move", TONE_TEXT}, {"RIGHT", "open on right", TONE_TEXT},
                               {"ENT", "open", TONE_SELECT}};
    drawKeyFooter(keys, 3);
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
    const char* labels[] = {"MIDI Out", "MIDI Thru", "Clock", "Brightness", "Theme", "USB Drive", "Count In Bars", "Version"};
    const char* values[] = {"HW + USB", "Off", "Internal", "80%", "default", "", "1", "0.6.0"};
    int y0 = HEADER_H + 4;
    for (int i = 0; i < ROWS; i++)
        drawTextRow(y0 + i * LIST_ROW_H, i < 8 ? labels[i] : nullptr, i < 8 ? values[i] : nullptr, 180, i == 3);
    drawScrollbar(y0, ROWS, 8, 0);
    drawHintFooter("UD move  LR page", "EDIT+LR change");
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
    statusBattery(55, false);
    Status st;
    st.icon = &ICON_TRACKER;
    st.iconColor = COLOR_ACCENT;
    st.title = "TRACKER";
    drawStatus(st);
    drawHintFooter("EDIT+UD vol  PLAY tog", "NAV stop");
}

void looperChrome() {
    g_tft.fillScreen(COLOR_BG);
    Status st;
    st.icon = &ICON_LOOPER;
    st.iconColor = COLOR_ACCENT;
    st.title = "LOOPER";
    drawStatus(st);
    drawHintFooter("UD row/ALT out/in  PLAY pause  EDIT rec/bpm", "L back/R mute/EDIT bar  ENT menu  NAV stop");
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
bool wavScreen(const std::string& dir, const char* base, const char* name) {
    if (!readFile(dir + "/" + base + ".raw", g_raw)) return false;
    UiPlayer::WavInfo w = {};
    w.name = name;
    w.fileKey = 1;
    w.totalMs = (uint32_t)((uint64_t)(g_raw.size() / 2) * 1000 / RATE);
    w.sampleRate = RATE;
    w.bits = 16;
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
    stateScreens(dir);
    return 0;
}
} // namespace

// look_render <out dir> [--players <snapshot dir>]
int main(int argc, char** argv) {
    g_outDir = argc > 1 ? argv[1] : ".";
    crcInit();
    setScreen(&g_tft);
    statusBattery(70, false);
    if (argc > 3 && strcmp(argv[2], "--players") == 0) return playerScreens(argv[3]);

    drawSplash("0.6.0");
    save("splash");
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
    settingsScreen();
    save("settings");
    playerChrome();
    save("tracker-chrome");
    looperChrome();
    save("looper-chrome");
    fontSheet();
    save("fonts");
    return 0;
}

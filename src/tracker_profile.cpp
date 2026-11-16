// Profile build (env:pico_profile, -D TRACKER_PROFILE): at boot plays each
// file named in PROFILE_FILES ('|' between name parts, matched against
// file names on the card) for PROFILE_SECONDS and prints over serial how
// much of real time core 0 spends in the player's update(), the time in
// sample cache refills and SD reads (raw sector reads by time taken) and
// the ring underruns. Then repeats.
// Starts by listing up to 200 .mid names on the card. PROFILE_MIDI_FILES
// (same format, default "") names .mid files to play through MidiPlayer to
// the synth; those also print core 1 render cycles per sample, the synth
// governor counters and the peak voice count.
// The card searches and each .mid run under a 5 s watchdog, kicked while
// core 1 keeps looping, with a heartbeat line every second. After a watchdog reset the hang trace
// (see Synth::ProfileTrace) prints every 2 s and nothing else runs.
// PROFILE_WAV_FILES (same format, default "") names .wav files to play
// through WavPlayer; those print core 0 load, card reads and underruns.
// PROFILE_WAV_DIR (e.g. "/wav") instead profiles up to 16 .wav files found
// in that folder, each preceded by a line with its format.
// PROFILE_MP3_DIR (e.g. "/mp3") plays up to 16 .mp3 files in that folder
// through Mp3Player, each preceded by a format line, printing core 0 and
// core 1 load, decode time per frame on core 1, card reads and underruns.
// It needs MP3_PLAYER, which decodes on core 1 (see loop1()).
// With -D PROFILE_SAVER the trackers play with the screensaver's Pattern
// rain drawing, and each report adds the share of real time it took.
#ifdef TRACKER_PROFILE
#include <Arduino.h>
#include <new>
#include <strings.h>
#include "sd_card.h"
#include "synth.h"
#include "shared_ram_arena.h"
#include "mod_file.h"
#include "s3m_file.h"
#include "xm_file.h"
#include "it_file.h"
#include "midi_file.h"
#include "wav_file.h"
#include "mp3_file.h"
#include "midi_output.h"
#include "pins.h" // SAMPLE_RATE
#include <hardware/watchdog.h>
#include <hardware/clocks.h>
#ifdef PROFILE_SAVER
#include "ui_saver.h"
#include "ui_strip.h"
#endif
#include <hardware/regs/addressmap.h>

#ifndef PROFILE_FILES
#define PROFILE_FILES "Lucky Star|All The 7"
#endif
// How many .mid names to list at boot; 0 skips the list.
#ifndef PROFILE_LIST_MAX
#define PROFILE_LIST_MAX 0
#endif
#ifndef PROFILE_MIDI_FILES
#define PROFILE_MIDI_FILES ""
#endif
#ifndef PROFILE_WAV_FILES
#define PROFILE_WAV_FILES ""
#endif
#ifndef PROFILE_WAV_DIR
#define PROFILE_WAV_DIR ""
#endif
#ifndef PROFILE_MP3_DIR
#define PROFILE_MP3_DIR ""
#endif
#ifndef PROFILE_SECONDS
#define PROFILE_SECONDS 30
#endif

#ifdef PROFILE_SAVER
namespace {
// The player's update() as the draw pump, timed so it can be taken out of
// the saver's share.
TrackerPlayer* g_saverPlayer = nullptr;
uint32_t g_saverPumpUs = 0, g_saverUs = 0;
void saverPump() {
    uint32_t t = micros();
    g_saverPlayer->update();
    g_saverPumpUs += micros() - t;
}
} // namespace
#endif

namespace {

bool isTracker(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mod") || !strcasecmp(dot, ".s3m") || !strcasecmp(dot, ".xm") || !strcasecmp(dot, ".it"));
}

bool isMidi(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".mid");
}

bool isWav(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".wav");
}

bool isMp3(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".mp3");
}

enum FindKind { FIND_TRACKER, FIND_MIDI, FIND_WAV };

// Core 0's stack is the top of SCRATCH_Y (4 KB) and grows down into
// SCRATCH_X, whose top is core 1's stack (SCRATCH_X, 4 KB, nothing else in
// it). Core 0 paints its own below where it is at boot, core 1 paints all
// of SCRATCH_X below its stack in setup1(), and the heartbeat says how much
// of each was never touched.
const uint32_t STACK_PAINT = Synth::PROFILE_STACK_PAINT;

void paintStacks() {
    uint32_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    for (uint32_t a = SRAM5_BASE; a < sp - 64; a += 4) *(volatile uint32_t*)a = STACK_PAINT;
}

// Bytes never written from the bottom of a painted region up.
uint32_t untouched(uint32_t from, uint32_t to) {
    uint32_t a = from;
    while (a < to && *(volatile uint32_t*)a == STACK_PAINT) a += 4;
    return a - from;
}

void printStacks(const char* when) {
    uint32_t c0 = untouched(SRAM5_BASE, SRAM5_BASE + 0x1000);
    uint32_t c1 = untouched(SRAM4_BASE, SRAM4_BASE + 0x1000);
    Serial.printf("STACK %s: core0 never used %lu bytes of SCRATCH_Y%s, core1 never used %lu bytes of SCRATCH_X (4096)\n", when,
                  (unsigned long)c0, c0 == 0 ? " (reached SCRATCH_X, core 1's stack)" : "", (unsigned long)c1);
}

void watch();

// One name buffer for the searches below: each level only uses it before
// it recurses, and core 0's stack is 4 KB.
char g_name[200];

// Prints .mid paths, depth first, until max names have been printed.
void listMidi(FsFile& dir, char* path, size_t cap, int depth, int& count, int max) {
    FsFile e;
    size_t base = strlen(path);
    while (count < max && e.openNext(&dir, O_RDONLY)) {
        watch();
        char* name = g_name;
        e.getName(name, sizeof(g_name));
        bool isDir = e.isDir();
        if (name[0] != '.' && base + 1 + strlen(name) + 1 < cap) {
            path[base] = '/';
            strcpy(path + base + 1, name);
            if (isDir && (strcasecmp(name, "chords") == 0 || strcasecmp(name, "progressions") == 0 || strcasecmp(name, "testsongs") == 0)) { /* skip chord packs */ }
            else if (isDir && depth < 6) listMidi(e, path, cap, depth + 1, count, max);
            else if (!isDir && isMidi(name)) { Serial.printf("PROFILE midi %s\n", path); count++; }
            path[base] = 0;
        }
        e.close();
    }
}

// Depth-first search for a file of the given kind whose name contains want.
bool findFile(FsFile& dir, char* path, size_t cap, const char* want, int depth, FindKind kind = FIND_TRACKER) {
    FsFile e;
    size_t base = strlen(path);
    while (e.openNext(&dir, O_RDONLY)) {
        watch();
        char* name = g_name;
        e.getName(name, sizeof(g_name));
        bool isDir = e.isDir();
        if (name[0] != '.' && base + 1 + strlen(name) + 1 < cap) {
            path[base] = '/';
            strcpy(path + base + 1, name);
            if (isDir && depth < 6) {
                if (findFile(e, path, cap, want, depth + 1, kind)) { e.close(); return true; }
            } else if (!isDir && strstr(name, want) &&
                       (kind == FIND_MIDI ? isMidi(name) : kind == FIND_WAV ? isWav(name) : isTracker(name))) {
                e.close();
                return true;
            }
            path[base] = 0;
        }
        e.close();
    }
    return false;
}

TrackerPlayer* makePlayer(const char* path) {
    const char* dot = strrchr(path, '.');
    void* mem = SharedRamArena::data();
    if (!strcasecmp(dot, ".mod")) return new (mem) ModPlayer();
    if (!strcasecmp(dot, ".s3m")) return new (mem) S3mPlayer();
    if (!strcasecmp(dot, ".xm")) return new (mem) XmPlayer();
    return new (mem) ItPlayer();
}

void report(const char* tag, const char* name, const TrackerPlayer::Profile& p, uint32_t underruns) {
    // Load in tenths of a percent of the audio time produced.
    float audioUs = (float)p.frames * 1e6f / (float)SAMPLE_RATE;
    int load = audioUs > 0 ? (int)(1000.0f * (float)p.updateUs / audioUs) : 0;
    int refill = audioUs > 0 ? (int)(1000.0f * (float)p.refillUs / audioUs) : 0;
    Serial.printf("PROFILE %s %s: core0 load %d.%d%% (refills %d.%d%%, %lu), worst update %lu us, underrun samples %lu\n",
                  tag, name, load / 10, load % 10, refill / 10, refill % 10, (unsigned long)p.refills,
                  (unsigned long)p.worstUs, (unsigned long)underruns);
    Serial.printf("PROFILE %s %s: reads %lu (%lu KB), seek %lu ms, read %lu ms, frames per refill %lu\n", tag, name,
                  (unsigned long)p.reads, (unsigned long)(p.readBytes / 1024), (unsigned long)(p.seekUs / 1000), (unsigned long)(p.readUs / 1000),
                  (unsigned long)(p.refills ? p.refillFrames / p.refills : 0));
    Serial.printf("PROFILE %s %s: read times <200us %lu (%lu ms), <500 %lu (%lu), <1ms %lu (%lu), <2ms %lu (%lu), <5ms %lu (%lu), more %lu (%lu)\n", tag, name,
                  (unsigned long)p.hist[0], (unsigned long)p.histUs[0] / 1000, (unsigned long)p.hist[1], (unsigned long)p.histUs[1] / 1000,
                  (unsigned long)p.hist[2], (unsigned long)p.histUs[2] / 1000, (unsigned long)p.hist[3], (unsigned long)p.histUs[3] / 1000,
                  (unsigned long)p.hist[4], (unsigned long)p.histUs[4] / 1000, (unsigned long)p.hist[5], (unsigned long)p.histUs[5] / 1000);
    if (p.ticks) {
        // Voice counts as an average per tick, in tenths.
        auto avg = [&](uint32_t n) { return (unsigned long)((uint64_t)n * 10 / p.ticks); };
        Serial.printf("PROFILE %s %s: voices avg %lu.%lu peak %lu, background %lu.%lu, under -48 dB %lu.%lu (background %lu.%lu), at 0 %lu.%lu, "
                      "step > 1 %lu.%lu, > 2 %lu.%lu\n", tag, name,
                      avg(p.voiceTicks) / 10, avg(p.voiceTicks) % 10, (unsigned long)p.peakVoices, avg(p.bgTicks) / 10, avg(p.bgTicks) % 10,
                      avg(p.q48Ticks) / 10, avg(p.q48Ticks) % 10, avg(p.q48BgTicks) / 10, avg(p.q48BgTicks) % 10,
                      avg(p.zeroTicks) / 10, avg(p.zeroTicks) % 10, avg(p.up1Ticks) / 10, avg(p.up1Ticks) % 10,
                      avg(p.up2Ticks) / 10, avg(p.up2Ticks) % 10);
        Serial.printf("PROFILE %s %s: reads by voice: background %lu, under -48 dB %lu, step > 1 %lu, > 2 %lu (of %lu)\n", tag, name,
                      (unsigned long)p.readsBg, (unsigned long)p.readsQ48, (unsigned long)p.readsUp1, (unsigned long)p.readsUp2,
                      (unsigned long)p.reads);
    }
}

void addProfile(TrackerPlayer::Profile& t, const TrackerPlayer::Profile& c) {
    t.updateUs += c.updateUs; t.frames += c.frames; t.refillUs += c.refillUs; t.refills += c.refills;
    t.refillFrames += c.refillFrames; t.seekUs += c.seekUs; t.readUs += c.readUs; t.reads += c.reads;
    for (int h = 0; h < 6; h++) { t.hist[h] += c.hist[h]; t.histUs[h] += c.histUs[h]; }
    if (c.worstUs > t.worstUs) t.worstUs = c.worstUs;
    t.ticks += c.ticks; t.voiceTicks += c.voiceTicks; t.bgTicks += c.bgTicks; t.q48Ticks += c.q48Ticks;
    t.q48BgTicks += c.q48BgTicks; t.zeroTicks += c.zeroTicks; t.up1Ticks += c.up1Ticks; t.up2Ticks += c.up2Ticks;
    t.readBytes += c.readBytes;
    t.readsBg += c.readsBg; t.readsQ48 += c.readsQ48; t.readsUp1 += c.readsUp1; t.readsUp2 += c.readsUp2;
    if (c.peakVoices > t.peakVoices) t.peakVoices = c.peakVoices;
}

const uint32_t WATCHDOG_MS = 5000;
uint32_t g_kickCount1 = 0, g_beatMs = 0;

// Kicks the watchdog while core 1 is still looping, and prints the heartbeat.
void watch() {
    Synth::ProfileTrace& t = Synth::profileTrace();
    t.ms0 = millis();
    uint32_t c = t.count1;
    if (c != g_kickCount1) {
        g_kickCount1 = c;
        watchdog_update();
    }
    if (t.ms0 - g_beatMs >= 1000) {
        g_beatMs = t.ms0;
        Synth::profileHeartbeat();
        printStacks("beat");
    }
}

void armWatchdog() {
    Synth::ProfileTrace& t = Synth::profileTrace();
    t.magic = Synth::PROFILE_TRACE_MAGIC;
    t.armed = 1;
    t.faultMagic = 0;
    g_kickCount1 = t.count1;
    watchdog_enable(WATCHDOG_MS, true);
}

void disarmWatchdog() {
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    Synth::profileTrace().armed = 0;
}

void profileMidi(const char* want) {
    Synth::ProfileTrace& tr = Synth::profileTrace();
    tr.where0 = Synth::PW0_FIND;
    char path[256] = "";
    FsFile root;
    root.open("/");
    bool found = findFile(root, path, sizeof(path), want, 0, FIND_MIDI);
    root.close();
    if (!found) { Serial.printf("PROFILE notfound %s\n", want); return; }
    printStacks("after findFile");
    Serial.printf("PROFILE loading %s\n", path);
    Serial.flush();
    tr.where0 = Synth::PW0_LOAD;
    MidiPlayer* p = new (SharedRamArena::data()) MidiPlayer();
    if (!p->load(path)) {
        Serial.printf("PROFILE loadfail %s: %s\n", path, p->errorMessage());
        p->~MidiPlayer();
        return;
    }
    watch();
    Serial.printf("PROFILE playing %s\n", path);
    Serial.flush();
    tr.where0 = Synth::PW0_PLAY;
    p->setTempoScale(1.0f);
    MidiOutput::setAudioOutput(true);
    Synth::resetPrograms();
    p->play();
    uint32_t start = millis();
    while (millis() - start < 1000 && p->state() == MidiPlayer::STATE_PLAYING) {
        tr.where0 = Synth::PW0_UPDATE;
        p->update();
        watch();
    }
    uint32_t u0 = Synth::wavStreamUnderrunSamples();
    Synth::profileReset();
    // The loop below does nothing but call update(), so core0 load counts
    // every empty poll. Calls under 20 us are counted apart as polls, the
    // rest (events sent, SD refills, output waits) as work.
    uint32_t busyUs = 0, worstUs = 0, calls = 0, workCalls = 0, workUs = 0;
    uint32_t t0 = micros(), win = millis();
    const uint32_t cpuMhz = F_CPU / 1000000;
    auto print = [&](const char* tag, uint32_t busy, uint32_t elapsedUs) {
        tr.where0 = Synth::PW0_PRINT;
        Synth::SynthProfile sp;
        Synth::profileGet(sp);
        int load = elapsedUs ? (int)(1000.0f * (float)busy / (float)elapsedUs) : 0;
        int work = elapsedUs ? (int)(1000.0f * (float)workUs / (float)elapsedUs) : 0;
        uint32_t polls = calls - workCalls;
        uint32_t pollNs = polls ? (uint32_t)((uint64_t)(busy - workUs) * 1000u / polls) : 0;
        Serial.printf("PROFILE %s %s: core0 load %d.%d%% (work %d.%d%% in %lu calls, %lu polls at %lu ns), worst update %lu us, underrun samples %lu\n", tag, want,
                      load / 10, load % 10, work / 10, work % 10, (unsigned long)workCalls, (unsigned long)polls,
                      (unsigned long)pollNs, (unsigned long)worstUs, (unsigned long)(Synth::wavStreamUnderrunSamples() - u0));
        // Budget per sample is cpu clock / SAMPLE_RATE cycles. Governor block = 32 samples,
        // limit is 90% of the block budget; sheds = voices faded out since the window start.
        Serial.printf("PROFILE %s %s: core1 cycles/sample peak %lu avg %lu (budget %lu), peak 32-sample block %lu (limit %lu), "
                      "governor sheds %lu (boot %lu), governor peak block since boot %lu cycles, peak voices %lu\n",
                      tag, want, (unsigned long)sp.peakSampleCycles, (unsigned long)sp.avgSampleCycles,
                      (unsigned long)(cpuMhz * 1000000u / SAMPLE_RATE), (unsigned long)sp.peakBlockCycles,
                      (unsigned long)(cpuMhz * 1000000u / SAMPLE_RATE * 32 / 10 * 9), (unsigned long)sp.sheds,
                      (unsigned long)sp.shedsTotal, (unsigned long)sp.peakUsedTotal, (unsigned long)sp.peakVoices);
    };
    while (millis() - start < 1000u + PROFILE_SECONDS * 1000u && p->state() == MidiPlayer::STATE_PLAYING) {
        uint32_t a = micros();
        tr.where0 = Synth::PW0_UPDATE;
        p->update();
        uint32_t d = micros() - a;
        watch();
        busyUs += d;
        calls++;
        if (d >= 20) { workCalls++; workUs += d; }
        if (d > worstUs) worstUs = d;
        if (millis() - win >= 5000) {
            print("5s", busyUs, micros() - t0);
            win = millis();
        }
    }
    print("RESULT", busyUs, micros() - t0);
    tr.where0 = Synth::PW0_STOP;
    p->stop();
    p->~MidiPlayer();
    Synth::allNotesOff();
    watch();
    tr.where0 = Synth::PW0_IDLE;
}

// Plays a .wav like the file player does (update() every loop) and
// prints core 0 load, card reads and underruns per 5 s and for the run.
void profileWavPath(const char* path, const char* want, bool showFormat);

void profileWav(const char* want) {
    char path[256] = "";
    FsFile root;
    root.open("/");
    bool found = findFile(root, path, sizeof(path), want, 0, FIND_WAV);
    root.close();
    if (!found) { Serial.printf("PROFILE notfound %s\n", want); return; }
    profileWavPath(path, want, false);
}

// The format tag from the file's fmt chunk, 0 if it can't be read.
uint16_t wavFormatTag(const char* path) {
    FsFile f;
    if (!f.open(path, O_RDONLY)) return 0;
    uint8_t h[12];
    uint16_t tag = 0;
    if (f.read(h, 12) == 12) {
        for (int n = 0; n < 64; n++) {
            uint8_t c[8];
            if (f.read(c, 8) != 8) break;
            uint32_t len = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
            if (!memcmp(c, "fmt ", 4)) {
                uint8_t t[2];
                if (f.read(t, 2) == 2) tag = t[0] | (t[1] << 8);
                break;
            }
            if (!f.seekSet(f.curPosition() + len + (len & 1))) break;
        }
    }
    f.close();
    return tag;
}

void profileWavPath(const char* path, const char* want, bool showFormat) {
    if (showFormat) {
        uint16_t tag = wavFormatTag(path);
        const char* tn = tag == 1 ? "PCM" : tag == 0x11 ? "ADPCM" : tag == 3 ? "float" : tag == 0xFFFE ? "EXTENSIBLE" : "other";
        static WavPlayer probe;
        if (probe.load(path)) {
            Serial.printf("PROFILE file %s: %s (tag 0x%04X), %lu Hz, %d ch, %d bit, %lu ms\n", path, tn, tag,
                          (unsigned long)probe.sampleRate(), probe.channels(), probe.bitsPerSample(), (unsigned long)probe.totalMs());
        } else {
            Serial.printf("PROFILE file %s: %s (tag 0x%04X), load error: %s\n", path, tn, tag, probe.errorMessage());
            return;
        }
        probe.stop();
    }
    static WavPlayer p;
    if (!p.load(path)) {
        Serial.printf("PROFILE loadfail %s: %s\n", path, p.errorMessage());
        return;
    }
    Serial.printf("PROFILE playing %s (%lu Hz, %d bit, %d ch)\n", path, (unsigned long)p.sampleRate(), p.bitsPerSample(), p.channels());
    p.play();
    uint32_t start = millis();
    while (millis() - start < 1000 && p.state() == WavPlayer::STATE_PLAYING) p.update();
    uint32_t u0 = p.underrunSamples(), e0 = p.elapsedMs();
    uint32_t busyUs = 0, worstUs = 0, winWorst = 0, winBusy = 0, winU0 = u0;
    uint32_t t0 = micros(), win = millis(), winT0 = t0;
    WavPlayer::Profile total = {};
    WavPlayer::profile = {};
    auto print = [&](const char* tag, uint32_t busy, uint32_t us, uint32_t worst, const WavPlayer::Profile& r, uint32_t underruns) {
        int load = us ? (int)(1000.0f * (float)busy / (float)us) : 0;
        Serial.printf("PROFILE %s %s: core0 load %d.%d%%, worst update %lu us, reads %lu (%lu KB, %lu ms), underrun samples %lu\n",
                      tag, want, load / 10, load % 10, (unsigned long)worst, (unsigned long)r.reads,
                      (unsigned long)(r.readBytes / 1024), (unsigned long)(r.readUs / 1000), (unsigned long)underruns);
    };
    while (millis() - start < 1000u + PROFILE_SECONDS * 1000u && p.state() == WavPlayer::STATE_PLAYING) {
        uint32_t a = micros();
        p.update();
        uint32_t d = micros() - a;
        busyUs += d;
        winBusy += d;
        if (d > worstUs) worstUs = d;
        if (d > winWorst) winWorst = d;
        if (millis() - win >= 5000) {
            WavPlayer::Profile& c = WavPlayer::profile;
            uint32_t now = micros();
            print("5s", winBusy, now - winT0, winWorst, c, p.underrunSamples() - winU0);
            total.reads += c.reads; total.readBytes += c.readBytes; total.readUs += c.readUs;
            c = {};
            winBusy = 0;
            winWorst = 0;
            winT0 = now;
            winU0 = p.underrunSamples();
            win = millis();
        }
    }
    WavPlayer::Profile& c = WavPlayer::profile;
    total.reads += c.reads; total.readBytes += c.readBytes; total.readUs += c.readUs;
    uint32_t us = micros() - t0;
    print("RESULT", busyUs, us, worstUs, total, p.underrunSamples() - u0);
    // The elapsed time should move with real time when nothing underran.
    Serial.printf("PROFILE RESULT %s: elapsed moved %lu ms in %lu ms\n", want, (unsigned long)(p.elapsedMs() - e0), (unsigned long)(us / 1000));
    p.stop();
}

// Profiles every .wav directly in PROFILE_WAV_DIR, up to 16.
void profileWavDir() {
    static char names[16][48];
    int count = 0;
    FsFile dir;
    if (!dir.open(PROFILE_WAV_DIR)) { Serial.printf("PROFILE notfound %s\n", PROFILE_WAV_DIR); return; }
    FsFile e;
    while (count < 16 && e.openNext(&dir, O_RDONLY)) {
        char* name = g_name;
        e.getName(name, sizeof(g_name));
        if (!e.isDir() && name[0] != '.' && isWav(name) && strlen(name) < sizeof(names[0])) strcpy(names[count++], name);
        e.close();
    }
    dir.close();
    Serial.printf("PROFILE wav dir %s: %d files\n", PROFILE_WAV_DIR, count);
    for (int i = 0; i < count; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", PROFILE_WAV_DIR, names[i]);
        profileWavPath(path, names[i], true);
    }
}

// Plays an .mp3 like a file player would (update() every loop) and prints
// per 5 s and for the run: core 0 load (time in update()), core 1 load
// (time outside its I2S write wait, synth render included), decode time per
// frame on core 1, input ring starved count, card reads and underruns.
void profileMp3Path(const char* path, const char* want) {
    static Mp3Player p;
    if (!p.load(path)) {
        Serial.printf("PROFILE loadfail %s: %s\n", path, p.errorMessage());
        return;
    }
    uint32_t frameUs = (uint32_t)((uint64_t)p.samplesPerFrame() * 1000000u / p.sampleRate());
    Serial.printf("PROFILE file %s: MPEG%s layer III, %lu Hz, %d ch, %lu kbps, %s, ID3v2 %lu bytes, %lu ms, frame %lu us; "
                  "decoder %u bytes, arena %u of %u bytes\n",
                  path, p.mpegVersion() == 25 ? "2.5" : p.mpegVersion() == 2 ? "2" : "1", (unsigned long)p.sampleRate(),
                  p.channels(), (unsigned long)(p.bitrate() / 1000),
                  !strcmp(p.vbrTag(), "Xing") || !strcmp(p.vbrTag(), "VBRI") ? "VBR" : p.vbrTag()[0] ? "CBR (Info)" : "no VBR header",
                  (unsigned long)p.id3Bytes(), (unsigned long)p.totalMs(), (unsigned long)frameUs,
                  (unsigned)Mp3Player::decoderBytes(), (unsigned)Mp3Player::arenaBytes(), (unsigned)SharedRamArena::SIZE);
    p.play();
    uint32_t start = millis();
    while (millis() - start < 1000 && p.state() == Mp3Player::STATE_PLAYING) p.update();
    uint32_t u0 = p.underrunSamples(), e0 = p.elapsedMs();
    uint32_t busyUs = 0, worstUs = 0, winWorst = 0, winBusy = 0, winU0 = u0;
    uint32_t cyclesPerUs = clock_get_hz(clk_sys) / 1000000u;
    uint64_t idleCycles = 0;
    uint32_t idle0 = Synth::profileCore1IdleCycles();
    uint32_t t0 = micros(), win = millis(), winT0 = t0;
    Mp3Player::Profile total = {};
    Mp3Player::profileTake();
    Synth::profileReset();
    // core1 load splits into decode (the frames that decoded) and output,
    // which is the rest: render, I2S writes, resampling into the WAV ring
    // and rejected decoder calls. render is renderSample()'s average and
    // peak since the file's first second.
    auto print = [&](const char* tag, uint32_t busy, uint32_t us, uint64_t idle, uint32_t worst, const Mp3Player::Profile& r,
                     uint32_t underruns) {
        int load = us ? (int)(1000.0f * (float)busy / (float)us) : 0;
        float idleUs = (float)idle / (float)cyclesPerUs;
        int load1 = us ? (int)(1000.0f * (1.0f - idleUs / (float)us)) : 0;
        if (load1 < 0) load1 = 0;
        int dec1 = us ? (int)(1000.0f * (float)r.decodeUs / (float)us) : 0;
        int out1 = load1 > dec1 ? load1 - dec1 : 0;
        Synth::SynthProfile sp;
        Synth::profileGet(sp);
        uint32_t avg = r.frames ? r.decodeUs / r.frames : 0;
        int avgPct = frameUs ? (int)(1000u * avg / frameUs) : 0;
        int worstPct = frameUs ? (int)(1000u * r.decodeWorstUs / frameUs) : 0;
        Serial.printf("PROFILE %s %s: core0 load %d.%d%%, worst update %lu us, core1 load %d.%d%% "
                      "(decode %d.%d%%, output %d.%d%%, render avg %lu peak %lu cycles/sample), "
                      "core1 decode %lu frames avg %lu us (%d.%d%%) worst %lu us (%d.%d%%), rejects %lu, input starved %lu, "
                      "reads %lu (%lu KB, %lu ms), underrun samples %lu\n",
                      tag, want, load / 10, load % 10, (unsigned long)worst, load1 / 10, load1 % 10, dec1 / 10, dec1 % 10,
                      out1 / 10, out1 % 10, (unsigned long)sp.avgSampleCycles, (unsigned long)sp.peakSampleCycles,
                      (unsigned long)r.frames, (unsigned long)avg, avgPct / 10, avgPct % 10, (unsigned long)r.decodeWorstUs,
                      worstPct / 10, worstPct % 10, (unsigned long)r.rejects, (unsigned long)r.starved, (unsigned long)r.reads,
                      (unsigned long)(r.readBytes / 1024), (unsigned long)(r.readUs / 1000), (unsigned long)underruns);
    };
    auto add = [](Mp3Player::Profile& t, const Mp3Player::Profile& c) {
        t.reads += c.reads; t.readBytes += c.readBytes; t.readUs += c.readUs;
        t.frames += c.frames; t.decodeUs += c.decodeUs; t.rejects += c.rejects; t.starved += c.starved;
        if (c.decodeWorstUs > t.decodeWorstUs) t.decodeWorstUs = c.decodeWorstUs;
    };
    while (millis() - start < 1000u + PROFILE_SECONDS * 1000u && p.state() == Mp3Player::STATE_PLAYING) {
        uint32_t a = micros();
        p.update();
        uint32_t d = micros() - a;
        busyUs += d;
        winBusy += d;
        if (d > worstUs) worstUs = d;
        if (d > winWorst) winWorst = d;
        if (millis() - win >= 5000) {
            Mp3Player::Profile c = Mp3Player::profileTake();
            uint32_t idle = Synth::profileCore1IdleCycles();
            uint32_t now = micros();
            print("5s", winBusy, now - winT0, idle - idle0, winWorst, c, p.underrunSamples() - winU0);
            idleCycles += idle - idle0;
            idle0 = idle;
            add(total, c);
            winBusy = 0;
            winWorst = 0;
            winT0 = now;
            winU0 = p.underrunSamples();
            win = millis();
        }
    }
    uint32_t idle = Synth::profileCore1IdleCycles();
    idleCycles += idle - idle0;
    add(total, Mp3Player::profileTake());
    uint32_t us = micros() - t0;
    print("RESULT", busyUs, us, idleCycles, worstUs, total, p.underrunSamples() - u0);
    Serial.printf("PROFILE RESULT %s: elapsed moved %lu ms in %lu ms\n", want, (unsigned long)(p.elapsedMs() - e0), (unsigned long)(us / 1000));
    p.stop();
    printStacks("after the mp3");
}

// Profiles every .mp3 directly in PROFILE_MP3_DIR, up to 16.
void profileMp3Dir() {
    static char names[16][96];
    int count = 0;
    FsFile dir;
    if (!dir.open(PROFILE_MP3_DIR)) { Serial.printf("PROFILE notfound %s\n", PROFILE_MP3_DIR); return; }
    FsFile e;
    while (count < 16 && e.openNext(&dir, O_RDONLY)) {
        char* name = g_name;
        e.getName(name, sizeof(g_name));
        if (!e.isDir() && name[0] != '.' && isMp3(name) && strlen(name) < sizeof(names[0])) strcpy(names[count++], name);
        e.close();
    }
    dir.close();
    Serial.printf("PROFILE mp3 dir %s: %d files\n", PROFILE_MP3_DIR, count);
    for (int i = 0; i < count; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", PROFILE_MP3_DIR, names[i]);
        profileMp3Path(path, names[i]);
    }
}

} // namespace

void trackerProfileRun() {
    static const char list[] = PROFILE_FILES;
    static const char midiList[] = PROFILE_MIDI_FILES;
    static const char wavList[] = PROFILE_WAV_FILES;
    // A trace left by a watchdog reset during a .mid: print it every 2 s
    // and run nothing else.
    Synth::ProfileTrace& tr = Synth::profileTrace();
    if (watchdog_enable_caused_reboot() && tr.magic == Synth::PROFILE_TRACE_MAGIC && tr.armed) {
        static Synth::ProfileTrace saved;
        saved = tr;
        tr.magic = 0;
        for (uint32_t n = 0;; n++) {
            Serial.printf("TRACE watchdog reset during the .mid search or playback (report %lu)\n", (unsigned long)n);
            Synth::profilePrintTrace(saved, "last run before the reset");
            delay(2000);
        }
    }
    tr = Synth::ProfileTrace{};
    tr.where0 = Synth::PW0_IDLE;
    paintStacks();
    // Wait up to 15 s for a serial reader so the file list isn't missed.
    for (uint32_t t0 = millis(); !Serial && millis() - t0 < 15000;) delay(10);
    armWatchdog();
    {
        char path[256] = "";
        FsFile root;
        root.open("/");
        int count = 0;
        Serial.println("PROFILE midi list start");
        listMidi(root, path, sizeof(path), 0, count, PROFILE_LIST_MAX);
        root.close();
        Serial.printf("PROFILE midi list end, %d names%s\n", count, count >= PROFILE_LIST_MAX ? " (stopped at the limit)" : "");
        printStacks("after the list");
    }
    for (;;) {
        armWatchdog();
        for (const char* w = midiList; *w;) {
            const char* bar = strchr(w, '|');
            size_t n = bar ? (size_t)(bar - w) : strlen(w);
            char want[64];
            if (n >= sizeof(want)) n = sizeof(want) - 1;
            memcpy(want, w, n);
            want[n] = 0;
            w += n + (bar ? 1 : 0);
            profileMidi(want);
        }
        disarmWatchdog();
        for (const char* w = wavList; *w;) {
            const char* bar = strchr(w, '|');
            size_t n = bar ? (size_t)(bar - w) : strlen(w);
            char want[64];
            if (n >= sizeof(want)) n = sizeof(want) - 1;
            memcpy(want, w, n);
            want[n] = 0;
            w += n + (bar ? 1 : 0);
            profileWav(want);
        }
        if (PROFILE_WAV_DIR[0]) profileWavDir();
        if (PROFILE_MP3_DIR[0]) profileMp3Dir();
        const char* w = list;
        while (*w) {
            const char* bar = strchr(w, '|');
            size_t n = bar ? (size_t)(bar - w) : strlen(w);
            char want[64];
            if (n >= sizeof(want)) n = sizeof(want) - 1;
            memcpy(want, w, n);
            want[n] = 0;
            w += n + (bar ? 1 : 0);

            char path[256] = "";
            FsFile root;
            root.open("/");
            bool found = findFile(root, path, sizeof(path), want, 0);
            root.close();
            if (!found) { Serial.printf("PROFILE notfound %s\n", want); continue; }
            TrackerPlayer* p = makePlayer(path);
            uint32_t loadMs = millis();
            if (!p->load(path)) {
                Serial.printf("PROFILE loadfail %s: %s\n", path, p->errorMessage());
                p->~TrackerPlayer();
                continue;
            }
            loadMs = millis() - loadMs;
            Serial.printf("PROFILE load %s: %lu ms\n", want, (unsigned long)loadMs);
            {
                FsFile f;
                uint32_t b = 0, e = 0;
                bool cont = f.open(path, O_RDONLY) && f.contiguousRange(&b, &e);
                Serial.printf("PROFILE playing %s (fat %d, cluster %lu, contiguous %d)\n", path, sd.fatType(),
                              (unsigned long)sd.bytesPerCluster(), cont ? 1 : 0);
                f.close();
            }
            p->play();
            // The first second (ring pre-fill, first reads) is left out.
            uint32_t start = millis();
            while (millis() - start < 1000 && p->state() == TrackerPlayer::STATE_PLAYING) p->update();
#ifdef PROFILE_SAVER
            UiSaver::Info si;
            si.src = UiSaver::SRC_TRACKER;
            si.channels = p->channelCount();
            g_saverPlayer = p;
            UiSaver::start(UiSaver::VIEW_RAIN, si);
            uint32_t saverAt = millis();
            bool saverPending = true;
            g_saverUs = 0;
#endif
            TrackerPlayer::profile = {};
            uint32_t u0 = Synth::wavStreamUnderrunSamples();
            TrackerPlayer::Profile total = {};
            uint32_t win = millis();
            while (millis() - start < 1000u + PROFILE_SECONDS * 1000u && p->state() == TrackerPlayer::STATE_PLAYING) {
                p->update();
#ifdef PROFILE_SAVER
                Ui::sliceBeginLoop();
                uint32_t now = millis();
                bool due = now - saverAt >= UiSaver::frameMs(UiSaver::VIEW_RAIN);
                if (due || (saverPending && !Ui::ringLow())) {
                    if (due) saverAt = now;
                    uint32_t t0 = micros(), p0 = g_saverPumpUs;
                    Ui::setDrawPump(saverPump);
                    saverPending = UiSaver::frame(si);
                    Ui::setDrawPump(nullptr);
                    g_saverUs += micros() - t0 - (g_saverPumpUs - p0);
                }
#endif
                if (millis() - win >= 5000) {
                    TrackerPlayer::Profile& c = TrackerPlayer::profile;
                    report("5s", want, c, Synth::wavStreamUnderrunSamples() - u0);
#ifdef PROFILE_SAVER
                    Serial.printf("PROFILE saver %lu us in %lu ms\n", (unsigned long)g_saverUs, (unsigned long)(millis() - win));
                    g_saverUs = 0;
#endif
                    addProfile(total, c);
                    c = {};
                    win = millis();
                }
            }
            TrackerPlayer::Profile& c = TrackerPlayer::profile;
            addProfile(total, c);
            report("RESULT", want, total, Synth::wavStreamUnderrunSamples() - u0);
            if (!strcasecmp(strrchr(path, '.'), ".it")) {
                Serial.printf("PROFILE RESULT %s: reads by sample since load:", want);
                for (int i = 0; i < ItPlayer::MAX_SAMPLES; i++)
                    if (ItPlayer::profileSampleReads[i]) Serial.printf(" %d:%lu", i + 1, (unsigned long)ItPlayer::profileSampleReads[i]);
                Serial.println();
            }
            p->stop();
            p->~TrackerPlayer();
        }
        Serial.println("PROFILE done");
        delay(5000);
    }
}
#endif

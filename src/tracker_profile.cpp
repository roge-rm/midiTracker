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
#include "midi_output.h"
#include "pins.h" // SAMPLE_RATE
#include <hardware/watchdog.h>
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
#ifndef PROFILE_SECONDS
#define PROFILE_SECONDS 30
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

// Core 0's stack is the top of SCRATCH_Y (4 KB) and grows down into
// SCRATCH_X, whose top is core 1's stack. Both get painted at boot and the
// heartbeat says how much of each was never touched.
const uint32_t STACK_PAINT = 0x5AC5AC5A;
const uint32_t CORE1_PAINT_END = SRAM4_BASE + 0x800; // the top 2 KB is left to core 1

void paintStacks() {
    uint32_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    for (uint32_t a = SRAM5_BASE; a < sp - 64; a += 4) *(volatile uint32_t*)a = STACK_PAINT;
    for (uint32_t a = SRAM4_BASE; a < CORE1_PAINT_END; a += 4) *(volatile uint32_t*)a = STACK_PAINT;
}

// Bytes never written from the bottom of a painted region up.
uint32_t untouched(uint32_t from, uint32_t to) {
    uint32_t a = from;
    while (a < to && *(volatile uint32_t*)a == STACK_PAINT) a += 4;
    return a - from;
}

void printStacks(const char* when) {
    uint32_t c0 = untouched(SRAM5_BASE, SRAM5_BASE + 0x1000);
    uint32_t c1 = untouched(SRAM4_BASE, CORE1_PAINT_END);
    Serial.printf("STACK %s: core0 never used %lu bytes of SCRATCH_Y%s, SCRATCH_X never used %lu of its lower 2048\n", when,
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

// Depth-first search for a file whose name contains want, a .mid one if
// midi is set, else a tracker file.
bool findFile(FsFile& dir, char* path, size_t cap, const char* want, int depth, bool midi = false) {
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
                if (findFile(e, path, cap, want, depth + 1, midi)) { e.close(); return true; }
            } else if (!isDir && strstr(name, want) && (midi ? isMidi(name) : isTracker(name))) {
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
    Serial.printf("PROFILE %s %s: reads %lu, seek %lu ms, read %lu ms, frames per refill %lu\n", tag, name,
                  (unsigned long)p.reads, (unsigned long)(p.seekUs / 1000), (unsigned long)(p.readUs / 1000),
                  (unsigned long)(p.refills ? p.refillFrames / p.refills : 0));
    Serial.printf("PROFILE %s %s: read times <200us %lu (%lu ms), <500 %lu (%lu), <1ms %lu (%lu), <2ms %lu (%lu), <5ms %lu (%lu), more %lu (%lu)\n", tag, name,
                  (unsigned long)p.hist[0], (unsigned long)p.histUs[0] / 1000, (unsigned long)p.hist[1], (unsigned long)p.histUs[1] / 1000,
                  (unsigned long)p.hist[2], (unsigned long)p.histUs[2] / 1000, (unsigned long)p.hist[3], (unsigned long)p.histUs[3] / 1000,
                  (unsigned long)p.hist[4], (unsigned long)p.histUs[4] / 1000, (unsigned long)p.hist[5], (unsigned long)p.histUs[5] / 1000);
}

void addProfile(TrackerPlayer::Profile& t, const TrackerPlayer::Profile& c) {
    t.updateUs += c.updateUs; t.frames += c.frames; t.refillUs += c.refillUs; t.refills += c.refills;
    t.refillFrames += c.refillFrames; t.seekUs += c.seekUs; t.readUs += c.readUs; t.reads += c.reads;
    for (int h = 0; h < 6; h++) { t.hist[h] += c.hist[h]; t.histUs[h] += c.histUs[h]; }
    if (c.worstUs > t.worstUs) t.worstUs = c.worstUs;
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
    bool found = findFile(root, path, sizeof(path), want, 0, true);
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

} // namespace

void trackerProfileRun() {
    static const char list[] = PROFILE_FILES;
    static const char midiList[] = PROFILE_MIDI_FILES;
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
            if (!p->load(path)) {
                Serial.printf("PROFILE loadfail %s: %s\n", path, p->errorMessage());
                p->~TrackerPlayer();
                continue;
            }
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
            TrackerPlayer::profile = {};
            uint32_t u0 = Synth::wavStreamUnderrunSamples();
            TrackerPlayer::Profile total = {};
            uint32_t win = millis();
            while (millis() - start < 1000u + PROFILE_SECONDS * 1000u && p->state() == TrackerPlayer::STATE_PLAYING) {
                p->update();
                if (millis() - win >= 5000) {
                    TrackerPlayer::Profile& c = TrackerPlayer::profile;
                    report("5s", want, c, Synth::wavStreamUnderrunSamples() - u0);
                    addProfile(total, c);
                    c = {};
                    win = millis();
                }
            }
            TrackerPlayer::Profile& c = TrackerPlayer::profile;
            addProfile(total, c);
            report("RESULT", want, total, Synth::wavStreamUnderrunSamples() - u0);
            p->stop();
            p->~TrackerPlayer();
        }
        Serial.println("PROFILE done");
        delay(5000);
    }
}
#endif

// Profile build (env:pico_profile, -D TRACKER_PROFILE): at boot plays each
// file named in PROFILE_FILES ('|' between name parts, matched against
// file names on the card) for PROFILE_SECONDS and prints over serial how
// much of real time core 0 spends in the player's update(), the time in
// sample cache refills and the ring underruns. Then repeats.
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
#include "pins.h" // SAMPLE_RATE

#ifndef PROFILE_FILES
#define PROFILE_FILES "Lucky Star|All The 7"
#endif
#ifndef PROFILE_SECONDS
#define PROFILE_SECONDS 30
#endif

namespace {

bool isTracker(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mod") || !strcasecmp(dot, ".s3m") || !strcasecmp(dot, ".xm") || !strcasecmp(dot, ".it"));
}

// Depth-first search for a tracker file whose name contains want.
bool findFile(FsFile& dir, char* path, size_t cap, const char* want, int depth) {
    FsFile e;
    size_t base = strlen(path);
    while (e.openNext(&dir, O_RDONLY)) {
        char name[200];
        e.getName(name, sizeof(name));
        bool isDir = e.isDir();
        if (name[0] != '.' && base + 1 + strlen(name) + 1 < cap) {
            path[base] = '/';
            strcpy(path + base + 1, name);
            if (isDir && depth < 6) {
                if (findFile(e, path, cap, want, depth + 1)) { e.close(); return true; }
            } else if (!isDir && strstr(name, want) && isTracker(name)) {
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
}

} // namespace

void trackerProfileRun() {
    static const char list[] = PROFILE_FILES;
    for (;;) {
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
            Serial.printf("PROFILE playing %s\n", path);
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
                    total.updateUs += c.updateUs; total.frames += c.frames; total.refillUs += c.refillUs;
                    total.refills += c.refills; if (c.worstUs > total.worstUs) total.worstUs = c.worstUs;
                    c = {};
                    win = millis();
                }
            }
            TrackerPlayer::Profile& c = TrackerPlayer::profile;
            total.updateUs += c.updateUs; total.frames += c.frames; total.refillUs += c.refillUs;
            total.refills += c.refills; if (c.worstUs > total.worstUs) total.worstUs = c.worstUs;
            report("RESULT", want, total, Synth::wavStreamUnderrunSamples() - u0);
            p->stop();
            p->~TrackerPlayer();
        }
        Serial.println("PROFILE done");
        delay(5000);
    }
}
#endif

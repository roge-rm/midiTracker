// Feeds noisy voltage sequences through BatteryGauge and checks percent,
// charging, full and the low flag. make -C test/host battery-check
#include "battery_gauge.h"
#include <stdio.h>
#include <stdlib.h>

using namespace BatteryGauge;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint32_t g_seed = 1;
static int rnd(int n) {  // -n..n
    g_seed = g_seed * 1103515245u + 12345u;
    return (int)((g_seed >> 8) % (2 * n + 1)) - n;
}

// Noise like the device's: each read off by up to 200 mV (load ripple
// caught at random points), plus up to 30 mV that holds for a second.
struct Sim {
    Gauge g;
    uint32_t t = 0;
    int slow = 0;
    int changes = 0, chgFlips = 0;
    uint32_t minS = 99999, maxS = 0;
    void block(double mv, bool power) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < BLOCK_READS; i++) {
            if (t % 1000 == 0) slow = rnd(30);
            sum += (uint32_t)(mv + slow + rnd(200));
            t += SAMPLE_MS;
        }
        int p = g.percent();
        bool c = g.charging();
        g.feed(sum / BLOCK_READS, power);
        if (g.count > 1) {
            if (p != g.percent()) changes++;
            if (c != g.charging()) chgFlips++;
        }
    }
    void track() {
        if (g.smoothedMv() < minS) minS = g.smoothedMv();
        if (g.smoothedMv() > maxS) maxS = g.smoothedMv();
    }
};

const uint32_t PER_MIN = 60000 / BLOCK_MS;

int main() {
    CHECK(lipoPercent(4250) == 100, "4250");
    CHECK(lipoPercent(3900) == 68, "3900");
    CHECK(lipoPercent(3725) == 40, "3725 %d", lipoPercent(3725));
    CHECK(lipoPercent(3300) == 0, "3300");
    CHECK(lipoPercent(3000) == 0, "3000");

    // Steady 4.08 V off power: smoothed within 10 mV, percent still.
    {
        Sim s;
        for (uint32_t i = 0; i < 2 * PER_MIN; i++) s.block(4080, false);
        int c0 = s.changes;
        for (uint32_t i = 0; i < 30 * PER_MIN; i++) { s.block(4080, false); s.track(); }
        CHECK(s.minS >= 4070 && s.maxS <= 4090, "steady: smoothed %u-%u", s.minS, s.maxS);
        CHECK(s.changes == c0, "steady: %d percent changes", s.changes - c0);
        CHECK(!s.g.charging(), "steady: charging");
        printf("steady: smoothed %u-%u, %d%%\n", s.minS, s.maxS, s.g.percent());
    }

    // Falling: 4.10 V to 3.35 V over 3 hours, off power.
    {
        Sim s;
        const uint32_t n = 180 * PER_MIN;
        int lastP = 101, lowFlips = 0;
        bool low = false;
        for (uint32_t i = 0; i < n; i++) {
            s.block(4100.0 - 750.0 * i / n, false);
            CHECK(!s.g.charging(), "falling: charging");
            if (i > PER_MIN)
                CHECK(s.g.percent() <= lastP, "falling: rose to %d, smooth %u", s.g.percent(), s.g.smoothedMv());
            lastP = s.g.percent();
            if (s.g.low() != low) { low = s.g.low(); lowFlips++; }
        }
        CHECK(s.g.percent() <= 5, "falling: ends at %d", s.g.percent());
        CHECK(lowFlips == 1, "falling: low flipped %d times", lowFlips);
        CHECK(s.changes <= 60, "falling: %d percent changes", s.changes);
        printf("falling: %d percent changes, ends %d%%\n", s.changes, s.g.percent());
    }

    // On USB at 3.70 V: charging, rising 4 mV a minute to 4.20 V, then full.
    {
        Sim s;
        for (uint32_t i = 0; i < 5 * PER_MIN; i++) s.block(3700, false);
        CHECK(!s.g.charging(), "before plug: charging");
        s.block(3760, true);
        CHECK(s.g.charging(), "plug: not charging");
        double v = 3760;
        for (uint32_t i = 0; i < 180 * PER_MIN; i++) {
            v += 4.0 / PER_MIN;
            if (v > 4200) v = 4200;
            s.block(v, true);
        }
        CHECK(s.g.charging(), "charged: still on power, not charging");
        CHECK(s.g.percent() >= 98, "charged: %d%%", s.g.percent());
        CHECK(s.chgFlips == 1, "charged: %d charging flips", s.chgFlips);

        // Unplugged when full, easing to 4.12 V: no charging, percent from voltage.
        for (uint32_t i = 0; i < 10 * PER_MIN; i++) {
            s.block(4160 - (i < 5 * PER_MIN ? 40.0 * i / (5 * PER_MIN) : 40.0), false);
            CHECK(!s.g.charging(), "unplugged: charging %d", s.g.charging());
        }
        CHECK(s.g.percent() >= 90 && s.g.percent() <= 95, "unplugged: %d%%", s.g.percent());
        printf("charge and unplug: ends %d%%\n", s.g.percent());
    }

    // On USB with a battery sitting near the full edge: no flapping.
    {
        Sim s;
        for (uint32_t i = 0; i < 60 * PER_MIN; i++) s.block(4140, true);
        CHECK(s.chgFlips == 0, "edge: charging flipped %d times", s.chgFlips);
    }

    printf(g_fail ? "battery-check: %d failed\n" : "battery-check: ok\n", g_fail);
    return g_fail ? 1 : 0;
}

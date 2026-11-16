#include "battery_gauge.h"

namespace BatteryGauge {
namespace {

struct Point { uint16_t mv; uint8_t pct; };

// Typical single LiPo at light load, highest first.
const Point TABLE[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 68}, {3800, 55}, {3750, 45},
    {3700, 35},  {3650, 25}, {3600, 15}, {3500, 7},  {3400, 3},  {3300, 0},
};
const int TABLE_N = sizeof(TABLE) / sizeof(TABLE[0]);

// Smoothing weight in blocks: 120 x 500 ms is about 60 s.
const uint32_t WEIGHT = 120;

// On power, full from 4150 mV, back to charging below 4120 mV.
const uint32_t FULL_ON_MV = 4150, FULL_OFF_MV = 4120;

// The percent shown comes from a held voltage that follows the smoothed one
// only when it moves HOLD_MV or more, past the noise left after smoothing.
// It follows freely while the average settles, the first QUICK blocks.
const uint32_t HOLD_MV = 20;
const uint32_t QUICK = WEIGHT;
// Low at 15% or less, back to normal above 18%.
const int LOW_ON = 15, LOW_OFF = 18;

} // namespace

uint8_t lipoPercent(uint32_t mv) {
    if (mv >= TABLE[0].mv) return 100;
    for (int i = 1; i < TABLE_N; i++) {
        const Point& lo = TABLE[i];
        if (mv >= lo.mv) {
            const Point& hi = TABLE[i - 1];
            uint32_t span = hi.mv - lo.mv;
            return (uint8_t)(lo.pct + ((mv - lo.mv) * (hi.pct - lo.pct) + span / 2) / span);
        }
    }
    return 0;
}

void Gauge::feed(uint32_t mv, bool power) {
    uint32_t w = count < WEIGHT ? count + 1 : WEIGHT;
    if (count < WEIGHT) count++;
    int32_t d = (int32_t)(mv << 8) - (int32_t)smooth256;
    smooth256 = (uint32_t)((int32_t)smooth256 + d / (int32_t)w);
    uint32_t s = smoothedMv();

    // Charging is only known from USB power, so the voltage can't fool it.
    if (!power) isFull = false;
    else if (s >= FULL_ON_MV) isFull = true;
    else if (s < FULL_OFF_MV) isFull = false;
    isCharging = power && !isFull;

    if (count < QUICK || s >= heldMv + HOLD_MV || s + HOLD_MV <= heldMv) heldMv = s;
    shown = isFull ? 100 : lipoPercent(heldMv);
    if (shown <= LOW_ON) isLow = true;
    else if (shown > LOW_OFF) isLow = false;
}

} // namespace BatteryGauge

#include "battery_gauge.h"

namespace BatteryGauge {
namespace {

struct Point { uint16_t mv; uint8_t pct; };

// Single LiPo at light load, highest first. This board reads a full cell
// at about 4.05 V once the charger stops, so that's 100%.
const Point TABLE[] = {
    {4050, 100}, {4000, 88}, {3900, 68}, {3800, 55}, {3750, 45},
    {3700, 35},  {3650, 25}, {3600, 15}, {3500, 7},  {3400, 3},  {3300, 0},
};
const int TABLE_N = sizeof(TABLE) / sizeof(TABLE[0]);

// Smoothing weight in blocks: 120 x 500 ms is about 60 s.
const uint32_t WEIGHT = 120;

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

    // Charging means on USB power. The charger holds the cell near 4.2 V
    // until it's done, so the voltage can't tell full from charging.
    isCharging = power;

    if (count < QUICK || s >= heldMv + HOLD_MV || s + HOLD_MV <= heldMv) heldMv = s;
    shown = lipoPercent(heldMv);
    if (shown <= LOW_ON) isLow = true;
    else if (shown > LOW_OFF) isLow = false;
}

} // namespace BatteryGauge

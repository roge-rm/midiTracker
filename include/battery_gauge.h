#pragma once
#include <stdint.h>

// Battery gauge maths, kept free of Arduino calls so the host check can
// drive it. Battery feeds it a block average every BatteryGauge::BLOCK_MS.
namespace BatteryGauge {

const uint32_t SAMPLE_MS = 25;  // one ADC read
const uint32_t BLOCK_READS = 20;
const uint32_t BLOCK_MS = SAMPLE_MS * BLOCK_READS;

// Percent left for a single LiPo at light load, from a piecewise-linear
// table. mv is the cell voltage in millivolts.
uint8_t lipoPercent(uint32_t mv);

struct Gauge {
    // mv: block average. power: on USB (or VBUS) power.
    void feed(uint32_t mv, bool power);

    uint32_t smoothedMv() const { return (smooth256 + 128) >> 8; }
    uint8_t percent() const { return shown; }
    bool charging() const { return isCharging; }
    bool full() const { return isFull; }
    bool low() const { return isLow; }

    // Exponential average in mV * 256, about 60 s. count ramps the weight
    // up so the first readings settle at once.
    uint32_t smooth256 = 0;
    uint32_t count = 0;
    uint32_t heldMv = 0;   // voltage the percent shown comes from
    uint8_t shown = 100;
    bool isCharging = false;
    bool isFull = false;
    bool isLow = false;
};

} // namespace BatteryGauge

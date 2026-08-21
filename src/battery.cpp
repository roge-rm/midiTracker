#include "battery.h"
#include "pins.h"

namespace Battery {
namespace {

uint8_t g_percentage = 100;
bool g_charging = false;
bool g_haveSample = false;
uint32_t g_lastSampleMs = 0;
const uint32_t SAMPLE_INTERVAL_MS = 5000;

} // namespace

void begin() {
    // Match picoTracker's raw ADC math below, which assumes the full
    // 12-bit 0-4095 range -- the earlephilhower core defaults to 10-bit
    // (Arduino-standard 0-1023) otherwise.
    analogReadResolution(12);
}

void update() {
    uint32_t now = millis();
    if (g_haveSample && (now - g_lastSampleMs) < SAMPLE_INTERVAL_MS) return;
    g_lastSampleMs = now;
    g_haveSample = true;

    uint32_t adcReading = analogRead(BATTERY_VOLTAGE_PIN);

    // Same math as picoTracker's picoTrackerSystem::GetBatteryState():
    // the RP2040's ADC is ~0.8mV/count at its 3.3V/12-bit range, doubled
    // for this board's 2:1 battery voltage divider -- mV = counts * 1.6,
    // done here in integer math as counts * 8 / 5.
    uint32_t voltageMv = (adcReading * 8) / 5;

    uint8_t pct;
    if (voltageMv < 3325) {
        pct = 0;
    } else if (voltageMv > 3900) {
        pct = 100;
    } else {
        // Same quadratic curve picoTracker fits to their measured LiPo
        // discharge profile: 100 - (100*mv - 390,000)^2 / 33,000,000,
        // with the division approximated as a >>25 (2^25 = 33,554,432).
        // q's intermediate subtraction goes negative in real-number terms
        // for mv near the low end of this range, but stays correct here:
        // uint32_t wraparound is well-defined modular arithmetic, and
        // squaring commutes with that wraparound the same way it would
        // with a true negative value, so this needs no special-casing --
        // ported byte-for-byte from picoTracker's own implementation.
        uint32_t q = 100 * voltageMv - 390000;
        q *= q;
        q >>= 25;
        pct = (uint8_t)(100 - q);
    }
    g_percentage = pct;

    // picoTracker's own heuristic: a charger/USB feed pushes the sensed
    // voltage above what any real battery-only reading reaches.
    g_charging = voltageMv > 4000;
}

uint8_t percentage() { return g_percentage; }
bool isCharging() { return g_charging; }

} // namespace Battery

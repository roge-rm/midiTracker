#include "battery.h"
#include "battery_gauge.h"
#include "pins.h"
#include <Adafruit_TinyUSB.h>

namespace Battery {
namespace {

BatteryGauge::Gauge g_gauge;
uint32_t g_sampleAtMs = 0;
uint32_t g_sum = 0;         // ADC counts in the block so far
uint32_t g_reads = 0;
uint32_t g_lastRaw = 0;     // ADC counts, average of the last block
uint32_t g_lastMv = 0;      // mV of the last block

// On external power: VBUS sense when the board has it, otherwise a USB
// host. A charger with no data lines doesn't count without VBUS sense.
bool onPower() {
#ifdef BATTERY_VBUS_PIN
    return digitalRead(BATTERY_VBUS_PIN) != 0;
#else
    return TinyUSBDevice.mounted();
#endif
}

#ifdef BATTERY_DIAG
uint32_t g_diagAtMs = 0;

// GPIO24 read with each pull, then left with the pull-down.
int readPin24(bool pullUp) {
    pinMode(24, pullUp ? INPUT_PULLUP : INPUT_PULLDOWN);
    delayMicroseconds(100);
    return digitalRead(24);
}

void diagPrint(uint32_t now) {
    if (now - g_diagAtMs < 1000) return;
    g_diagAtMs = now;
    int up = readPin24(true);
    int down = readPin24(false);
    Serial.printf("raw %lu mv %lu smooth %lu pct %u power %d chg %d full %d low %d gp24 down %d up %d BATT\n",
                  (unsigned long)g_lastRaw, (unsigned long)g_lastMv,
                  (unsigned long)g_gauge.smoothedMv(), g_gauge.percent(), onPower() ? 1 : 0,
                  g_gauge.charging() ? 1 : 0, g_gauge.full() ? 1 : 0, g_gauge.low() ? 1 : 0,
                  down, up);
}
#endif

// Turns the block in g_sum into mV and feeds the gauge.
void feedBlock() {
    const uint32_t n = BatteryGauge::BLOCK_READS;
    g_lastRaw = (g_sum + n / 2) / n;
    // 3.3 V over 4096 counts, doubled for the 2:1 divider.
    g_lastMv = (g_sum * 6600 + 2048 * n) / (4096 * n);
    g_sum = 0;
    g_reads = 0;
    g_gauge.feed(g_lastMv, onPower());
}

} // namespace

void begin() {
    analogReadResolution(12);
#ifdef BATTERY_VBUS_PIN
    pinMode(BATTERY_VBUS_PIN, INPUT_PULLDOWN);
#endif
    // One block straight away (about 20 ms) so the first screen shows a
    // real level instead of a full battery.
    for (uint32_t i = 0; i < BatteryGauge::BLOCK_READS; i++) {
        g_sum += analogRead(BATTERY_VOLTAGE_PIN);
        delay(1);
    }
    feedBlock();
    g_sampleAtMs = millis();
}

void update() {
    uint32_t now = millis();
#ifdef BATTERY_DIAG
    diagPrint(now);
#endif
    if (g_reads && now - g_sampleAtMs < BatteryGauge::SAMPLE_MS) return;
    g_sampleAtMs = now;
    g_sum += analogRead(BATTERY_VOLTAGE_PIN);
    if (++g_reads < BatteryGauge::BLOCK_READS) return;
    feedBlock();
}

uint8_t percentage() { return g_gauge.percent(); }
bool isCharging() { return g_gauge.charging(); }
bool isLow() { return g_gauge.low(); }

} // namespace Battery

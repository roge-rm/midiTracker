#pragma once
#include <Arduino.h>

// Reads the onboard LiPo battery voltage divider via ADC (see pins.h's
// BATTERY_VOLTAGE_PIN) and derives a percentage + charging estimate.
// This board reuses picoTracker's own hardware (same GPIO for the
// battery sense line), so the math here is ported directly from
// picoTracker's own firmware (picoTrackerSystem::GetBatteryState() in
// xiphonics/picoTracker) rather than re-derived from scratch.
namespace Battery {

void begin();

// Call every loop() iteration; internally throttles the actual ADC
// sample (and the percentage/charging recompute) to once per ~5s --
// battery voltage doesn't move meaningfully faster than that, and this
// avoids paying analogRead()'s brief blocking cost any more often than
// the display would even show a difference.
void update();

uint8_t percentage(); // 0-100, clamped
bool isCharging();

} // namespace Battery

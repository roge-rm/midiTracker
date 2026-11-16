#pragma once
#include <Arduino.h>

// Reads the LiPo voltage on BATTERY_VOLTAGE_PIN (see pins.h) through the
// board's 2:1 divider and gives a steady percent, charging and low flag.
// The maths lives in battery_gauge.h.
//
// Charging means on USB power below about 4.15 V; above that it shows full.
// USB power is a mounted USB host, so a charger with no data lines doesn't
// show. Build with -D BATTERY_VBUS_PIN=N to take it from a VBUS sense pin
// (high = USB power) instead.
// -D BATTERY_DIAG prints a line ending in "BATT" over USB serial each second.
namespace Battery {

void begin();

// Call every loop() iteration. Takes one ADC read (a few us) every 25 ms
// and feeds the gauge the average of each 20.
void update();

uint8_t percentage(); // 0-100
bool isCharging();
bool isLow();         // at 15% or less, clears above 18%

} // namespace Battery

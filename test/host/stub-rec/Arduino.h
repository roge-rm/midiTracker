// Host stand-in for Arduino.h, for the recorder check. The clock is set by the test.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern uint32_t g_hostMillis;
inline uint32_t millis() { return g_hostMillis; }

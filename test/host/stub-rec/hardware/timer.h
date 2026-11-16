#pragma once
#include <Arduino.h>
inline uint64_t time_us_64() { return (uint64_t)g_hostMillis * 1000; }

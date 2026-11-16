#pragma once
// Just enough Arduino for ui_look.cpp on the host.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
inline uint32_t micros() { return 0; }
inline uint32_t millis() { return 0; }

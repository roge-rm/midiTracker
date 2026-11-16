#pragma once
// Just enough Arduino for ui_look.cpp and ui_player.cpp on the host. The
// renderer moves the clock to step the live updates.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
extern uint32_t g_hostMs;
inline uint32_t micros() { return g_hostMs * 1000; }
inline uint32_t millis() { return g_hostMs; }

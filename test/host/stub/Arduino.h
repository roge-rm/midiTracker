// Host stand-in for the bits of Arduino.h the tracker players use.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define __not_in_flash_func(f) f

uint32_t micros();
uint32_t millis();

template <class T, class L>
auto min(const T& a, const L& b) -> decltype((b < a) ? b : a) { return (b < a) ? b : a; }
template <class T, class L>
auto max(const T& a, const L& b) -> decltype((b < a) ? b : a) { return (a < b) ? b : a; }

struct HostSerial {
    template <class... A> int printf(const char* f, A... a) { return fprintf(stderr, f, a...); }
    void println(const char* s) { fprintf(stderr, "%s\n", s); }
};
extern HostSerial Serial;

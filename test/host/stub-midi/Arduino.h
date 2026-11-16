// Host stand-in for what synth.cpp needs from the Arduino core, on top of the tracker stub.
#pragma once
#include "../stub/Arduino.h"
#include <deque>

#define OUTPUT 1
#define PROGMEM
#define M_PI_F 3.14159265358979f

// Core 0 to core 1 queue: unbounded, drained by loop1() on the same thread.
struct HostFifo {
    std::deque<uint32_t> q;
    void push(uint32_t v) { q.push_back(v); }
    bool push_nb(uint32_t v) { q.push_back(v); return true; }
    bool pop_nb(uint32_t* v) { if (q.empty()) return false; *v = q.front(); q.pop_front(); return true; }
};
struct HostRp2040 { HostFifo fifo; };
extern HostRp2040 rp2040;

void setup1();
void loop1();

// Host stand-in for the I2S output: write16() appends to a capture buffer.
#pragma once
#include <Arduino.h>
#include <vector>

inline std::vector<int16_t> g_i2sCapture; // interleaved L/R frames written

class I2S {
public:
    I2S(int, int, int) {}
    void setBuffers(int, int) {}
    void setBitsPerSample(int) {}
    bool begin(long rate) { _rate = rate; return true; }
    void write16(int16_t l, int16_t r) { g_i2sCapture.push_back(l); g_i2sCapture.push_back(r); }
    long _rate = 0;
};

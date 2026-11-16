#pragma once
#include <stdint.h>
#define HOST_SAMPLE_RATE 44100
void hostSetFrames(uint64_t frames); // sets what micros() reports

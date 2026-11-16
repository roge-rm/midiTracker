// Host stand-in for the synth's WAV ring: the harness captures frames.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Synth {
void wavStreamReset();
void wavStreamSetActive(bool active);
size_t wavStreamWrite(const int16_t* frames, size_t count);
size_t wavStreamFree();
void wavStreamEnd();
bool wavStreamTookUnderrun();
uint32_t wavStreamUnderrunSamples();
uint32_t wavStreamWritten();
uint32_t wavStreamPlayed();
size_t wavStreamPeek(int16_t* out, size_t frames);
}

// Harness side of the ring.
namespace HostRing {
void drain();                  // the consumer took everything written so far
size_t framesCaptured();
const int16_t* data();
}

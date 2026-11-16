#pragma once
// The WAV ring calls ui_player.cpp makes; look_render plays a render back.
#include <stdint.h>
#include <stddef.h>
namespace Synth {
uint32_t wavStreamWritten();
uint32_t wavStreamPlayed();
size_t wavStreamPeek(int16_t* out, size_t frames);
}

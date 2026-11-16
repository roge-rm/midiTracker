#pragma once
// The note state calls ui_player.cpp makes; look_render feeds a snapshot.
#include <stdint.h>
namespace MidiOutput {
bool isNoteActive(uint8_t note);
bool isNotePercussion(uint8_t note);
uint8_t noteVelocity(uint8_t note);
uint16_t activeChannelMask();
}

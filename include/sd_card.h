#pragma once
#include <SdFat.h>

// Single shared SdFat volume instance used by both the browser and the
// MIDI player (the player opens track chunks directly by path/position).
extern SdFs sd;

// Mounts the card over SDIO using SDIO_CLK/SDIO_CMD/SDIO_D0 from pins.h
// (SdFat only needs DAT0 -- DAT1..3 are inferred as the next 3 GPIOs).
// Returns true on success; on failure call sd.card()->errorCode() /
// sd.sdErrorCode() for diagnostics.
bool sdCardBegin();

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

// Path helpers for paths built by SdBrowser. A path component of the form
// "*<n>" (never a valid FAT name) stands for the entry with directory
// index n in its folder, used for names that are too long or not plain
// ASCII. Paths without one behave exactly like the plain SdFat calls.
bool sdOpenPath(FsFile& f, const char* path, oflag_t flags = O_RDONLY);
bool sdExists(const char* path);
bool sdRemove(const char* path);
bool sdRmdir(const char* path);
bool sdMkdir(const char* path);
bool sdRename(const char* oldPath, const char* newPath);

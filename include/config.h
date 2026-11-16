#pragma once

// Maximum number of tracks a Standard MIDI File can have and still be
// played by this firmware. Each track holds an open SdFat file handle
// and a small read-ahead buffer (about 160 bytes of heap apiece while a
// MIDI file is loaded, allocated for the file's own track count).
#define MIDI_MAX_TRACKS 64

// Button debounce time, ms
#define BUTTON_DEBOUNCE_MS 25

// How many directory entries are visible on screen at once in the browser.
// Recomputed at runtime from font metrics, this is just the array cap.
#define BROWSER_MAX_VISIBLE_ROWS 16

// Longest file/dir name (8.3 + long name) we'll keep in memory per entry.
#define MAX_FILENAME_LEN 64

// Largest System Exclusive message for MIDI thru, the MIDI recorder and
// SysEx inside a .mid (including the leading F0 and trailing F7). The MIDI
// input instances in midi_output.cpp use this as their SysExMaxSize, one
// buffer of this size each; longer messages are dropped there. SysEx
// capture and .syx sending stream and have no limit.
#define MIDI_SYSEX_MAX_LEN 1024

// Root directory to browse. Change if your card stores MIDI files elsewhere.
#define BROWSE_ROOT "/"

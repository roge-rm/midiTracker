#pragma once

// Maximum number of tracks a Standard MIDI File can have and still be
// played by this firmware. Each track needs one open SdFat file handle
// for the whole duration of playback (~108 bytes of RAM apiece -- no
// per-file sector buffer, SdFat's cache is shared at the volume level),
// so there's plenty of headroom to raise this well past what any real
// SMF is likely to contain.
#define MIDI_MAX_TRACKS 64

// Button debounce time, ms
#define BUTTON_DEBOUNCE_MS 25

// How many directory entries are visible on screen at once in the browser.
// Recomputed at runtime from font metrics, this is just the array cap.
#define BROWSER_MAX_VISIBLE_ROWS 16

// Longest file/dir name (8.3 + long name) we'll keep in memory per entry.
#define MAX_FILENAME_LEN 64

// Largest single System Exclusive message (boundary-inclusive, i.e.
// including the leading F0 and trailing F7) this firmware will buffer in
// RAM at once -- for receiving, capturing, and playing one back. Matches
// the MIDI library's own SysExMaxSize (its default receive-side cap, see
// midi_Defs.h/midi_Settings.h), so a message the library can even accept
// is guaranteed to fit here too; anything longer (which the library
// itself would already refuse) is silently dropped rather than raising
// this further, since a single 1KB+ stack/static buffer is already a
// meaningful RAM commitment on a 256KB device.
#define MIDI_SYSEX_MAX_LEN 1024

// Root directory to browse. Change if your card stores MIDI files elsewhere.
#define BROWSE_ROOT "/"

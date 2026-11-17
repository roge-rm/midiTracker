#pragma once
#include <stddef.h>

// A single fixed-address static RAM block shared by LooperMode::tracks[]
// and FilePlayerMode's ModPlayer/S3mPlayer/XmPlayer/ItPlayer -- the biggest
// allocations in the firmware (tracks[] ~98KB; the tracker-format players
// ~73KB/~91KB/~99KB/~102KB), never live at the same time. LooperMode's existing
// "free tracks[] before opening a tracker file" gate (see
// FilePlayerMode::openSelected()'s FILE_MOD/S3M/XM/IT handling)
// already guarantees tracks[] is down before any of the players
// come up, and the players are mutually exclusive with each other
// too -- openSelected() is only reachable from APP_BROWSE, and each
// player frees+nulls itself before returning there (see e.g.
// handlePlayModInput()'s "LEFT -> back to browser" handler). MidiPlayer is
// deliberately NOT part of this -- MIDI preview is the one on-demand
// player that was never gated behind freeing tracks[] (browsing/preview/
// WAV playback never touch that gate), so its lifetime isn't guaranteed
// disjoint from tracks[]'s the way the others' is.
//
// Mp3Player uses it too, from load() until close(): the start of the arena
// holds the Helix decoder's state, the compressed input ring and one decoded
// frame (Mp3Player::arenaBytes(), about 40 KB), and core 1 reads and writes
// it while an MP3 plays. FlacPlayer does the same with dr_flac's state, one
// decoded block, the input ring and one output chunk (FlacPlayer::arenaUsed(),
// up to all of it). FilePlayerMode opens an MP3 or FLAC only once tracks[]
// is down and no tracker player holds the arena.
//
// Backing all four with the same static bytes instead of separate heap
// allocations means claiming any one of them is never subject to heap
// fragmentation: there's no allocator search involved at all, just
// placement-new at a compile-time-fixed address. This exists because a
// real-hardware freeze showed new(nothrow) LoopTrack[NUM_TRACKS] (131KB)
// could hang indefinitely once the heap had already served other,
// smaller, variably-sized allocations first (SdBrowser scanning a folder
// with tens of thousands of files, specifically) -- even though nominal
// free-byte totals looked comfortable. A fixed static block sidesteps the
// underlying allocator's fragmentation behavior entirely rather than
// trying to out-guess it.
namespace SharedRamArena {

// Sized to ItPlayer (104372 bytes on this build), the largest user;
// LooperMode sizes its tracks to fill it. Each placement-new call site
// carries its own static_assert against this, so a future growth in any
// of these types past what fits here is a compile error, not a silent overlap.
// The host test build has 8-byte pointers, so its players are a bit bigger.
constexpr size_t SIZE = sizeof(void*) == 4 ? 104384 : 106496;

void* data();

// A small block shared by short-lived lists that are never in use together:
// Settings' Load Theme list, SynthEditor's Load Bank list and LooperMode's
// saved session list (nameList), and LooperMode's MIDI import track readers
// (bytes, only alive inside one importMidiEvents() call), and WavPlayer's
// read buffer (bytes, while a WAV is open in the file player). The name lists
// are each in a different mode, only one mode runs at a time, each list is
// rescanned from the card every time its screen opens, and an import never
// runs from one of those screens. A WAV only plays on the file player's WAV
// screen, which none of the others are reachable from.
constexpr int NAME_LIST_COUNT = 64;
constexpr int NAME_LIST_LEN = 17; // 16 chars + terminator
constexpr size_t SCRATCH_SIZE = 1728;
union Scratch {
    char nameList[NAME_LIST_COUNT][NAME_LIST_LEN];
    alignas(16) unsigned char bytes[SCRATCH_SIZE];
};
extern Scratch scratch;

} // namespace SharedRamArena

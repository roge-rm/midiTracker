#pragma once
#include <stddef.h>

// A single fixed-address static RAM block shared by LooperMode::tracks[]
// and FilePlayerMode's ModPlayer/S3mPlayer/XmPlayer -- the four biggest
// allocations in the firmware (tracks[] ~131KB; the tracker-format players
// 11KB/25.5KB/~64KB), never live at the same time. LooperMode's existing
// "free tracks[] before opening a tracker file" gate (see
// FilePlayerMode::openSelected()'s FILE_MOD/FILE_S3M/FILE_XM handling)
// already guarantees tracks[] is down before any of the three players
// come up, and the three players are mutually exclusive with each other
// too -- openSelected() is only reachable from APP_BROWSE, and each
// player frees+nulls itself before returning there (see e.g.
// handlePlayModInput()'s "LEFT -> back to browser" handler). MidiPlayer is
// deliberately NOT part of this -- MIDI preview is the one on-demand
// player that was never gated behind freeing tracks[] (browsing/preview/
// WAV playback never touch that gate), so its lifetime isn't guaranteed
// disjoint from tracks[]'s the way the other three's is.
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

// Sized to LoopTrack[NUM_TRACKS] (~131312 bytes measured on real
// hardware -- see looper_mode.cpp), comfortably bigger than any of
// ModPlayer/S3mPlayer/XmPlayer. Each placement-new call site carries its
// own static_assert against this, so a future growth in any of these
// types past what fits here is a compile error, not a silent overlap.
constexpr size_t SIZE = 131312;

void* data();

} // namespace SharedRamArena

#pragma once
#include <stdint.h>
#include "file_kind.h"

// What the file player does when a file ends. Saved in settings.txt as its
// number, so keep the order.
enum PlayMode : uint8_t { PLAY_ONCE, PLAY_REPEAT, PLAY_FOLDER, PLAY_MODE_COUNT };

inline const char* playModeName(PlayMode m) {
    switch (m) {
        case PLAY_REPEAT: return "Repeat";
        case PLAY_FOLDER: return "Folder";
        default:          return "Once";
    }
}

// The player screens' status strip tag.
inline const char* playModeTag(PlayMode m) {
    switch (m) {
        case PLAY_REPEAT: return "RPT";
        case PLAY_FOLDER: return "DIR";
        default:          return "ONCE";
    }
}

inline PlayMode nextPlayMode(PlayMode m) { return (PlayMode)((m + 1) % PLAY_MODE_COUNT); }

// The kinds Folder mode plays. A .syx is a dump to send, not a song.
inline bool playModeKind(FileKind k) { return k != FILE_SYX; }

// The next entry after `from` that isPlayable(i) accepts, in list order,
// wrapping from the last to the first, with `from` itself tried last.
// -1 if none.
template <class F>
int nextPlayable(int count, int from, F isPlayable) {
    if (count <= 0) return -1;
    if (from < 0 || from >= count) from = count - 1;
    for (int step = 1; step <= count; step++) {
        int i = (from + step) % count;
        if (isPlayable(i)) return i;
    }
    return -1;
}

// Folder mode's step: opens the next playable entry after `from` with
// open(i), which returns 1 when it plays, 0 when it failed to load and -1
// when it handed off (the caller's state changed, stop here). Moves past
// failures, at most once round the folder. Returns the last index tried,
// or -1 if there was nothing to try. Sets played.
template <class F, class O>
int advanceFolder(int count, int from, F isPlayable, O open, bool& played) {
    played = false;
    int last = -1;
    int i = from;
    for (int tries = 0; tries < count; tries++) {
        i = nextPlayable(count, i, isPlayable);
        if (i < 0) break;
        last = i;
        int r = open(i);
        if (r != 0) {
            played = r > 0;
            break;
        }
        if (i == from) break; // round the folder and back to where it started
    }
    return last;
}

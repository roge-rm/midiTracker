// Folder play mode's next-file choice (include/play_mode.h) on made-up
// folder listings: order, wrapping, skipping folders, .syx and files that
// fail to load.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "play_mode.h"

namespace {

struct Entry { bool isDir; FileKind kind; bool loads; };

int g_fail = 0;

void expect(const char* what, int got, int want) {
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        g_fail++;
    }
}

// Runs advanceFolder() over list from `from`, returns the index it
// stopped on and the indexes it tried.
int step(const std::vector<Entry>& list, int from, bool& played, std::vector<int>& tried) {
    tried.clear();
    auto playable = [&](int i) { return !list[i].isDir && playModeKind(list[i].kind); };
    auto open = [&](int i) { tried.push_back(i); return list[i].loads ? 1 : 0; };
    return advanceFolder((int)list.size(), from, playable, open, played);
}

} // namespace

int main() {
    const Entry D = {true, FILE_MID, true};
    // folders first, as the browser sorts them
    std::vector<Entry> list = {
        D, D,
        {false, FILE_MID, true},  // 2
        {false, FILE_SYX, true},  // 3, not played
        {false, FILE_MP3, true},  // 4
        {false, FILE_XM, false},  // 5, fails to load
        {false, FILE_WAV, true},  // 6
        {false, FILE_IT, true},   // 7
    };
    bool played;
    std::vector<int> tried;

    expect("next after mid", step(list, 2, played, tried), 4);
    expect("played", played, 1);
    expect("skips syx", (int)tried.size(), 1);

    expect("past a broken xm", step(list, 4, played, tried), 6);
    expect("tried the xm first", tried.size() == 2 && tried[0] == 5, 1);

    expect("wraps from the last", step(list, 7, played, tried), 2);
    expect("wrap played", played, 1);

    expect("nextPlayable order", nextPlayable((int)list.size(), 6, [&](int i) {
               return !list[i].isDir && playModeKind(list[i].kind);
           }), 7);

    // The only file that plays is the one playing: it comes round again.
    std::vector<Entry> one = {D, {false, FILE_MOD, true}, {false, FILE_SYX, true}, {false, FILE_S3M, false}};
    expect("only one plays", step(one, 1, played, tried), 1);
    expect("only one played", played, 1);

    // Nothing loads: once round, then stop.
    std::vector<Entry> broken = {{false, FILE_MID, false}, {false, FILE_WAV, false}, {false, FILE_IT, false}};
    step(broken, 0, played, tried);
    expect("broken folder stops", played, 0);
    expect("broken folder tries each once", (int)tried.size(), 3);

    // Folders only, or empty: nothing to try.
    std::vector<Entry> dirs = {D, D};
    expect("folders only", step(dirs, 0, played, tried), -1);
    expect("empty", advanceFolder(0, 0, [](int) { return true; }, [](int) { return 1; }, played), -1);

    // A hand-off (the looper's free-tracks gate) stops the step.
    int calls = 0;
    int r = advanceFolder((int)list.size(), 2, [&](int i) { return !list[i].isDir && playModeKind(list[i].kind); },
                          [&](int) { calls++; return -1; }, played);
    expect("hand-off stops", calls, 1);
    expect("hand-off index", r, 4);
    expect("hand-off not played", played, 0);

    expect("mode cycle", nextPlayMode(PLAY_FOLDER), PLAY_ONCE);
    expect("tags", strcmp(playModeTag(PLAY_REPEAT), "RPT"), 0);

    if (g_fail) return 1;
    printf("play-mode-check: all passed\n");
    return 0;
}

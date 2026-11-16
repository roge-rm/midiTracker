#include "ui_tap.h"
#include <string.h>

namespace UiTap {

Tracker g_tracker;
Midi g_midi;

void trackerReset() {
    g_tracker.rowCount = 0;
    g_tracker.levelCount = 0;
    rowBegin();
}

void rowEnd(uint32_t at, int order, int row) {
    Row& r = g_tracker.rows[g_tracker.rowCount % ROW_RING];
    memcpy(&r, &g_tracker.pending, sizeof(Row));
    r.at = at;
    r.order = (uint16_t)order;
    r.row = (uint16_t)row;
    g_tracker.rowCount++;
}

namespace {
// Index (a count, not a slot) of the newest entry stamped at or before
// played, or -1 when every entry kept is still to be heard.
template <class T>
int newestAt(const T* ring, int size, uint32_t count, uint32_t played) {
    if (count == 0) return -1;
    uint32_t oldest = count > (uint32_t)size ? count - size : 0;
    for (uint32_t i = count; i-- > oldest;) {
        if ((int32_t)(ring[i % size].at - played) <= 0) return (int)i;
    }
    return -1;
}
} // namespace

bool rowAt(uint32_t played, Row& out) {
    int i = newestAt(g_tracker.rows, ROW_RING, g_tracker.rowCount, played);
    if (i < 0) return false;
    out = g_tracker.rows[i % ROW_RING];
    return true;
}

bool rowsBefore(uint32_t played, Row* out, int n, int& got) {
    got = 0;
    int i = newestAt(g_tracker.rows, ROW_RING, g_tracker.rowCount, played);
    if (i < 0) return false;
    uint32_t oldest = g_tracker.rowCount > (uint32_t)ROW_RING ? g_tracker.rowCount - ROW_RING : 0;
    int first = i - (n - 1);
    if (first < (int)oldest) first = (int)oldest;
    for (int k = first; k <= i; k++) out[got++] = g_tracker.rows[k % ROW_RING];
    return true;
}

bool levelsAt(uint32_t played, Levels& out) {
    int i = newestAt(g_tracker.levels, LEVEL_RING, g_tracker.levelCount, played);
    if (i < 0) return false;
    out = g_tracker.levels[i % LEVEL_RING];
    return true;
}

} // namespace UiTap

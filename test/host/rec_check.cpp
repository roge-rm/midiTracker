// Checks MidiRecorder and SysExRecorder against an in-memory card
// (stub-rec/): a full card, a failed sync, replacing a file and discarding
// a replacing take. Prints one line per check and exits non-zero on a fail.
#include "midi_recorder.h"
#include "sysex_recorder.h"
#include "sd_card.h"
#include <string>

std::map<std::string, std::vector<uint8_t>> g_files;
long g_cardFree = 1L << 30;
bool g_syncFails = false;
std::set<std::string> g_locked;
uint32_t g_hostMillis = 1000;

static int g_failed = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failed++;
}

static void reset(long cardFree) {
    g_files.clear();
    g_cardFree = cardFree;
    g_syncFails = false;
    g_locked.clear();
    g_hostMillis = 1000;
}

static uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return ((uint32_t)d[at] << 24) | ((uint32_t)d[at + 1] << 16) | ((uint32_t)d[at + 2] << 8) | d[at + 3];
}

// Walks the single MTrk and counts its note events. False if the file
// isn't a whole format-0 SMF whose track length matches the file.
static bool parseMid(const std::vector<uint8_t>& d, int& notes, bool& hasEot) {
    notes = 0; hasEot = false;
    if (d.size() < 22 || memcmp(d.data(), "MThd", 4) != 0 || memcmp(d.data() + 14, "MTrk", 4) != 0) return false;
    uint32_t len = be32(d, 18);
    if (len != d.size() - 22) return false;
    size_t p = 22;
    while (p < d.size()) {
        while (d[p] & 0x80) p++; // delta
        p++;
        uint8_t st = d[p++];
        if (st == 0xFF) {
            uint8_t type = d[p++];
            uint8_t l = d[p++];
            if (type == 0x2F) hasEot = true;
            p += l;
        } else if ((st & 0xF0) == 0x90) {
            p += 2; notes++;
        } else {
            return false;
        }
        if (p > d.size()) return false;
    }
    return p == d.size();
}

static void feedNotes(MidiRecorder& r, int n) {
    for (int i = 0; i < n; i++) {
        g_hostMillis += 10;
        r.feed(0x90, 60, 100, 2);
    }
}

int main() {
    MidiRecorder rec;
    SysExRecorder syx;
    const char* mid = "/d/A.mid";
    const char* tmp = "/d/.A.mid.part";
    std::vector<uint8_t> old = {1, 2, 3};

    // Plain take.
    reset(1L << 30);
    check(rec.arm(mid), "arm");
    feedNotes(rec, 50);
    check(rec.stop() && rec.state() == MidiRecorder::STATE_IDLE, "stop saves");
    int notes; bool eot;
    check(parseMid(g_files[mid], notes, eot) && notes == 50 && eot, "plain take is a whole .mid");
    check(rec.arm("/d/B.mid") && rec.msSinceLastEvent() == UINT32_MAX, "arm resets the last event time");
    rec.cancel();

    // Card fills mid-take: keeps the whole events, track length matches.
    for (long room : {200L, 201L, 202L, 203L, 204L, 205L}) {
        reset(room);
        rec.arm(mid);
        feedNotes(rec, 100);
        rec.service();
        bool ok = rec.state() == MidiRecorder::STATE_ERROR && strstr(rec.errorMessage(), "kept") &&
                  parseMid(g_files[mid], notes, eot) && notes == (int)rec.eventCount() && notes > 0;
        char what[64];
        snprintf(what, sizeof(what), "card full at %ld bytes keeps %d events, eot %d", room, notes, (int)eot);
        check(ok, what);
        check(rec.stop() && rec.state() == MidiRecorder::STATE_IDLE, "stop after the error leaves");
    }

    // No room for the header: nothing left behind.
    reset(10);
    rec.arm(mid);
    feedNotes(rec, 5);
    rec.service();
    check(rec.state() == MidiRecorder::STATE_ERROR && strstr(rec.errorMessage(), "not saved") && !sdExists(mid),
          "card full before the header removes the file");

    // Failed sync.
    reset(1L << 30);
    rec.arm(mid);
    feedNotes(rec, 5);
    g_syncFails = true;
    g_hostMillis += 6000;
    rec.service();
    check(rec.state() == MidiRecorder::STATE_ERROR, "failed sync stops the take");

    // Sync waits for a gap in input, then runs.
    reset(1L << 30);
    rec.arm(mid);
    feedNotes(rec, 600); // 6 s of dense input
    g_syncFails = true;
    rec.service();
    check(rec.state() == MidiRecorder::STATE_RECORDING, "no sync while input is dense");
    g_hostMillis += 300;
    rec.service();
    check(rec.state() == MidiRecorder::STATE_ERROR, "sync once input goes quiet");

    // Replace, then discard: the old file stays, no temp left.
    reset(1L << 30);
    g_files[mid] = old;
    check(rec.arm(mid, true), "arm to replace");
    feedNotes(rec, 10);
    check(sdExists(tmp), "replacing take writes the temp file");
    rec.cancel();
    check(g_files[mid] == old && !sdExists(tmp), "discard keeps the old file");

    // Replace, then save.
    g_files[tmp] = old; // left by a power cut
    rec.arm(mid, true);
    check(!sdExists(tmp) && g_files["/d/REC001.mid"] == old, "arm moves a leftover temp file to REC001");
    feedNotes(rec, 10);
    check(rec.stop(), "stop replaces");
    check(parseMid(g_files[mid], notes, eot) && notes == 10 && !sdExists(tmp), "old file replaced by the take");

    // Old file can't be removed: the take goes to the next free name.
    reset(1L << 30);
    g_files[mid] = old;
    g_files["/d/REC001.mid"] = old;
    g_locked.insert(mid);
    rec.arm(mid, true);
    feedNotes(rec, 10);
    check(!rec.stop() && strstr(rec.errorMessage(), "REC002.mid") && g_files[mid] == old &&
              parseMid(g_files["/d/REC002.mid"], notes, eot) && notes == 10 && !sdExists(tmp),
          "locked old file: take saved as REC002.mid");
    printf("      (%s)\n", rec.errorMessage());

    // Write fails while replacing: old file stays, the part goes to a free name.
    reset(300);
    g_files[mid] = old;
    rec.arm(mid, true);
    feedNotes(rec, 100);
    rec.service();
    check(rec.state() == MidiRecorder::STATE_ERROR && strstr(rec.errorMessage(), "REC001.mid") && g_files[mid] == old &&
              parseMid(g_files["/d/REC001.mid"], notes, eot) && notes == (int)rec.eventCount() && !sdExists(tmp),
          "write fails while replacing: old kept, part saved as REC001.mid");
    printf("      (%s)\n", rec.errorMessage());

    // Final sync fails with no earlier error: the file stays.
    reset(1L << 30);
    rec.arm(mid);
    feedNotes(rec, 10);
    g_syncFails = true;
    check(!rec.stop() && rec.state() == MidiRecorder::STATE_ERROR && parseMid(g_files[mid], notes, eot) && notes == 10,
          "final sync fails: take kept");
    printf("      (%s)\n", rec.errorMessage());
    check(rec.stop() && rec.state() == MidiRecorder::STATE_IDLE && sdExists(mid), "leaving after it keeps the file");

    // Recovery of temp files.
    reset(1L << 30);
    g_files["/d/.B.mid.part"] = old;
    g_files[mid] = old;
    g_files[tmp] = {9, 9};
    g_files["/d/.C.syx.part"] = {};
    g_files["/d/.E.syx.part"] = old;
    g_files["/d/E.syx"] = old;
    bool r1 = recorderRecoverTemp("/d/.B.mid.part");
    bool r2 = recorderRecoverTemp(tmp);
    bool r3 = recorderRecoverTemp("/d/.C.syx.part");
    bool r4 = recorderRecoverTemp("/d/.E.syx.part");
    check(r1 && g_files["/d/B.mid"] == old && !sdExists("/d/.B.mid.part"), "recover: temp with a free name goes into place");
    check(r2 && g_files[mid] == old && g_files["/d/REC001.mid"] == std::vector<uint8_t>({9, 9}) && !sdExists(tmp),
          "recover: name taken, goes to REC001.mid");
    check(r3 && !sdExists("/d/.C.syx.part"), "recover: empty temp removed");
    check(r4 && g_files["/d/DUMP001.syx"] == old, "recover: .syx goes to DUMP001.syx");

    // Arm without replace on an existing file, and a path that won't fit.
    check(!rec.arm(mid) && rec.state() == MidiRecorder::STATE_ERROR, "existing file refused");
    std::string longPath = "/" + std::string(230, 'x') + ".mid";
    check(!rec.arm(longPath.c_str()) && strstr(rec.errorMessage(), "too long"), "long path refused");
    std::string fits = "/" + std::string(218, 'x') + ".mid"; // 223, fits; its temp name doesn't need to
    check(rec.arm(fits.c_str()), "223-char path accepted");
    rec.cancel();

    // SysEx: full card keeps whole messages.
    uint8_t msg[20];
    msg[0] = 0xF0; for (int i = 1; i < 19; i++) msg[i] = (uint8_t)i; msg[19] = 0xF7;
    const char* syxPath = "/d/D.syx";
    reset(150);
    syx.arm(syxPath);
    for (int i = 0; i < 20; i++) { g_hostMillis += 10; syx.feed(msg, sizeof(msg)); }
    syx.service();
    check(syx.state() == SysExRecorder::STATE_ERROR && g_files[syxPath].size() == 140 && syx.messageCount() == 7,
          "sysex full card keeps 7 whole messages");

    // SysEx replace, discard, then save.
    reset(1L << 30);
    g_files[syxPath] = old;
    syx.arm(syxPath, true);
    syx.feed(msg, sizeof(msg));
    syx.cancel();
    check(g_files[syxPath] == old && !sdExists("/d/.D.syx.part"), "sysex discard keeps the old file");
    syx.arm(syxPath, true);
    syx.feed(msg, sizeof(msg));
    check(syx.stop() && g_files[syxPath].size() == 20 && !sdExists("/d/.D.syx.part"), "sysex save replaces");

    // SysEx: locked old file, and a final sync failure.
    g_locked.insert(syxPath);
    syx.arm(syxPath, true);
    syx.feed(msg, sizeof(msg));
    check(!syx.stop() && strstr(syx.errorMessage(), "DUMP001.syx") && g_files["/d/DUMP001.syx"].size() == 20,
          "sysex locked old file: saved as DUMP001.syx");
    g_locked.clear();
    reset(1L << 30);
    syx.arm(syxPath);
    syx.feed(msg, sizeof(msg));
    g_syncFails = true;
    check(!syx.stop() && g_files[syxPath].size() == 20, "sysex final sync fails: dump kept");

    // SysEx: write fails while replacing.
    reset(150);
    g_files[syxPath] = old;
    syx.arm(syxPath, true);
    for (int i = 0; i < 20; i++) { g_hostMillis += 10; syx.feed(msg, sizeof(msg)); }
    syx.service();
    check(g_files[syxPath] == old && g_files["/d/DUMP001.syx"].size() == 20 * syx.messageCount() &&
              strstr(syx.errorMessage(), "DUMP001.syx"),
          "sysex write fails while replacing: old kept, part saved as DUMP001.syx");

    printf("%s\n", g_failed ? "rec-check FAILED" : "rec-check passed");
    return g_failed ? 1 : 0;
}

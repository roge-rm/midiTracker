// Checks MidiRecorder and SysExRecorder against an in-memory card
// (stub-rec/): a full card, a failed sync, replacing a file and discarding
// a replacing take. Prints one line per check and exits non-zero on a fail.
#include "midi_recorder.h"
#include "sysex_recorder.h"
#include "sysex_stream.h"
#include "sysex_player.h"
#include "midi_output.h"
#include "thru_hold.h"
#include "sd_card.h"
#include <string>

std::map<std::string, std::vector<uint8_t>> g_files;
long g_cardFree = 1L << 30;
bool g_syncFails = false;
bool g_readFails = false, g_seekFails = false;
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
    g_readFails = g_seekFails = false;
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

// Stub SysEx out for the player: collects what it sends, taking at most
// g_outRoom bytes per write.
static std::vector<uint8_t> g_out;
static bool g_outBusy = false;
static size_t g_outRoom = 32;
static int g_outBegins = 0, g_outEnds = 0;
namespace MidiOutput {
void sysExOutNewPlayback() {}
void sysExOutBegin() { g_outBusy = true; g_outBegins++; }
size_t sysExOutWrite(const uint8_t* d, size_t len) {
    if (!g_outBusy) return 0;
    size_t n = len < g_outRoom ? len : g_outRoom;
    g_out.insert(g_out.end(), d, d + n);
    return n;
}
void sysExOutEnd() { g_outBusy = false; g_outEnds++; }
bool sysExOutDrained() { return true; }
} // namespace MidiOutput

// Capture input: bytes through the real parser into g_syx, in batches of
// 64 with a flush after each, like MidiOutput::update().
static SysExRecorder* g_syx = nullptr;
static void streamHandler(SysExStreamEvent ev, uint8_t src, const uint8_t* d, size_t len) {
    switch (ev) {
        case SYSEX_BEGIN: g_syx->beginMessage(src); break;
        case SYSEX_DATA:  g_syx->writeMessage(src, d, len); break;
        case SYSEX_END:   g_syx->endMessage(src); break;
        case SYSEX_ABORT: g_syx->abortMessage(src); break;
    }
}
static void streamIn(SysExInParser& p, const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); i++) {
        p.byte(bytes[i], streamHandler);
        if (i % 64 == 63) p.flush(streamHandler);
    }
    p.flush(streamHandler);
}
static std::vector<uint8_t> sysexMsg(size_t len, uint8_t seed) {
    std::vector<uint8_t> m(len);
    m[0] = 0xF0;
    for (size_t i = 1; i + 1 < len; i++) m[i] = (uint8_t)((i * 7 + seed) & 0x7F);
    m[len - 1] = 0xF7;
    return m;
}
static void append(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); }

static void streamChecks() {
    SysExRecorder syx;
    g_syx = &syx;
    const char* path = "/d/S.syx";

    // 5 KB message with realtime bytes inside.
    reset(1L << 30);
    SysExInParser din(0), usb(1);
    std::vector<uint8_t> big = sysexMsg(5000, 3), in;
    for (size_t i = 0; i < big.size(); i++) {
        in.push_back(big[i]);
        if (i % 700 == 5) in.push_back(0xF8);
        if (i == 2500) in.push_back(0xFE);
    }
    syx.arm(path);
    streamIn(din, in);
    check(syx.stop() && g_files[path] == big && syx.messageCount() == 1 && syx.droppedCount() == 0,
          "capture: 5 KB message with realtime bytes inside kept whole, realtime left out");

    // Aborted by a note, by a new F0, then stopped mid-message.
    reset(1L << 30);
    std::vector<uint8_t> a = sysexMsg(20, 1), b = sysexMsg(30, 2), want;
    in.clear();
    append(in, a);
    std::vector<uint8_t> cut = sysexMsg(100, 4);
    cut.pop_back();
    append(in, cut);
    append(in, {0x90, 0x40, 0x40, 0x12}); // note on cuts it off, stray data after
    append(in, {0xF0, 0x01, 0x02});        // cut off by the next F0
    append(in, b);
    append(in, {0xF0, 0x05, 0x06});        // capture stops here
    want = a;
    append(want, b);
    syx.arm(path);
    streamIn(din, in);
    check(syx.stop() && g_files[path] == want && syx.messageCount() == 2 && syx.droppedCount() == 3,
          "capture: messages cut off by a note, an F0 and stopping are dropped, 3 counted");

    // Only a cut-off message: nothing saved.
    reset(1L << 30);
    syx.arm(path);
    streamIn(din, {0xF0, 1, 2, 3});
    check(!syx.stop() && !sdExists(path) && strstr(syx.errorMessage(), "not saved"),
          "capture: only a cut-off message saves nothing");
    syx.stop();

    // A message on USB while DIN is mid-message is dropped.
    reset(1L << 30);
    syx.arm(path);
    din.byte(0xF0, streamHandler);
    din.byte(0x11, streamHandler);
    din.flush(streamHandler);
    streamIn(usb, b);
    streamIn(din, {0x22, 0xF7});
    check(syx.stop() && g_files[path] == std::vector<uint8_t>({0xF0, 0x11, 0x22, 0xF7}) && syx.droppedCount() == 1,
          "capture: USB message during a DIN one dropped");

    // A message that stops coming is dropped after 1 s; the rest of it
    // and its F7 are ignored, the next message is kept.
    reset(1L << 30);
    SysExInParser din2(0);
    syx.arm(path);
    streamIn(din2, a);
    streamIn(din2, {0xF0, 1, 2, 3});
    g_hostMillis += 900;
    syx.service();
    bool stillOpen = syx.droppedCount() == 0;
    g_hostMillis += 200;
    syx.service();
    bool dropped = syx.droppedCount() == 1 && g_files[path] == a;
    streamIn(din2, {4, 5, 0xF7});
    streamIn(din2, b);
    want = a;
    append(want, b);
    check(stillOpen && dropped && syx.stop() && g_files[path] == want && syx.messageCount() == 2,
          "capture: message with no bytes for 1 s dropped, file cut back, next kept");

    // No sync while a message is open, even past the 15 s max; it syncs
    // once the message ends.
    reset(1L << 30);
    syx.arm(path);
    streamIn(din2, {0xF0});
    g_syncFails = true;
    bool synced = false;
    for (int i = 0; i < 200; i++) { // 20 s of a slow message
        g_hostMillis += 100;
        streamIn(din2, {0x10});
        syx.service();
        if (syx.state() != SysExRecorder::STATE_RECORDING) synced = true;
    }
    streamIn(din2, {0xF7});
    syx.service();
    check(!synced && syx.state() == SysExRecorder::STATE_ERROR,
          "capture: no sync mid-message, sync once it ends");
    syx.cancel();
}

static void playerChecks() {
    SysExPlayer player;
    const char* path = "/d/P.syx";
    reset(1L << 30);
    std::vector<uint8_t> a = sysexMsg(20, 1), big = sysexMsg(5000, 2), c = sysexMsg(40, 3);
    std::vector<uint8_t>& f = g_files[path];
    append(f, {0x01, 0x02});             // 2 junk
    append(f, a);
    append(f, {0x90, 0x40});             // 2 junk
    append(f, {0xF0, 0x11, 0x22});       // cut off by the next F0: skipped
    std::vector<uint8_t> bigIn = big;
    bigIn.insert(bigIn.begin() + 1000, 0xF8); // 1 junk, not sent
    append(f, bigIn);
    append(f, {0xF0, 0x33, 0x90, 0x44}); // cut off by a status byte: skipped, 2 junk
    append(f, c);
    append(f, {0xF0, 0x01, 0x02});       // cut off by the end of the file: skipped
    std::vector<uint8_t> want = a;
    append(want, big);
    append(want, c);

    g_out.clear();
    g_outBegins = g_outEnds = 0;
    g_outRoom = 32;
    check(player.load(path), "player: load");
    int updates = 0;
    size_t maxPerUpdate = 0;
    while (player.state() == SysExPlayer::STATE_SENDING && updates < 100000) {
        size_t before = g_out.size();
        player.update();
        if (g_out.size() - before > maxPerUpdate) maxPerUpdate = g_out.size() - before;
        g_hostMillis += 1;
        updates++;
    }
    check(player.state() == SysExPlayer::STATE_DONE && g_out == want, "player: sends exactly the 3 whole messages");
    check(player.messagesSent() == 3 && player.messagesSkipped() == 3 && player.junkBytes() == 7,
          "player: 3 sent, 3 skipped, 7 junk bytes");
    check(g_outBegins == 3 && g_outEnds == 3 && !g_outBusy, "player: each message begun and ended once");
    check(player.bytesSent() == player.totalBytes(), "player: progress reaches the end");
    printf("      (%d updates, at most %zu bytes per update)\n", updates, maxPerUpdate);

    // Closing mid-message ends it.
    g_out.clear();
    g_outRoom = 10;
    player.load(path);
    for (int i = 0; i < 1000 && g_out.size() < 1000; i++) {
        player.update();
        g_hostMillis += 1;
    }
    bool midway = g_outBusy; // in the 5 KB message
    player.close();
    check(midway && !g_outBusy && player.state() == SysExPlayer::STATE_IDLE, "player: close mid-message ends it");

    // Card read fails mid-message, and the seek back to send fails.
    g_outRoom = 32;
    player.load(path);
    for (int i = 0; i < 1000 && g_out.size() < 1000; i++) {
        player.update();
        g_hostMillis += 1;
    }
    g_readFails = true;
    for (int i = 0; i < 10; i++) player.update();
    check(player.state() == SysExPlayer::STATE_ERROR && strstr(player.errorMessage(), "read") && !g_outBusy,
          "player: read error mid-message is an error, message ended");
    g_readFails = false;
    player.close();
    player.load(path);
    g_seekFails = true;
    for (int i = 0; i < 10; i++) player.update();
    check(player.state() == SysExPlayer::STATE_ERROR && !g_outBusy, "player: failed seek is an error");
    g_seekFails = false;
    player.close();

    // A file with no SysEx at all.
    g_files[path] = {0x01, 0x02, 0x90, 0x40, 0x40};
    player.load(path);
    for (int i = 0; i < 10; i++) player.update();
    check(player.state() == SysExPlayer::STATE_ERROR, "player: no SysEx in the file is an error");
    player.close();
}

static void thruHoldChecks() {
    ThruHold h;
    for (int i = 0; i < 16; i++) h.push(0x90, (uint8_t)(60 + i), 100);
    for (int i = 0; i < 16; i++) h.push(0xB1, 1, (uint8_t)i);
    h.push(0x90, 90, 100); // full: new note-on dropped
    h.push(0x80, 60, 0);   // full: oldest note-on makes room
    bool ok = h.count() == 32 && h.front().data1 == 61 && h.panicChannels() == 0;
    check(ok, "thru hold: full drops note-ons first, keeps the note-off");

    ThruHold c;
    for (int i = 0; i < 32; i++) c.push(0xB2, 64, 127);
    c.push(0x83, 60, 0); // no note-on left to drop: channel 4 marked
    int order = 0;
    bool inOrder = true;
    while (c.count()) { inOrder = inOrder && c.front().status == 0xB2; c.pop(); order++; }
    check(inOrder && order == 32 && c.takePanicChannel() == 3 && c.empty(),
          "thru hold: overflow of note-offs marks the channel for the panic");
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

    streamChecks();
    playerChecks();
    thruHoldChecks();

    printf("%s\n", g_failed ? "rec-check FAILED" : "rec-check passed");
    return g_failed ? 1 : 0;
}

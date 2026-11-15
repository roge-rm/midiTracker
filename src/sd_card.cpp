#include "sd_card.h"
#include "pins.h"
#include <string.h>
#include <stdlib.h>

SdFs sd;

bool sdCardBegin() {
    // SdioConfig(clkPin, cmdPin, dat0Pin, clkDiv) -- DAT1..DAT3 must be
    // (and here are) the next three consecutive GPIOs after dat0Pin.
    //
    // clkDiv is a PIO state-machine divider applied to whatever the
    // CURRENT system clock is, not an absolute SD bus frequency -- the
    // driver does NOT self-adjust it for F_CPU (unlike the very first
    // init handshake, which does compute its own divider from
    // clock_get_hz(clk_sys)). Left at the default 1.0, the SD bus clock
    // scales directly with F_CPU. When F_CPU was raised from 133MHz to
    // 200MHz (see platformio.ini) to fix S3M/XM mixing-throughput
    // underruns, this silently pushed the SD bus ~50% faster too and
    // produced real-hardware symptoms consistent with SD bus errors: a
    // brief run of catastrophically slow reads (each _file.read() taking
    // 18-22ms instead of ~300us) a few seconds into an otherwise-clean
    // playback session, self-recovering after a few seconds -- a classic
    // CRC-error-retry signature, not a mixing-cost problem (see
    // S3mPlayer's own diagnostic instrumentation, which showed this same
    // window's cost concentrated entirely in chunkUs, not mixUs).
    // Scaling clkDiv by F_CPU/133000000 keeps the SD bus at the same
    // effective clock rate it ran at when 133MHz was already known
    // stable, while still letting everything else benefit from the
    // faster CPU.
    return sd.begin(SdioConfig(SDIO_CLK, SDIO_CMD, SDIO_D0, (float)F_CPU / 133000000.0f));
}

bool sdCardDetected() {
    return sd.card() && !sd.card()->errorCode();
}

// Opens each folder of `path` in turn, leaving the last open one in
// *parent and the final component in `leaf`. a and b are scratch handles.
static bool walkToParent(FsFile& a, FsFile& b, const char* path, FsFile*& parent, const char*& leaf) {
    if (!a.openRoot(&sd)) return false;
    FsFile* cur = &a;
    FsFile* nxt = &b;
    const char* p = path;
    while (*p == '/') p++;
    for (;;) {
        const char* slash = strchr(p, '/');
        if (!slash) break;
        char comp[80];
        size_t len = slash - p;
        if (len == 0) { p = slash + 1; continue; }
        if (len >= sizeof(comp)) return false;
        memcpy(comp, p, len);
        comp[len] = '\0';
        bool ok = (comp[0] == '*') ? nxt->open(cur, (uint32_t)strtoul(comp + 1, nullptr, 10), O_RDONLY)
                                   : nxt->open(cur, comp, O_RDONLY);
        if (!ok) return false;
        cur->close();
        if (!nxt->isDir()) { nxt->close(); return false; }
        FsFile* t = cur; cur = nxt; nxt = t;
        p = slash + 1;
    }
    parent = cur;
    leaf = p;
    return *leaf != '\0';
}

static bool openLeaf(FsFile& f, FsFile* parent, const char* leaf, oflag_t flags) {
    if (leaf[0] == '*') return f.open(parent, (uint32_t)strtoul(leaf + 1, nullptr, 10), flags);
    return f.open(parent, leaf, flags);
}

bool sdOpenPath(FsFile& f, const char* path, oflag_t flags) {
    if (!strchr(path, '*')) return f.open(path, flags);
    FsFile a, b;
    FsFile* parent;
    const char* leaf;
    if (!walkToParent(a, b, path, parent, leaf)) return false;
    return openLeaf(f, parent, leaf, flags);
}

bool sdExists(const char* path) {
    if (!strchr(path, '*')) return sd.exists(path);
    FsFile f;
    if (!sdOpenPath(f, path, O_RDONLY)) return false;
    f.close();
    return true;
}

bool sdRemove(const char* path) {
    if (!strchr(path, '*')) return sd.remove(path);
    FsFile f;
    return sdOpenPath(f, path, O_RDWR) && f.remove();
}

bool sdRmdir(const char* path) {
    if (!strchr(path, '*')) return sd.rmdir(path);
    FsFile f;
    return sdOpenPath(f, path, O_RDONLY) && f.rmdir();
}

bool sdMkdir(const char* path) {
    if (!strchr(path, '*')) return sd.mkdir(path);
    FsFile a, b;
    FsFile* parent;
    const char* leaf;
    if (!walkToParent(a, b, path, parent, leaf)) return false;
    FsFile d;
    if (!d.mkdir(parent, leaf, false)) return false;
    d.close();
    return true;
}

bool sdRename(const char* oldPath, const char* newPath) {
    if (!strchr(oldPath, '*') && !strchr(newPath, '*')) return sd.rename(oldPath, newPath);
    FsFile f;
    if (!sdOpenPath(f, oldPath, O_RDONLY)) return false;
    FsFile a, b;
    FsFile* parent;
    const char* leaf;
    if (!walkToParent(a, b, newPath, parent, leaf)) return false;
    return f.rename(parent, leaf);
}

// Stub implementations for the host render harness.
#include <Arduino.h>
#include <SdFat.h>
#include "synth.h"
#include <chrono>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "sd_card.h"

#ifndef HOST_MIDI // the MIDI target has its own clock and the real synth, see midi_stubs.cpp
HostSerial Serial;

static auto g_start = std::chrono::steady_clock::now();
uint32_t micros() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}
uint32_t millis() { return micros() / 1000; }
#endif

unsigned long FsFile::readCalls = 0;
unsigned long FsFile::readBytes = 0;

bool FsFile::open(const char* path, int flags) {
    close();
    if (flags & O_TRUNC) _f = fopen(path, "w+b");
    else if (flags & O_RDWR) _f = fopen(path, "r+b");
    else _f = fopen(path, "rb");
    if (!_f) return false;
    fseek(_f, 0, SEEK_END);
    _size = (uint64_t)ftell(_f);
    fseek(_f, 0, SEEK_SET);
    _pos = 0;
    return true;
}

namespace {
FsFile* g_rawFiles[16]; // contiguousRange() ids, sector >> 20 is the id + 1
}

bool FsFile::contiguousRange(uint32_t* bgnSector, uint32_t* endSector) {
    if (!_f) return false;
    int id = -1;
    for (int i = 0; i < 16; i++) if (g_rawFiles[i] == this) id = i;
    for (int i = 0; i < 16 && id < 0; i++) if (!g_rawFiles[i]) { g_rawFiles[i] = this; id = i; }
    if (id < 0) return false;
    fflush(_f);
    if (bgnSector) *bgnSector = (uint32_t)(id + 1) << 20;
    if (endSector) *endSector = ((uint32_t)(id + 1) << 20) + (uint32_t)(_size / 512);
    return true;
}

int FsFile::fd() const { return _f ? fileno(_f) : -1; }

bool SdCard::readSectors(uint32_t sector, uint8_t* dst, size_t ns) {
    int id = (int)(sector >> 20) - 1;
    if (id < 0 || id >= 16 || !g_rawFiles[id]) return false;
    off_t at = (off_t)(sector & 0xFFFFF) * 512;
    ssize_t got = pread(g_rawFiles[id]->fd(), dst, ns * 512, at);
    if (got < 0) return false;
    memset(dst + got, 0xA5, ns * 512 - (size_t)got); // past the end reads as junk, the player mustn't use it
    return true;
}

void FsFile::close() {
    for (int i = 0; i < 16; i++) if (g_rawFiles[i] == this) g_rawFiles[i] = nullptr;
    if (_f) fclose(_f);
    _f = nullptr;
    _pos = 0;
    _size = 0;
}

int FsFile::read(void* buf, size_t n) {
    if (!_f) return -1;
    readCalls++;
    if (fseek(_f, (long)_pos, SEEK_SET) != 0) return -1;
    size_t got = fread(buf, 1, n, _f);
    _pos += got;
    readBytes += got;
    return (int)got;
}

long FsFile::writeBudget = -1;

size_t FsFile::write(const void* buf, size_t n) {
    if (!_f || fseek(_f, (long)_pos, SEEK_SET) != 0) return 0;
    size_t want = n;
    if (writeBudget >= 0 && _pos + n > _size) {
        // Only bytes past the end take space.
        uint64_t grow = _pos + n - _size;
        if ((long)grow > writeBudget) want = n - (size_t)(grow - (uint64_t)writeBudget);
    }
    size_t put = fwrite(buf, 1, want, _f);
    if (writeBudget >= 0 && _pos + put > _size) writeBudget -= (long)(_pos + put - _size);
    _pos += put;
    if (_pos > _size) _size = _pos;
    return put;
}

SdFs sd;
bool SdFs::exists(const char* path) { struct stat st; return stat(path, &st) == 0; }
bool SdFs::mkdir(const char* path) { return ::mkdir(path, 0755) == 0; }
bool SdFs::remove(const char* path) { return ::remove(path) == 0; }

bool FsFile::seekSet(uint64_t pos) {
    if (!_f || pos > _size) return false;
    _pos = pos;
    return true;
}

#ifndef HOST_MIDI
namespace {
const size_t RING_FRAMES = 8192; // same as the firmware's WAV ring
std::vector<int16_t> g_out;
size_t g_pending = 0; // frames written since the last drain
bool g_active = false, g_ended = false;
}

namespace Synth {
void wavStreamReset() {
    g_out.resize(g_out.size() - g_pending * 2);
    g_pending = 0;
    g_active = g_ended = false;
}
void wavStreamSetActive(bool a) { g_active = a; }
size_t wavStreamWrite(const int16_t* frames, size_t count) {
    size_t room = RING_FRAMES - g_pending;
    if (count > room) count = room;
    g_out.insert(g_out.end(), frames, frames + count * 2);
    g_pending += count;
    return count;
}
size_t wavStreamFree() { return RING_FRAMES - g_pending; }
void wavStreamEnd() { g_ended = true; }
// The harness's consumer never stops on its own, so an active stream stays active.
bool wavStreamIsActive() { return g_active; }
bool wavStreamIsEnded() { return g_ended; }
bool wavStreamTookUnderrun() { return false; }
uint32_t wavStreamUnderrunSamples() { return 0; }
uint32_t wavStreamWritten() { return (uint32_t)(g_out.size() / 2); }
uint32_t wavStreamPlayed() { return (uint32_t)(g_out.size() / 2 - g_pending); }
// The frames written since the last drain, as if none had played yet.
size_t wavStreamPeek(int16_t* out, size_t frames) {
    size_t n = frames < g_pending ? frames : g_pending;
    memcpy(out, g_out.data() + (g_out.size() - g_pending * 2), n * 4);
    return n;
}
}

namespace HostRing {
void drain() { g_pending = 0; }
size_t framesCaptured() { return g_out.size() / 2; }
const int16_t* data() { return g_out.data(); }
}
#endif

bool sdOpenPath(FsFile& f, const char* path, int flags) { return f.open(path, flags); }
bool sdExists(const char* path) { struct stat st; return stat(path, &st) == 0; }
bool sdRemove(const char* path) { return ::remove(path) == 0; }
bool sdRename(const char* a, const char* b) { return !sdExists(b) && ::rename(a, b) == 0; }

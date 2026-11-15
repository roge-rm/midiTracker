// Stub implementations for the host render harness.
#include <Arduino.h>
#include <SdFat.h>
#include "synth.h"
#include <chrono>
#include <vector>
#include <sys/stat.h>
#include "sd_card.h"

HostSerial Serial;

static auto g_start = std::chrono::steady_clock::now();
uint32_t micros() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}
uint32_t millis() { return micros() / 1000; }

unsigned long FsFile::readCalls = 0;

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

void FsFile::close() {
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
    return (int)got;
}

size_t FsFile::write(const void* buf, size_t n) {
    if (!_f || fseek(_f, (long)_pos, SEEK_SET) != 0) return 0;
    size_t put = fwrite(buf, 1, n, _f);
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

namespace {
const size_t RING_FRAMES = 8192; // same as the firmware's WAV ring
std::vector<int16_t> g_out;
size_t g_pending = 0; // frames written since the last drain
}

namespace Synth {
void wavStreamReset() {
    g_out.resize(g_out.size() - g_pending * 2);
    g_pending = 0;
}
void wavStreamSetActive(bool) {}
size_t wavStreamWrite(const int16_t* frames, size_t count) {
    size_t room = RING_FRAMES - g_pending;
    if (count > room) count = room;
    g_out.insert(g_out.end(), frames, frames + count * 2);
    g_pending += count;
    return count;
}
size_t wavStreamFree() { return RING_FRAMES - g_pending; }
void wavStreamEnd() {}
bool wavStreamTookUnderrun() { return false; }
uint32_t wavStreamUnderrunSamples() { return 0; }
}

namespace HostRing {
void drain() { g_pending = 0; }
size_t framesCaptured() { return g_out.size() / 2; }
const int16_t* data() { return g_out.data(); }
}

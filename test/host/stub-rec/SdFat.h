// In-memory card for the recorder check: files live in g_files, and
// g_cardFree caps how many bytes writes can still add. A write that
// doesn't fit copies what it can and fails, like SdFat on a full card.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <map>
#include <set>
#include <string>
#include <vector>

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
typedef int oflag_t;

extern std::map<std::string, std::vector<uint8_t>> g_files;
extern long g_cardFree;
extern bool g_syncFails;
extern std::set<std::string> g_locked; // files that can't be removed
extern bool g_readFails, g_seekFails;

class FsFile {
public:
    bool open(const char* path, int flags) {
        auto it = g_files.find(path);
        if (it == g_files.end()) {
            if (!(flags & O_CREAT)) return false;
            g_files[path];
        } else if (flags & O_TRUNC) {
            g_cardFree += (long)it->second.size();
            it->second.clear();
        }
        _name = path; _open = true; _pos = 0; _err = false;
        return true;
    }
    void close() { _open = false; }
    bool isOpen() const { return _open; }
    explicit operator bool() const { return _open; }
    size_t write(const void* buf, size_t n) {
        if (!_open) return (size_t)-1;
        std::vector<uint8_t>& d = g_files[_name];
        const uint8_t* b = (const uint8_t*)buf;
        for (size_t i = 0; i < n; i++, _pos++) {
            if (_pos >= d.size()) {
                if (g_cardFree <= 0) { _err = true; return (size_t)-1; }
                d.push_back(0); g_cardFree--;
            }
            d[_pos] = b[i];
        }
        return n;
    }
    int read(void* buf, size_t n) {
        if (g_readFails) return -1;
        std::vector<uint8_t>& d = g_files[_name];
        size_t k = 0;
        for (; k < n && _pos < d.size(); k++) ((uint8_t*)buf)[k] = d[_pos++];
        return (int)k;
    }
    int read() { uint8_t b; return read(&b, 1) == 1 ? b : -1; }
    bool seekSet(uint64_t p) { if (g_seekFails || p > g_files[_name].size()) return false; _pos = p; return true; }
    uint64_t curPosition() const { return _pos; }
    uint64_t fileSize() const { return g_files[_name].size(); }
    bool sync() { return _open && !g_syncFails; }
    bool truncate(uint64_t len) {
        std::vector<uint8_t>& d = g_files[_name];
        if (len > d.size()) return false;
        g_cardFree += (long)(d.size() - len);
        d.resize(len); _pos = len;
        return true;
    }
    bool getWriteError() const { return _err; }
    void clearWriteError() { _err = false; }

private:
    std::string _name;
    bool _open = false;
    uint64_t _pos = 0;
    bool _err = false;
};

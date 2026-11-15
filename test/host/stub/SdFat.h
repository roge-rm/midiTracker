// Host stand-in for SdFat's FsFile, over stdio. Seeks past the end fail
// and leave the position alone, like SdFat does for a file opened to read.
#pragma once
#include <stdint.h>
#include <stdio.h>

#define O_RDONLY 0

class FsFile {
public:
    FsFile() {}
    FsFile(const FsFile&) = delete;
    FsFile& operator=(const FsFile&) = delete;
    ~FsFile() { close(); }

    bool open(const char* path, int flags = O_RDONLY);
    void close();
    explicit operator bool() const { return _f != nullptr; }
    bool isOpen() const { return _f != nullptr; }
    int read(void* buf, size_t n);
    bool seekSet(uint64_t pos);
    bool seekCur(int64_t off) { return seekSet((uint64_t)((int64_t)_pos + off)); }
    uint64_t curPosition() const { return _pos; }
    uint64_t fileSize() const { return _size; }

    // Count of read() calls that had to hit the backing file, for the harness.
    static unsigned long readCalls;

private:
    FILE* _f = nullptr;
    uint64_t _pos = 0;
    uint64_t _size = 0;
};

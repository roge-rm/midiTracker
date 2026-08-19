#pragma once
#include <Arduino.h>
#include "config.h"

struct BrowserEntry {
    char name[MAX_FILENAME_LEN];
    bool isDir;
    bool isSysEx; // false (and meaningless) for directories -- a .syx dump vs. a .mid file, see isSysExFilename()
    uint32_t size;
};

// Lists and navigates directories on the SD card, filtering to
// directories and *.mid/*.MID/*.syx/*.SYX files. Call begin() once after
// sdCardBegin() succeeds.
class SdBrowser {
public:
    bool begin(const char* rootPath = BROWSE_ROOT);

    int entryCount() const { return _count; }
    const BrowserEntry& entry(int idx) const { return _entries[idx]; }
    const char* currentPath() const { return _path; }

    // Re-reads the current directory's contents (e.g. after SD insert).
    bool refresh();

    // Descends into entry idx if it's a directory. Returns false if it's
    // a file or on error.
    bool enterDir(int idx);

    // Moves to the parent directory. Returns false if already at root.
    bool goUp();

    // Builds the absolute path for entry idx into outPath (size outSize).
    void buildFullPath(int idx, char* outPath, size_t outSize) const;

    // Joins `name` onto the current directory, the same way buildFullPath
    // does for an existing entry -- for a file that doesn't exist yet
    // (e.g. a new recording).
    void buildPath(const char* name, char* outPath, size_t outSize) const;

private:
    static const int MAX_DIR_ENTRIES = 96; // cap on entries held per directory

    char _path[192] = BROWSE_ROOT;
    BrowserEntry _entries[MAX_DIR_ENTRIES];
    int _count = 0;

    void loadEntries();
    static bool isMidiFilename(const char* name);
    static bool isSysExFilename(const char* name);
};

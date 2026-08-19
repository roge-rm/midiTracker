#include "sd_browser.h"
#include "sd_card.h"
#include <string.h>
#include <strings.h> // strcasecmp

bool SdBrowser::isMidiFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + (len - 4);
    return strcasecmp(ext, ".mid") == 0;
}

bool SdBrowser::isSysExFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + (len - 4);
    return strcasecmp(ext, ".syx") == 0;
}

bool SdBrowser::begin(const char* rootPath) {
    strncpy(_path, rootPath, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    loadEntries();
    return true;
}

bool SdBrowser::refresh() {
    loadEntries();
    return true;
}

void SdBrowser::loadEntries() {
    _count = 0;

    FsFile dir = sd.open(_path);
    if (!dir || !dir.isDir()) {
        if (dir) dir.close();
        return;
    }

    FsFile file;
    char nameBuf[MAX_FILENAME_LEN];
    while (_count < MAX_DIR_ENTRIES && file.openNext(&dir, O_RDONLY)) {
        file.getName(nameBuf, sizeof(nameBuf));

        // Skip hidden/system entries.
        if (nameBuf[0] != '.' && !file.isHidden()) {
            bool isDir = file.isDir();
            bool isSysEx = !isDir && isSysExFilename(nameBuf);
            if (isDir || isMidiFilename(nameBuf) || isSysEx) {
                BrowserEntry& e = _entries[_count];
                strncpy(e.name, nameBuf, sizeof(e.name) - 1);
                e.name[sizeof(e.name) - 1] = '\0';
                e.isDir = isDir;
                e.isSysEx = isSysEx;
                e.size = isDir ? 0 : (uint32_t)file.fileSize();
                _count++;
            }
        }
        file.close();
    }
    dir.close();

    // Simple insertion sort: directories first, then alphabetical
    // (case-insensitive) within each group. Directory listings here are
    // small enough (MAX_DIR_ENTRIES) that O(n^2) is not a concern.
    for (int i = 1; i < _count; i++) {
        BrowserEntry key = _entries[i];
        int j = i - 1;
        while (j >= 0) {
            bool keyFirst;
            if (_entries[j].isDir != key.isDir) {
                keyFirst = key.isDir; // dirs sort before files
            } else {
                keyFirst = strcasecmp(key.name, _entries[j].name) < 0;
            }
            if (!keyFirst) break;
            _entries[j + 1] = _entries[j];
            j--;
        }
        _entries[j + 1] = key;
    }
}

void SdBrowser::buildFullPath(int idx, char* outPath, size_t outSize) const {
    if (idx < 0 || idx >= _count) {
        outPath[0] = '\0';
        return;
    }
    buildPath(_entries[idx].name, outPath, outSize);
}

void SdBrowser::buildPath(const char* name, char* outPath, size_t outSize) const {
    bool rootIsSlash = (strcmp(_path, "/") == 0);
    if (rootIsSlash) {
        snprintf(outPath, outSize, "/%s", name);
    } else {
        snprintf(outPath, outSize, "%s/%s", _path, name);
    }
}

bool SdBrowser::enterDir(int idx) {
    if (idx < 0 || idx >= _count || !_entries[idx].isDir) return false;

    char newPath[192];
    buildFullPath(idx, newPath, sizeof(newPath));
    strncpy(_path, newPath, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    loadEntries();
    return true;
}

bool SdBrowser::goUp() {
    if (strcmp(_path, "/") == 0) return false;

    char* lastSlash = strrchr(_path, '/');
    if (lastSlash == _path) {
        _path[1] = '\0'; // parent is root
    } else if (lastSlash != nullptr) {
        *lastSlash = '\0';
    } else {
        return false;
    }
    loadEntries();
    return true;
}

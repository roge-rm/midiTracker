#include "sd_browser.h"
#include "sd_card.h"
#include "convert.h" // isConvertTempName
#include <string.h>
#include <strings.h> // strcasecmp
#include <ctype.h>   // toupper
#include <stdlib.h>  // atoi, strtoul
#include <new>       // std::nothrow

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

bool SdBrowser::isWavFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + (len - 4);
    return strcasecmp(ext, ".wav") == 0;
}

bool SdBrowser::isMp3Filename(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + (len - 4);
    return strcasecmp(ext, ".mp3") == 0;
}

bool SdBrowser::isAiffFilename(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".aif") == 0 || strcasecmp(dot, ".aiff") == 0 || strcasecmp(dot, ".aifc") == 0;
}

bool SdBrowser::isFlacFilename(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".flac") == 0;
}

bool SdBrowser::isModFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + (len - 4);
    return strcasecmp(ext, ".mod") == 0;
}

bool SdBrowser::isS3mFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + (len - 4);
    return strcasecmp(ext, ".s3m") == 0;
}

bool SdBrowser::isXmFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 3) return false;
    const char* ext = name + (len - 3);
    return strcasecmp(ext, ".xm") == 0;
}

bool SdBrowser::isItFilename(const char* name) {
    size_t len = strlen(name);
    if (len < 3) return false;
    const char* ext = name + (len - 3);
    return strcasecmp(ext, ".it") == 0;
}

// Longest name (UTF-8 bytes, with the zero) either FAT or exFAT can hold.
static const size_t NAME_FETCH_LEN = 768;

// Replaces each non-ASCII character with one '?', in place.
void SdBrowser::foldToAscii(char* s) {
    char* w = s;
    for (const char* r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x80) *w++ = (char)c;
        else if (c >= 0xC0) *w++ = '?'; // lead byte; its continuation bytes (0x80-0xBF) are skipped
    }
    *w = '\0';
}

// Folds s to ASCII and copies it into out, cutting the middle of an
// over-long name so a short extension stays at the end.
void SdBrowser::shortenForDisplay(char* s, char* out, size_t outSize) {
    foldToAscii(s);
    size_t len = strlen(s);
    if (len < outSize) {
        memcpy(out, s, len + 1);
        return;
    }
    const char* dot = strrchr(s, '.');
    size_t extLen = (dot && (size_t)(s + len - dot) <= 5) ? (size_t)(s + len - dot) : 0;
    size_t head = outSize - 1 - extLen;
    memcpy(out, s, head);
    memcpy(out + head, s + len - extLen, extLen);
    out[head + extLen] = '\0';
}

// Display name of an open entry, whatever its length.
void SdBrowser::displayNameOf(FsFile& f, char* out, size_t outSize) {
    if (f.getName(out, outSize) > 0) {
        foldToAscii(out);
        return;
    }
    char* big = new (std::nothrow) char[NAME_FETCH_LEN];
    if (big && f.getName(big, NAME_FETCH_LEN) > 0) {
        shortenForDisplay(big, out, outSize);
    } else {
        strncpy(out, "?", outSize);
        out[outSize - 1] = '\0';
    }
    delete[] big;
}

// Shared by both loadEntries() scan passes so they agree on exactly which
// entries qualify -- skips hidden/dotfiles, accepts any directory, and
// accepts a file only if its extension is one of the eight recognized
// kinds. `file` must already be freshly opened via openNext() (or
// otherwise positioned) so isHidden()/isDir() are meaningful.
bool SdBrowser::classifyEntry(FsFile& file, const char* name, bool& isDirOut, uint8_t& kindOut) {
    if (name[0] == '.' || file.isHidden()) return false;
    isDirOut = file.isDir();
    if (isDirOut) {
        kindOut = FILE_MID; // meaningless for a directory, never read
        return true;
    }
    if (isSysExFilename(name)) { kindOut = FILE_SYX; return true; }
    if (isWavFilename(name))   { kindOut = FILE_WAV; return true; }
    if (isMp3Filename(name))   { kindOut = FILE_MP3; return true; }
    if (isAiffFilename(name))  { kindOut = FILE_AIFF; return true; }
    if (isFlacFilename(name))  { kindOut = FILE_FLAC; return true; }
    if (isModFilename(name))   { kindOut = FILE_MOD; return true; }
    if (isS3mFilename(name))   { kindOut = FILE_S3M; return true; }
    if (isXmFilename(name))    { kindOut = FILE_XM;  return true; }
    if (isItFilename(name))    { kindOut = FILE_IT;  return true; }
    if (isMidiFilename(name))  { kindOut = FILE_MID; return true; }
    return false;
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

void SdBrowser::freeIndexAndSortKeys() {
    delete[] _index;
    _index = nullptr;
    delete[] _sortKeys;
    _sortKeys = nullptr;
    _count = 0;
}

void SdBrowser::freeBuffers() {
    freeIndexAndSortKeys();
    _hitScanCap = false;
    _scannedTotal = 0;
    _buffersFreed = true;
    for (int i = 0; i < WINDOW_SIZE; i++) _windowLogicalIndex[i] = -1;
    _windowNextSlot = 0;
}

void SdBrowser::resetToRoot() {
    freeBuffers();
    strncpy(_path, BROWSE_ROOT, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
}

// Two passes: the first just counts how many entries actually qualify
// (cheap -- no storage, same openNext() walk as before), so the second
// pass can ask for a heap buffer sized to what THIS folder really has
// instead of always reserving the worst case. See this class's header
// comment and the SdBrowser sizing plan for why a fixed eager reservation
// doesn't work here (LooperMode::tracks[], about 98KB, can already be
// resident, and isn't visible in the linker's static RAM report).
void SdBrowser::loadEntries() {
    freeIndexAndSortKeys();
    _hitScanCap = false;
    _scannedTotal = 0;
    _buffersFreed = false; // a real scan just ran -- no longer "silently emptied", see needsRebuild()
    for (int i = 0; i < WINDOW_SIZE; i++) _windowLogicalIndex[i] = -1;
    _windowNextSlot = 0;

    _maxRec = 0;
    _maxDump = 0;
    _sawTemp = false;

    FsFile dir;
    sdOpenPath(dir, _path, O_RDONLY);
    if (!dir || !dir.isDir()) {
        if (dir) dir.close();
        return;
    }

    // Full names are read into a transient buffer so extensions and the
    // REC/DUMP numbers are seen whatever the name length.
    // Falls back to a short stack buffer if the heap is short.
    char smallBuf[MAX_FILENAME_LEN];
    char* heapBuf = new (std::nothrow) char[NAME_FETCH_LEN];
    char* nameBuf = heapBuf ? heapBuf : smallBuf;
    const size_t nameCap = heapBuf ? NAME_FETCH_LEN : sizeof(smallBuf);

    int total = 0;
    {
        FsFile file;
        while (total < MAX_INDEX_ENTRIES && file.openNext(&dir, O_RDONLY)) {
            bool isDirEntry;
            uint8_t kind;
            if (file.getName(nameBuf, nameCap) == 0) nameBuf[0] = '\0';
            size_t nameLen = strlen(nameBuf);
            if (nameBuf[0] == '.' && nameLen > 5 && !file.isDir() &&
                ((nameLen > 6 && strcmp(nameBuf + nameLen - 5, ".part") == 0) || isConvertTempName(nameBuf)))
                _sawTemp = true;
            if (classifyEntry(file, nameBuf, isDirEntry, kind)) {
                total++;
                if (!isDirEntry) {
                    if (strncasecmp(nameBuf, "REC", 3) == 0) {
                        int num = atoi(nameBuf + 3);
                        if (num > _maxRec) _maxRec = num;
                    } else if (kind == FILE_SYX && strncasecmp(nameBuf, "DUMP", 4) == 0) {
                        int num = atoi(nameBuf + 4);
                        if (num > _maxDump) _maxDump = num;
                    }
                }
            }
            file.close();
        }
    }
    _hitScanCap = (total >= MAX_INDEX_ENTRIES);
    _scannedTotal = total;

    if (total == 0) {
        delete[] heapBuf;
        dir.close();
        return;
    }
    dir.rewind();

    // Headroom check: cap the allocation to what's actually safe to take
    // right now, not just what the folder has -- rp2040.getFreeHeap() is
    // a real runtime measurement, unlike the linker's static RAM%, which
    // says nothing about tracks[] or any other heap allocation already
    // made. HEAP_SAFETY_MARGIN is tuned against a real-hardware measurement
    // (see this class's header comment).
    bool wantSortKeys = (total <= SORT_KEY_CEILING);
    size_t perEntryBytes = sizeof(DirIndexEntry) + (wantSortKeys ? sizeof(SortKeyEntry) : 0);
    uint32_t freeHeap = rp2040.getFreeHeap();
    uint32_t available = (freeHeap > HEAP_SAFETY_MARGIN) ? (freeHeap - HEAP_SAFETY_MARGIN) : 0;
    int affordable = (int)(available / perEntryBytes);

    int allocCount = total < affordable ? total : affordable;
    if (allocCount < 0) allocCount = 0;
    // A headroom-limited count that no longer covers the whole folder
    // isn't a meaningful base for a full sort -- drop back to index-only
    // rather than alphabetizing part of the list (see the class comment's
    // "keep it binary" reasoning, same idea as the SORT_KEY_CEILING case).
    if (allocCount < total) wantSortKeys = false;

    if (allocCount == 0) {
        delete[] heapBuf;
        dir.close();
        return; // out of headroom -- browse as empty rather than crash; truncationNote() still reports what was scanned
    }

    _index = new (std::nothrow) DirIndexEntry[allocCount];
    if (!_index) {
        delete[] heapBuf;
        dir.close();
        return;
    }
    if (wantSortKeys) {
        _sortKeys = new (std::nothrow) SortKeyEntry[allocCount];
        // Not fatal if this one fails -- just skip sorting below.
    }

    FsFile file;
    int filled = 0;
    while (filled < allocCount && file.openNext(&dir, O_RDONLY)) {
        bool isDirEntry;
        uint8_t kind;
        size_t nameLen = file.getName(nameBuf, nameCap);
        if (nameLen == 0) nameBuf[0] = '\0';
        if (classifyEntry(file, nameBuf, isDirEntry, kind)) {
            DirIndexEntry& e = _index[filled];
            e.dirIndex = file.dirIndex();
            e.isDir = isDirEntry ? 1 : 0;
            e.kind = kind;
            e.byIndex = (nameLen == 0 || nameLen >= MAX_FILENAME_LEN) ? 1 : 0;
            for (const char* c = nameBuf; !e.byIndex && *c; c++) {
                if ((unsigned char)*c >= 0x80) e.byIndex = 1;
            }
            if (_sortKeys) {
                char key[SORT_KEY_LEN];
                size_t n = strlen(nameBuf);
                if (n > SORT_KEY_LEN) n = SORT_KEY_LEN;
                size_t i = 0;
                for (; i < n; i++) key[i] = (char)toupper((unsigned char)nameBuf[i]);
                for (; i < SORT_KEY_LEN; i++) key[i] = '\0';
                memcpy(_sortKeys[filled].key, key, SORT_KEY_LEN);
            }
            filled++;
        }
        file.close();
    }
    dir.close();
    delete[] heapBuf;

    _count = filled;
    if (_sortKeys) sortIndex();
}

// Simple insertion sort over the compact index (+ sort key, moved in
// lockstep) -- directories first, then alphabetical by the cached
// uppercased prefix. Same "small enough that O(n^2) is fine" reasoning
// the old eager version used, just bounded by SORT_KEY_CEILING now
// instead of the old MAX_DIR_ENTRIES. Only ever called when _sortKeys is
// non-null (see loadEntries()).
void SdBrowser::sortIndex() {
    for (int i = 1; i < _count; i++) {
        DirIndexEntry keyIdx = _index[i];
        SortKeyEntry keySort = _sortKeys[i];
        int j = i - 1;
        while (j >= 0) {
            bool keyFirst;
            if (_index[j].isDir != keyIdx.isDir) {
                keyFirst = keyIdx.isDir != 0; // dirs sort before files
            } else {
                keyFirst = memcmp(keySort.key, _sortKeys[j].key, SORT_KEY_LEN) < 0;
            }
            if (!keyFirst) break;
            _index[j + 1] = _index[j];
            _sortKeys[j + 1] = _sortKeys[j];
            j--;
        }
        _index[j + 1] = keyIdx;
        _sortKeys[j + 1] = keySort;
    }
}

// Materializes entry idx's full name/size into the on-demand window,
// re-opening it directly by its cached dirIndex() (~O(1), see this
// class's header comment) rather than re-scanning the directory from the
// top. Logically const (called from entry(), a const method) -- only
// mutates the `mutable` window cache, not anything that changes what the
// browser conceptually holds.
const BrowserEntry& SdBrowser::materialize(int idx) const {
    static const BrowserEntry emptyFallback = {};
    if (idx < 0 || idx >= _count) return emptyFallback;

    for (int i = 0; i < WINDOW_SIZE; i++) {
        if (_windowLogicalIndex[i] == idx) return _window[i];
    }

    int slot = _windowNextSlot;
    _windowNextSlot = (_windowNextSlot + 1) % WINDOW_SIZE;

    const DirIndexEntry& idxEntry = _index[idx];
    BrowserEntry& e = _window[slot];
    e.isDir = idxEntry.isDir != 0;
    e.kind = (FileKind)idxEntry.kind;
    e.size = 0;
    e.name[0] = '\0';

    FsFile dirFile;
    if (sdOpenPath(dirFile, _path, O_RDONLY)) {
        FsFile file;
        if (file.open(&dirFile, idxEntry.dirIndex, O_RDONLY)) {
            displayNameOf(file, e.name, sizeof(e.name));
            e.size = e.isDir ? 0 : (uint32_t)file.fileSize();
            file.close();
        }
        dirFile.close();
    }

    _windowLogicalIndex[slot] = idx;
    return e;
}

const BrowserEntry& SdBrowser::entry(int idx) const {
    return materialize(idx);
}

void SdBrowser::truncationNote(char* outBuf, size_t outSize) const {
    if (_hitScanCap) {
        snprintf(outBuf, outSize, " (%d+)", MAX_INDEX_ENTRIES);
    } else if (_scannedTotal > _count) {
        snprintf(outBuf, outSize, " (+%d)", _scannedTotal - _count);
    } else {
        outBuf[0] = '\0';
    }
}

void SdBrowser::buildFullPath(int idx, char* outPath, size_t outSize) const {
    if (idx < 0 || idx >= _count) {
        outPath[0] = '\0';
        return;
    }
    if (_index[idx].byIndex) {
        char token[16];
        snprintf(token, sizeof(token), "*%lu", (unsigned long)_index[idx].dirIndex);
        buildPath(token, outPath, outSize);
        return;
    }
    buildPath(entry(idx).name, outPath, outSize);
}

void SdBrowser::displayPath(char* outBuf, size_t outSize) const {
    if (!strchr(_path, '*')) {
        snprintf(outBuf, outSize, "%s", _path);
        return;
    }
    FsFile a, b;
    FsFile* cur = &a;
    FsFile* nxt = &b;
    bool open = a.openRoot(&sd);
    size_t n = 0;
    outBuf[0] = '\0';
    const char* p = _path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char comp[MAX_FILENAME_LEN + 16];
        if (len >= sizeof(comp)) len = sizeof(comp) - 1;
        memcpy(comp, p, len);
        comp[len] = '\0';
        if (comp[0] == '*') {
            bool got = open && nxt->open(cur, (uint32_t)strtoul(comp + 1, nullptr, 10), O_RDONLY);
            if (got) displayNameOf(*nxt, comp, sizeof(comp)); else strcpy(comp, "?");
            if (got) {
                cur->close();
                FsFile* t = cur; cur = nxt; nxt = t;
            } else {
                open = false;
            }
        } else if (open) {
            if (nxt->open(cur, comp, O_RDONLY)) {
                cur->close();
                FsFile* t = cur; cur = nxt; nxt = t;
            } else {
                open = false;
            }
        }
        if (n + 1 < outSize) n += snprintf(outBuf + n, outSize - n, "/%s", comp);
        p += len;
    }
    if (n == 0) snprintf(outBuf, outSize, "/");
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
    if (idx < 0 || idx >= _count || !_index[idx].isDir) return false;

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

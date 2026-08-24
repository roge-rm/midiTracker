#pragma once
#include <Arduino.h>
#include <SdFat.h> // FsFile, used by classifyEntry()'s private signature below
#include "config.h"

// Meaningless for directories. FILE_MID/FILE_SYX/FILE_WAV/FILE_MOD/
// FILE_S3M/FILE_XM, see isMidiFilename()/isSysExFilename()/isWavFilename()/
// isModFilename()/isS3mFilename()/isXmFilename().
enum FileKind { FILE_MID, FILE_SYX, FILE_WAV, FILE_MOD, FILE_S3M, FILE_XM };

struct BrowserEntry {
    char name[MAX_FILENAME_LEN];
    bool isDir;
    FileKind kind;
    uint32_t size;
};

// Lists and navigates directories on the SD card, filtering to
// directories and *.mid/*.MID/*.syx/*.SYX/*.wav/*.WAV/*.mod/*.MOD/*.s3m/
// *.S3M/*.xm/*.XM files. Call begin() once after sdCardBegin() succeeds.
//
// A folder can hold far more entries than comfortably fits in RAM as full
// BrowserEntry records (~76 bytes each), so this does NOT eagerly
// materialize the whole directory. Instead, loadEntries() builds a
// compact per-entry index (~8 bytes each -- just SdFat's own dirIndex(),
// which lets any specific entry be re-opened later in ~O(1) via
// FsFile::open(dir, index, oflag) rather than re-scanning from the top,
// see FatFile.cpp/ExFatFile.cpp's own open(dirFile, index, oflag)) sized
// to the folder's *actual* content (heap-allocated, not a fixed
// reservation), capped at MAX_INDEX_ENTRIES and by real available heap
// (see loadEntries()). entry() materializes full BrowserEntry data lazily
// into a small fixed window, on demand -- the public API below is
// unchanged from the old eager-array version, so callers don't need to
// know any of this happens.
//
// Sorting (directories first, then alphabetical) only happens when the
// folder is small enough to have cached a short sort key per entry while
// scanning (see SORT_KEY_CEILING) -- a folder past that ceiling is left
// in on-disk scan order instead of alphabetized, rather than paying to
// re-open thousands of files mid-sort just to compare their names.
class SdBrowser {
public:
    bool begin(const char* rootPath = BROWSE_ROOT);

    int entryCount() const { return _count; }
    const BrowserEntry& entry(int idx) const;
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

    // Drops the heap-allocated index/sort-key buffers, if any, without
    // touching _path -- entryCount() reports 0 until the next
    // begin()/refresh()/enterDir()/goUp() rebuilds them. Safe to call any
    // time: this only ever holds disposable, cheaply-rebuildable scan
    // data, never anything the user could lose (unlike e.g.
    // LooperMode::tracks[]'s unsaved recordings) -- so, unlike that
    // RAM-reclaim path, this one needs no interactive confirmation.
    // Called from main.cpp/LooperMode right before LooperMode::tracks[]
    // (128KB) allocates, so a browser that happens to be sitting on a
    // huge folder doesn't compete with it for RAM. See needsRebuild().
    void freeBuffers();

    // True exactly when freeBuffers() has dropped this browser's data and
    // nothing has rebuilt it since. A caller whose own entry point
    // doesn't already unconditionally refresh() (see FilePlayerMode::
    // enter(), which normally relies on the list staying valid across a
    // mode switch) should check this and refresh() if true, so a silent
    // reclaim elsewhere doesn't leave the screen looking emptied out.
    bool needsRebuild() const { return _buffersFreed; }

    // Fills outBuf with a short parenthesized note describing entries the
    // last scan didn't index -- " (+42)" if the exact remainder is known
    // (RAM-limited but the scan still finished enumerating the whole
    // directory), " (4096+)" if the scan hit MAX_INDEX_ENTRIES before
    // finishing and the true remainder past that is unknown. Empty string
    // if nothing was truncated. Meant to be appended directly after
    // currentPath() in a browser screen's header.
    void truncationNote(char* outBuf, size_t outSize) const;

private:
    // Safety ceiling on how many directory entries a single folder will
    // ever index, independent of RAM -- see loadEntries()'s headroom
    // check for the (usually tighter) RAM-driven cap. 4096 is comfortably
    // above "a couple thousand files", the real-world case this exists
    // for, with room to raise later if needed (same "bump the constant"
    // precedent MIDI_MAX_TRACKS/this class's own old MAX_DIR_ENTRIES
    // already document in the README's Known Limitations).
    static const int MAX_INDEX_ENTRIES = 4096;

    // Above this many entries, stop caching sort keys and leave the list
    // in on-disk scan order instead of alphabetized -- see loadEntries().
    static const int SORT_KEY_CEILING = 512;

    // How many bytes of a name (uppercased) to keep as a sort key -- long
    // enough that two different files sharing this exact prefix is rare,
    // short enough to stay cheap even at SORT_KEY_CEILING entries.
    static const int SORT_KEY_LEN = 12;

    // How many fully-materialized entries (name/size/isDir/kind) are kept
    // ready for display at once -- comfortably more than Ui::visibleRows()
    // (10 today) so a full screen plus some scroll slack never needs to
    // re-open anything already visible.
    static const int WINDOW_SIZE = 32;

    // Reserved, never allocated into, so a big index/sort-key allocation
    // never eats into what the stack or other transient allocations need
    // right after -- especially important for LooperMode's own
    // loopBrowser, which can only ever scan while tracks[] (128KB) is
    // already resident, unlike FilePlayerMode's browser... except
    // FilePlayerMode's own `browser` can ALSO end up scanning while
    // tracks[] is resident, since tracks[] deliberately stays allocated
    // across ordinary mode switches (see looper_mode.h) -- File Player ->
    // Looper -> back to File Player hits exactly this. Real-hardware
    // measurement in that exact scenario (rp2040.getFreeHeap() logged
    // right after tracks[] allocates): only 24320 bytes free, which the
    // old 24*1024 (24576) margin here was *just* over -- available clamped
    // to 0 and even a folder with a handful of entries indexed nothing at
    // all (see truncationNote()'s "(+N)" case with entryCount()==0, an
    // unnavigable browser). 12KB leaves comfortable room for the far
    // smaller stack/transient needs this margin actually guards against
    // while still letting an ordinary folder index normally in that
    // tightest-known-real case.
    static const uint32_t HEAP_SAFETY_MARGIN = 12 * 1024;

    // Per-entry index record -- see loadEntries(). ~8 bytes with
    // alignment, not the ~76-byte full BrowserEntry this replaces; that's
    // what makes indexing thousands of entries affordable.
    struct DirIndexEntry {
        uint32_t dirIndex; // SdFat's own dirIndex() -- the re-open key, see FsFile::open(dir, index, oflag)
        uint8_t isDir;     // 0/1
        uint8_t kind;      // FileKind, cached as a plain byte (avoids re-deriving it from the name on every materialize)
    };
    struct SortKeyEntry {
        char key[SORT_KEY_LEN]; // uppercased, zero-padded name prefix -- see loadEntries()/sortIndex()
    };

    char _path[192] = BROWSE_ROOT;

    DirIndexEntry* _index = nullptr;
    SortKeyEntry* _sortKeys = nullptr; // nullptr once this folder's over SORT_KEY_CEILING, or on a low-heap fallback -- see loadEntries()
    int _count = 0;                   // how many entries actually got indexed (may be less than _scannedTotal -- see truncationNote())

    bool _hitScanCap = false; // scan stopped at MAX_INDEX_ENTRIES before finishing -- true remainder past that point is unknown
    int _scannedTotal = 0;    // exact total qualifying entries found, valid whenever !_hitScanCap

    bool _buffersFreed = false; // see needsRebuild()

    // On-demand materialization cache backing entry() -- see its
    // implementation. mutable: entry() is logically const (browsing
    // doesn't change what the browser conceptually holds), but still
    // needs to fill this cache lazily. Round-robin eviction rather than
    // true LRU -- simpler, and scroll access is mostly sequential so it
    // behaves close to LRU in practice anyway.
    mutable BrowserEntry _window[WINDOW_SIZE];
    mutable int _windowLogicalIndex[WINDOW_SIZE]; // -1 = empty slot
    mutable int _windowNextSlot = 0;

    void loadEntries();
    void freeIndexAndSortKeys(); // same freeing as freeBuffers(), just without setting _buffersFreed -- used internally before every rebuild, not the external reclaim signal
    void sortIndex();            // insertion sort _index (and _sortKeys in lockstep) -- only called when _sortKeys is non-null
    const BrowserEntry& materialize(int idx) const;
    static bool classifyEntry(FsFile& file, const char* name, bool& isDirOut, uint8_t& kindOut);
    static bool isMidiFilename(const char* name);
    static bool isSysExFilename(const char* name);
    static bool isWavFilename(const char* name);
    static bool isModFilename(const char* name);
    static bool isS3mFilename(const char* name);
    static bool isXmFilename(const char* name);
};

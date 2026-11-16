#pragma once
#include <SdFat.h>
inline bool sdOpenPath(FsFile& f, const char* path, oflag_t flags = O_RDONLY) { return f.open(path, flags); }
inline bool sdExists(const char* path) { return g_files.count(path) != 0; }
inline bool sdRemove(const char* path) {
    auto it = g_files.find(path);
    if (it == g_files.end() || g_locked.count(path)) return false;
    g_cardFree += (long)it->second.size();
    g_files.erase(it);
    return true;
}
inline bool sdRename(const char* a, const char* b) {
    if (!g_files.count(a) || g_files.count(b)) return false;
    g_files[b] = g_files[a];
    g_files.erase(a);
    return true;
}

#pragma once
#include <SdFat.h>
extern SdFs sd;
bool sdOpenPath(FsFile& f, const char* path, int flags = O_RDONLY);
bool sdExists(const char* path);
bool sdRemove(const char* path);
bool sdRename(const char* oldPath, const char* newPath);

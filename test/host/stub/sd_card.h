#pragma once
#include <SdFat.h>
extern SdFs sd;
bool sdOpenPath(FsFile& f, const char* path, int flags = O_RDONLY);

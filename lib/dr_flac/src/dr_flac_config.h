#pragma once
// dr_flac with this firmware's options. Include this, not dr_flac.h, so
// every file sees the same struct layout (DR_FLAC_BUFFER_SIZE is in drflac).
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_SIMD
#define DR_FLAC_BUFFER_SIZE 2048
// Memory only through the allocation callbacks; these are never used.
#define DRFLAC_MALLOC(sz) ((void*)0)
#define DRFLAC_REALLOC(p, sz) ((void*)0)
#define DRFLAC_FREE(p) ((void)0)
#define DRFLAC_ASSERT(expression) ((void)0)
#include "dr_flac.h"

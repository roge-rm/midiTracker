#include "shared_ram_arena.h"
#include <stdint.h>

namespace {
// 16-byte alignment is more than any of this arena's actual occupants
// need on this platform (nothing here requires past 8-byte/uint64_t
// alignment), but it's cheap and safely covers any future occupant too.
alignas(16) uint8_t g_storage[SharedRamArena::SIZE];
} // namespace

namespace SharedRamArena {

void* data() { return g_storage; }

} // namespace SharedRamArena

// Decomp build, packaging (cmake/Decomp.cmake WWHD_DECOMP_RUNTIME_ONLY): the runtime of the decomp build
// with no decompiled function. The release links it only to check the runtime; setup links its own hooks
// (tools/decomp_build/gen.py hooks.cpp) and game units in its place, with game code translated by
// recomp.py --decomp-hooks (which also defines g_recomp_decomp_kept).
#include "decomp/decomp.h"

extern "C" {
extern const uint32_t g_recomp_decomp_kept[] = {0u};
extern const unsigned g_recomp_decomp_kept_count = 0;
}

namespace decomp {
const char* const kUnits[] = {""};
const unsigned kNumUnits = 0;
const UnitAlias kAliases[] = {{"", 0}};
const unsigned kNumAliases = 0;
Slot g_slots[] = {{0u, 0, "", nullptr}};
const unsigned kNumSlots = 0;
}  // namespace decomp

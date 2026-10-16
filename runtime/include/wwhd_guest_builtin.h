/* SwitchWakerHD (this fork): guest mods built into the NRO (docs/guest-mods-switch-plan.md).
 *
 * tools/switch/guest_mods.py translates each mod and writes build/guest-mods/registry.c, the table below;
 * cmake/SwitchPort.cmake compiles it into the NRO with WWHD_GUEST_BUILTIN=1, and
 * runtime/src/mods/guest_builtin.cpp loads its modules at the start as the mod manager loads built modules. */
#pragma once
#include "wwhd_guest_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WWHDGuestBuiltin {
    const char* id;
    const char* name;
    const char* version;
    WWHDGuestInitV1 init;
    uint32_t base, region_size;  /* guest memory: module and heap, 64 KiB aligned */
    uint32_t heap_size;
} WWHDGuestBuiltin;

extern const WWHDGuestBuiltin g_guest_builtin[];
extern const unsigned g_guest_builtin_count;

#ifdef __cplusplus
}
#endif

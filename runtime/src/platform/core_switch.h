// SwitchWakerHD (this fork): the Switch parts of core.cpp (core_switch.cpp): the guest memory window.
// The buffered SD-card log (log_flush, heap_never_used_mib, log_heap: runtime.h) and the pthread/thrd wraps
// live there too and need no call.
#pragma once

namespace core_switch {
void init_memory();  // mem::init(): back the guest ranges in a reserved 4 GiB code-memory window
}  // namespace core_switch

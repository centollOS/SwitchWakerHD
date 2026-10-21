// Native fast paths for small, very frequent game functions (native_helpers_switch.cpp,
// docs/cpu-threading-plan.md phase 2.1). Off by default: a test option (Switch tab, Debug; debug server "native").
#pragma once

namespace native_helpers {
bool on();
void set_on(bool on);  // any thread
}  // namespace native_helpers

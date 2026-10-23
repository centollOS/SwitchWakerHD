// Test switch (Switch Developer options): park the OliveOperationMgrThread (Miiverse requests) for good, as
// Cemu's "FPS Slowdown Fix" graphic pack does, instead of the 1 ms nap per step system_stubs.cpp gives it.
// Miiverse always fails here (nn_olv Initialize returns failure), so nothing waits on its answers; the thread
// resumes by itself when the switch is turned off. Any thread.
#pragma once

namespace hle_olv {
bool parked();
void set_parked(bool on);
}  // namespace hle_olv

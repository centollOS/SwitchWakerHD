// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Switch 2: the CPU clock setting (clocks_switch2.cpp): Switch 2 tab > "CPU clock" (s2_clock_switch2.cpp). Ported from
// ZeldaWWHDRecomp (lschaupp private fork, perf/clocks, 2026-11-21).
#pragma once
#include <string>

extern "C" void switch2_clocks_start(void);         // main_switch.cpp, once the log is open
extern "C" void switch2_clocks_restore_quiet(void); // the crash handler

namespace switch2_clocks {
int choice();                // 0 Normal, 1 Boost (the console's own boost rate, default), 2 Maximum (experimental)
void set_choice(int c, bool save = true);  // saved ("s2CpuClock") and applied within 50 ms by the clock thread
unsigned cpu_hz();           // the CPU rate now (0 unknown)
const char* env_override();  // the variable that wins for this start (WWHD_S2_CPU_MHZ / WWHD_S2_CPU_BOOST), or null
std::string status();        // what the clock thread did last, for the menu
}  // namespace switch2_clocks

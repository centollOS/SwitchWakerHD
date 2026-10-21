// (this fork) The performance test's console side (docs/perf-test.md, tools/switch/perf_test.py): the debug
// server's "bench" command. Benchmark places are portable states in sdmc:/switch/wwhd/bench/ (not the player's
// slots), the game clock can be held at one time of day, and N presented frames are recorded one by one (frame
// time, render thread busy, the main thread's CPU time, sampled GPU time, GPU behind, draws, internal resolution,
// whether the player had control) and sent back as text.
#pragma once
#include <cstdint>

struct Cpu;  // runtime/include/ppc.h

namespace bench {
// game thread, at the frame boundary (interp.cpp hook_0203593C, after ss::service)
void game_frame(Cpu* c);
// render thread, at the end of each present (gfx/deko/backend.cpp present()): gpuBusyMs/gpuSamples are the
// GPU pass sampling's last result and how many sampled frames it has read (a new value every 30th frame)
void present(uint64_t nowNs, bool gpuBehind, double gpuBusyMs, uint64_t gpuSamples, float resScale, uint64_t draws);
// debug_switch.cpp start(): adds "bench" to the debug server
void register_commands();
}  // namespace bench

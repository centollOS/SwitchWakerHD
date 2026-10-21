#pragma once
// Sampling profiler for the threads that make the frames (Windows on ARM64), for studying where
// their time goes (the game, the runtime, the Vulkan driver, the kernel) without administrator
// rights or ETW. Off unless WWHD_SAMPLE_THREADS=<output prefix>:
//   WWHD_SAMPLE_THREADS=C:/tmp/prof  -> C:/tmp/prof.samples (binary) and C:/tmp/prof.modules (text)
//   WWHD_SAMPLE_AFTER=40             -> start 40 s after the first thread registered (default 0)
//   WWHD_SAMPLE_SECONDS=30           -> stop after 30 s of sampling (default: until exit)
// About 1000 times a second each registered thread is suspended, its pc, lr and frame-pointer
// chain are copied (no allocation or lock while it is suspended) and it is resumed.
// tools/bench/thread_profile.py turns the files into a report.
// On the Switch (thread_sampler_switch.cpp) the threads register always and sampling runs on request: the debug
// server's "profile <seconds>" (tools/switch/main_profile.py).
#include <string>
namespace thread_sampler {
void add_current_thread(const char* tag);  // tag: a short name for the report ("render", "game")
#ifdef __SWITCH__
std::string start(double seconds);  // "" or why not
std::string status();               // "running", or the last run's file and counts
#endif
}

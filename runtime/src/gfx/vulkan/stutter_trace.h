#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gfxvk::stutter {
// Opt-in bounded in-memory recording: no filesystem writes in measured frames.
// Written on orderly process exit. Forced termination loses the trace.
struct Event {
  std::string kind;
  uint64_t frame, key, start, end, thread, cpu;
  int result;
};
inline uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
class Trace {
  const char* path = std::getenv("WWHD_VK_STUTTER_TRACE");
  std::mutex mutex;
  std::vector<Event> events;
  uint64_t dropped = 0;
  bool stopped = false;
public:
  Trace() { if (enabled()) events.reserve(65536); }
  bool enabled() const { return path && *path; }
  void add(const char* kind, uint64_t frame, uint64_t key,
           uint64_t start, uint64_t end, int result, uint64_t cpu = ~uint64_t{0}) {
    if (!enabled()) return;
    std::lock_guard lock(mutex);
    if (stopped) return;
    if (events.size() == 1000000) { ++dropped; return; }
    events.push_back({kind, frame, key, start, end,
        uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id())), cpu, result});
  }
  void flush() {
    if (!enabled()) return;
    std::vector<Event> snapshot;
    uint64_t lost;
    {
      std::lock_guard lock(mutex);
      if (stopped) return;
      stopped = true;
      snapshot.swap(events);
      lost = dropped;
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "[vulkan trace] cannot write %s\n", path); return; }
    std::fprintf(f, "kind,frame,key,start_ns,end_ns,thread,result,cpu_ns\n");
    for (const auto& e : snapshot)
      std::fprintf(f, "%s,%llu,%016llx,%llu,%llu,%llu,%d,%lld\n", e.kind.c_str(),
          (unsigned long long)e.frame, (unsigned long long)e.key,
          (unsigned long long)e.start, (unsigned long long)e.end,
          (unsigned long long)e.thread, e.result, e.cpu == ~uint64_t{0} ? -1ll : (long long)e.cpu);
    const bool failed = std::ferror(f);
    const int closed = std::fclose(f);
    std::fprintf(stderr, "[vulkan trace] %zu events, %llu dropped, write %s\n",
        snapshot.size(), (unsigned long long)lost, failed || closed ? "failed" : "complete");
  }
  ~Trace() { flush(); }
};
inline Trace& trace() { static Trace value; return value; }
inline void frame(uint64_t id) {
  if (!trace().enabled()) return;
  static uint64_t previous = 0;
  auto end = now();
  if (previous) trace().add("frame", id, 0, previous, end, 0);
  previous = end;
}
}

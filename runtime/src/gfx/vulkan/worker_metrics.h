#pragma once
#include "platform/host.h"
#include "stutter_trace.h"
#include <ctime>

namespace gfxvk::stutter {
// CPU consumed by this calling thread only. Excludes driver-created threads.
// Zero means unavailable; trace CPU fields use -1 for unavailable measurements.
inline uint64_t thread_cpu_now() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto ns = [](FILETIME value) {
    return ((uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime) * 100;
  };
  return ns(kernel) + ns(user);
#else
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value)) return 0;
  return uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec);
#endif
}
inline uint64_t cpu_elapsed(uint64_t start) {
  const auto end = start ? thread_cpu_now() : 0;
  return start && end >= start ? end - start : ~uint64_t{0};
}
}

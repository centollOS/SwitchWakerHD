// What this build is: the release version and commit, embedded at build time (cmake/BuildInfo.cmake
// writes the definitions into the build directory on every build). Shown in the performance report
// header (report_header.h) and the log's first line (also in crash logs).
#pragma once

namespace build {
const char* version();  // "v0.2.5" (a release, or the tagged commit), "v0.2.5+3" (3 commits later), "dev"
const char* commit();   // short commit hash, "-dirty" with local changes, "unknown" without git
}  // namespace build

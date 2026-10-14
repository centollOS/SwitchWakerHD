// Bounds every head-start reader applies (shaderprep.py build writes the file; tools/shaderprep.py
// HEADSTART_* hold the same numbers). A full-game template (about 90,000 states, 40,000 pipelines)
// builds a head start of about 120,000 records, 90 MiB on disk and 200 MiB decoded; templates may
// hold up to 1,000,000 states and 1,000,000 pipelines (tools/shaderprep_format.py MAX_ENTRIES).
// Records are read one at a time, so these only reject files that cannot be a head start.
#pragma once
#include <cstdint>

inline constexpr uint64_t kHeadstartMaxFileBytes = 2ull * 1024 * 1024 * 1024;
inline constexpr uint64_t kHeadstartMaxRecords = 4000000;
inline constexpr uint64_t kHeadstartMaxRawBytes = 8ull * 1024 * 1024 * 1024;
inline constexpr uint32_t kHeadstartMaxRecordBytes = 16 * 1024 * 1024;

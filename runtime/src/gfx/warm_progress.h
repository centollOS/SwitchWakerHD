#pragma once
// Progress of `wwhd --warm-shaders` for whoever runs it:
//  - setup (tools/installer/shader_prebuild.py) reads "WWHD-PROGRESS <done> <total> <detail>"
//    lines on stdout;
//  - Android's ShaderPrepareActivity polls WWHD_WARM_PROGRESS_FILE, which holds the latest
//    "<done> <total> <detail>" line (replaced atomically).
// At most ~10 reports per second; only the warm paths call it, never normal play.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace warmprogress {

// 1234567 -> "1,234,567"
inline std::string count(size_t value) {
    std::string digits = std::to_string(value), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

inline void report(size_t done, size_t total, const std::string& what, bool force = false) {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (done > total) done = total;
    if (!force && done < total && now - last < std::chrono::milliseconds(100)) return;
    last = now;
    const std::string line = std::to_string(done) + " " + std::to_string(total) + " " +
                             count(done) + " of " + count(total) + " " + what;
    std::printf("WWHD-PROGRESS %s\n", line.c_str());
    std::fflush(stdout);
    if (const char* path = std::getenv("WWHD_WARM_PROGRESS_FILE"); path && *path) {
        const std::string temporary = std::string(path) + ".new";
        if (FILE* file = std::fopen(temporary.c_str(), "w")) {
            const bool written = std::fprintf(file, "%s\n", line.c_str()) > 0;
            if (std::fclose(file) == 0 && written) std::rename(temporary.c_str(), path);
        }
    }
}

}  // namespace warmprogress

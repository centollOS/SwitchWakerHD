// (this fork, issue #47) game files the game cannot find or open: fs_problems.h
#include "fs_problems.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "../runtime.h"

namespace fs_problems {
namespace {
constexpr const char* kContent = "/vol/content/";
constexpr const char* kCommon = "/vol/content/common/";
constexpr const char* kCode = "/vol/code/";

bool starts_with(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

// the guest path as an absolute, lower-cased /vol path: Wii U volumes are case-insensitive, and a relative path is
// under the game's working directory, /vol/content (as fs.cpp host_path_exact maps it)
std::string key_of(const std::string& gpath) {
    std::string k = !gpath.empty() && gpath[0] == '/' ? gpath : kContent + gpath;
    for (char& c : k) c = (char)tolower((unsigned char)c);
    return k;
}

std::mutex g_mu;  // g_seen, and strerror (not thread-safe everywhere)
std::unordered_set<std::string> g_seen;

bool first_time(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_seen.insert(key).second;
}
}  // namespace

// The game looks a content file up with FSGetStat in /vol/content/Cafe/<language>/, Cafe/Common/, Common/ and last
// at /vol/content/<path>; most files are in Common/, so the first misses are normal. A miss at the content root for
// the path that last missed in Common/ on this thread means the file is in none of those places: the game then gives
// up without FSOpenFile. (A file the game probes on purpose in the same order would be logged too; none seen so far.)
void stat_missed(const std::string& gpath, int err, const std::function<std::string(const std::string&)>& read_path) {
    thread_local std::string commonMiss;  // after Common/, lower-cased ("" when none is pending)
    const std::string k = key_of(gpath);
    if (!starts_with(k, kContent)) return;
    if (starts_with(k, kCommon)) {
        commonMiss.assign(k, strlen(kCommon), std::string::npos);
        return;
    }
    const size_t n = strlen(kContent);
    if (commonMiss.empty() || k.compare(n, std::string::npos, commonMiss) != 0) return;
    commonMiss.clear();
    if (!first_time(k)) return;
    // where the file usually is: Common/ with the game's spelling of the rest
    const std::string at = read_path(std::string("/vol/content/Common/") + gpath.substr(gpath.size() - (k.size() - n)));
    std::lock_guard<std::mutex> lk(g_mu);
    LOG("[fs] the game cannot find %s in Common/ or the content root (expected at %s): %s", gpath.c_str(), at.c_str(),
        strerror(err));
}

// A read-only open of a game file that fails with anything but "not found" (permissions, an I/O error: a damaged
// copy or SD card). Not found is left to stat_missed: the game looks files up before opening them, and an open that
// fails on purpose (an optional file) must not read like broken game files.
void open_failed(const std::string& gpath, const std::string& mode, int err, const std::string& hp) {
    if (err == ENOENT || mode.find_first_of("wa+") != std::string::npos) return;
    const std::string k = key_of(gpath);
    if (!starts_with(k, kContent) && !starts_with(k, kCode)) return;
    if (!first_time(k)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    LOG("[fs] cannot open %s (at %s): %s", gpath.c_str(), hp.c_str(), strerror(err));
}
}  // namespace fs_problems

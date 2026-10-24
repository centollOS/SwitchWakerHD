// (this fork) Training builds only (cmake -DWWHD_PGO=GEN, docs/pgo.md): the debug server's "pgo" command.
//
// The hot game files and the runtime's frame-thread code are instrumented (-fprofile-generate); libgcov keeps the
// counters in memory. "pgo dump" writes them to sdmc:/switch/wwhd/pgo/*.gcda, adding to what an earlier dump of
// the same session left there (libgcov merges), and then zeroes them, so every scene's training is dumped once
// and counted once. tools/switch/pgo_train.py visits scenes and fetches the files.
#ifdef WWHD_PGO_GEN
#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include "debug_server.h"

extern "C" {
void __gcov_dump(void);
void __gcov_reset(void);
}

namespace pgo {
namespace {
constexpr const char* kPrefix = "sdmc:/switch/wwhd";
constexpr const char* kDir = "sdmc:/switch/wwhd/pgo";

int g_dumps = 0;

// the folder libgcov writes into must exist: its own mkdir does not cope with the "sdmc:" device
void prepare_dir() {
    setenv("GCOV_PREFIX", kPrefix, 1);
    setenv("GCOV_PREFIX_STRIP", "0", 1);
    mkdir(kDir, 0777);
}

debugsrv::Reply command(const debugsrv::Args& a) {
    const std::string what = a.empty() ? "status" : a[0];
    if (what == "reset") {
        __gcov_reset();
        return debugsrv::ok("pgo: counters zeroed");
    }
    if (what == "dump") {
        prepare_dir();
        __gcov_dump();  // adds to the files already in sdmc:/switch/wwhd/pgo/
        __gcov_reset();
        ++g_dumps;
        return debugsrv::ok("pgo: dumped to " + std::string(kDir) + " (dump " + std::to_string(g_dumps) +
                            " of this session); counters zeroed");
    }
    if (what == "status") return debugsrv::ok("pgo: training build, " + std::to_string(g_dumps) + " dumps this session; files in " + kDir);
    return debugsrv::err("pgo dump | reset | status");
}
}  // namespace

void register_commands() {
    prepare_dir();
    debugsrv::add_command("pgo", "pgo dump | reset | status   (training build) write / zero the profile counters (gcda in pgo/)",
                          command);
}
}  // namespace pgo
#endif  // WWHD_PGO_GEN

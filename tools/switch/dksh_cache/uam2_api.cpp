#include "uam_api.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

extern char** environ;

namespace uam {
namespace {

std::string exe() { return UAM2_EXE; }

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Vertex: return "vert";
        case Stage::TessCtrl: return "tess_ctrl";
        case Stage::TessEval: return "tess_eval";
        case Stage::Geometry: return "geom";
        case Stage::Fragment: return "frag";
        case Stage::Compute: return "comp";
    }
    return "vert";
}

bool read_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string temp_file(const char* suffix) {
    const char* dir = getenv("TMPDIR");
    std::string t = std::string(dir && *dir ? dir : "/tmp") + "/wwhd_uam2_XXXXXX" + suffix;
    const int fd = mkstemps(t.data(), int(strlen(suffix)));
    if (fd < 0) return {};
    close(fd);
    return t;
}

}  // namespace

void init(bool) {
    struct stat st;
    if (stat(exe().c_str(), &st) != 0 || !(st.st_mode & S_IXUSR))
        fprintf(stderr, "dksh_cache: uam2 not found at %s\n", exe().c_str());
}
void set_dual_issue(bool) {}
void shutdown() {}

Result compile(Stage stage, const char* glsl) {
    Result r;
    const std::string in = temp_file(".glsl"), out = temp_file(".dksh"), log = temp_file(".log");
    if (in.empty() || out.empty() || log.empty()) {
        r.log = std::string("cannot create a temporary file: ") + strerror(errno);
        for (const std::string* p : {&in, &out, &log})
            if (!p->empty()) unlink(p->c_str());
        return r;
    }
    {
        std::ofstream f(in, std::ios::binary);
        f << glsl;
    }
    const std::string bin = exe();
    const char* argv[] = {bin.c_str(), "-s", stage_name(stage), "-o", out.c_str(), in.c_str(), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_TRUNC, 0);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = 0;
    int status = 0;
    const int rc = posix_spawn(&pid, bin.c_str(), &fa, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        r.log = "cannot run " + bin + ": " + strerror(rc);
    } else {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        read_file(log, &r.log);
        std::string dksh;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && read_file(out, &dksh) && !dksh.empty()) {
            r.ok = true;
            r.dksh.assign(dksh.begin(), dksh.end());
        } else if (WIFSIGNALED(status)) {
            r.log += "\nuam2 ended by signal " + std::to_string(WTERMSIG(status));
        } else if (r.log.empty()) {
            r.log = "uam2 failed without a message (exit " + std::to_string(WEXITSTATUS(status)) + ")";
        }
    }
    unlink(in.c_str());
    unlink(out.c_str());
    unlink(log.c_str());
    return r;
}

}  // namespace uam

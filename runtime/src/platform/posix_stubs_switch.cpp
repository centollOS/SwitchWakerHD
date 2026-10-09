// The POSIX calls upstream's desktop-only features link against and libnx lacks: code mods (mods/code_mods.cpp,
// platform/process.h: rebuilding and restarting the executable) and guest mod files (mods/guest_files.h).
// Neither can run on the Switch (no processes; native guest mods are refused, guest_mods.cpp), so each call
// fails with ENOSYS. Kept here, not as #ifdefs in upstream's files, so upstream syncs stay simple.
#include <cerrno>
#include <spawn.h>
#include <sys/types.h>

extern "C" {
int pipe(int*) { errno = ENOSYS; return -1; }
int execv(const char*, char* const*) { errno = ENOSYS; return -1; }
pid_t waitpid(pid_t, int*, int) { errno = ENOSYS; return -1; }
int openat(int, const char*, int, ...) { errno = ENOSYS; return -1; }
int posix_spawn_file_actions_init(posix_spawn_file_actions_t*) { return ENOSYS; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*) { return 0; }
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int, const char*, int, mode_t) { return ENOSYS; }
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int) { return ENOSYS; }
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int) { return ENOSYS; }
int posix_spawnp(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const*,
                 char* const*) { return ENOSYS; }
}

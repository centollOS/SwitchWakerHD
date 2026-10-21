#include "crash_redact.h"
#include <cassert>
#include <string>
static std::string result;
static void out(int, const char* p, size_t n) { result.append(p,n); }
int main() {
    crash_context::Redactor r; r.initialize("/Users/alice", "alice");
    r.write(0, "HOME=/Users/alice/cache\nuser=alice\n/home/bob/file\nC:\\Users\\bob\\file\n", out);
    assert(result == "HOME=~/cache\nuser=<user>\n~/file\n~\\file\n");
    result.clear();
    r.write(0, std::string(4000, 'x') + "/Users/alice/end", out);
    assert(result == std::string(4000, 'x') + "~/end");
    result.clear();
    crash_context::Redactor windows; windows.initialize("C:\\Users\\Alice", "Alice");
    windows.write(0, "C:/users/ALICE/game D:\\USERS\\Other Person\\save", out);
    assert(result == "~/game ~\\save");
    result.clear();
    crash_context::Redactor android; android.initialize(nullptr, nullptr);
    android.write(0, "/storage/emulated/10/Android/data/app/files /data/user/10/app/cache /data/user_de/10/app", out);
    assert(result == "~/Android/data/app/files ~/app/cache ~/app");
    result.clear();
    r.write(0, "/Users/alice2/game /home/Bob Smith/save", out);
    assert(result == "~/game ~/save");
}

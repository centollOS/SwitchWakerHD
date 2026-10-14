// Experimental mods and save backups (mods/save_backup.h, packages.h): catalogue/manifest flag,
// list filtering, the backup made before an experimental mod can be enabled, backup naming,
// no-overwrite and retention, staged restore with its "before restore" backup, and failure paths.
// Temporary folders only; never the player's saves or mod manager.
#include "mod_test_host.h"
#include "mods/packages.h"
#include "mods/save_backup.h"
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <winioctl.h>
#endif

namespace fs = std::filesystem;
using namespace mods;

namespace {
fs::path temp_root(const char* name) {
    auto root = fs::temp_directory_path() / (std::string(name) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    return root;
}
std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
// Prefer a symbolic link. On Windows without the symlink privilege, a junction still
// exercises refusal to delete a directory that redirects outside the backup store.
bool directory_link(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (!ec) return true;
#ifdef _WIN32
    if (ec.value() == ERROR_PRIVILEGE_NOT_HELD) {
        auto print = fs::absolute(target).make_preferred().wstring();
        auto substitute = print.starts_with(L"\\\\") ? L"\\??\\UNC\\" + print.substr(2) : L"\\??\\" + print;
        struct {
            DWORD tag;
            WORD length, reserved;
            WORD substitute_offset, substitute_length, print_offset, print_length;
            wchar_t paths[8192];
        } data{};
        static_assert(offsetof(decltype(data), paths) == 16);
        auto path_bytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
        assert(offsetof(decltype(data), paths) + path_bytes <= MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
        data.tag = IO_REPARSE_TAG_MOUNT_POINT;
        data.length = WORD(8 + path_bytes);
        data.substitute_length = WORD(substitute.size() * sizeof(wchar_t));
        data.print_offset = WORD((substitute.size() + 1) * sizeof(wchar_t));
        data.print_length = WORD(print.size() * sizeof(wchar_t));
        std::memcpy(data.paths, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
        std::memcpy(data.paths + substitute.size() + 1, print.c_str(), data.print_length + sizeof(wchar_t));
        assert(fs::create_directory(link));
        HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        DWORD failure = ERROR_SUCCESS, returned = 0;
        if (handle == INVALID_HANDLE_VALUE) failure = GetLastError();
        else {
            if (!DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, 8 + data.length,
                                 nullptr, 0, &returned, nullptr)) failure = GetLastError();
            CloseHandle(handle);
        }
        if (failure) {
            RemoveDirectoryW(link.c_str());
            throw fs::filesystem_error("create test junction", target, link,
                                       std::error_code(failure, std::system_category()));
        }
        std::cout << "symlink privilege unavailable; testing an NTFS junction\n";
        return false;
    }
#endif
    throw fs::filesystem_error("create test directory symlink", target, link, ec);
}
packages::View view(const std::string& id) {
    for (auto v : packages::list()) if (v.id == id) return v;
    return {};
}
size_t entries(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) { (void)e; n++; }
    return n;
}
const char* kEntry = R"({"id":"%s","name":"N","description":"D","version":"1.0.0","kind":"guest","authors":["A"],"licences":["MPL-2.0"],"port_versions":{"minimum":"0.2.0"},"downloads":{"all":{"url":"https://example.invalid/x.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000","size":1}}%s})";
std::string entry(const char* id, const char* extra) {
    char text[1024];
    snprintf(text, sizeof text, kEntry, id, extra);
    return text;
}

void schema() {
    auto index = catalogue::parse(R"({"format_version":1,"mods":[)" + entry("plain", "") + "," +
                                  entry("risky", R"(,"experimental":true)") + "," +
                                  entry("tested", R"(,"experimental":false)") + "]}");
    assert(index.entries.size() == 3);
    assert(!index.entries[0].experimental && index.entries[1].experimental && !index.entries[2].experimental);
    bool rejected = false;
    try { catalogue::parse(R"({"format_version":1,"mods":[)" + entry("bad", R"(,"experimental":"yes")") + "]}"); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
    // list filtering: hidden only while the toggle is off and the mod is not installed
    const auto& plain = index.entries[0];
    const auto& risky = index.entries[1];
    assert(catalogue::visible(plain, false, false) && catalogue::visible(plain, true, false));
    assert(!catalogue::visible(risky, false, false));
    assert(catalogue::visible(risky, true, false));
    assert(catalogue::visible(risky, false, true));  // installed: always listed, labelled
    std::cout << "catalogue flag and list filtering passed\n";
}

void store_rules() {
    auto root = temp_root("wwhd-save-backups");
    auto save = root / "save";
    saves::Store store(save);
    store.log = [](const std::string&) {};
    std::time_t clock = 1760000000;
    store.now = [&] { return clock; };
    // no save data yet: nothing created, still ok
    auto empty = store.create("manual");
    assert(empty.ok && !empty.created && !fs::exists(root / "save-backups" / empty.name / "backup.json"));
    write(save / "user/cking.sav", "progress-1");
    write(save / "common/system.sav", "options");
    auto first = store.create("before-mod", "dragon", "Call of the Sky");
    assert(first.ok && first.created);
    assert(first.name.ends_with("-mod-dragon"));
    assert(read(store.root() / first.name / "save/user/cking.sav") == "progress-1");
    assert(read(store.root() / first.name / "save/common/system.sav") == "options");
    // same second again: a new folder, the first is untouched
    write(save / "user/cking.sav", "progress-2");
    auto second = store.create("before-mod", "dragon");
    assert(second.ok && second.name == first.name + "-2");
    assert(read(store.root() / first.name / "save/user/cking.sav") == "progress-1");
    assert(read(store.root() / second.name / "save/user/cking.sav") == "progress-2");
    // listing: newest first, metadata intact
    auto list = store.list();
    assert(list.size() == 2 && list[0].name == second.name && list[1].mod_name == "Call of the Sky");
    assert(list[1].description() == "Before enabling Call of the Sky" && list[1].bytes == 17 && list[1].files == 2);
    // retention: 10 automatic backups kept, manual ones never removed, the staged one kept
    auto manual = store.create("manual");
    assert(manual.ok && !store.list().empty());
    std::string error;
    assert(store.restore(first.name, error));  // staged: the oldest automatic backup must survive
    for (int i = 0; i < 12; i++) { clock += 60; assert(store.create("before-mod", "m" + std::to_string(i)).ok); }
    clock += 60;
    auto manual2 = store.create("manual");
    size_t automatic = 0, kept_manual = 0;
    bool staged_kept = false;
    for (const auto& info : store.list()) {
        if (info.automatic) automatic++; else kept_manual++;
        if (info.name == first.name) staged_kept = true;
    }
    assert(automatic == saves::kKeepAutomatic + 1 && staged_kept);  // the newest 10 plus the staged one
    assert(kept_manual == 2 && fs::exists(store.root() / manual.name) && fs::exists(store.root() / manual2.name));
    assert(!fs::exists(store.root() / second.name));  // the oldest unstaged automatic one went
    // folders the store did not make are never touched or listed
    write(store.root() / "my-notes/readme.txt", "mine");
    store.invalidate();
    for (int i = 0; i < 3; i++) { clock += 60; store.create("before-mod", "x"); }
    assert(read(store.root() / "my-notes/readme.txt") == "mine");

    // staged restore: applied at the next start, after a "before restore" backup of the current save
    write(save / "user/cking.sav", "progress-after-mod");
    write(save / "user/extra.sav", "new file");
    assert(store.pending() == first.name);
    saves::Store next_start(save);  // a new process
    next_start.log = [](const std::string&) {};
    next_start.now = [&] { return clock + 3600; };
    auto applied = next_start.apply_pending();
    assert(applied.ok && applied.created && applied.name == first.name);
    assert(read(save / "user/cking.sav") == "progress-1" && !fs::exists(save / "user/extra.sav"));
    assert(next_start.pending().empty() && !fs::exists(next_start.root() / "restore-pending.json"));
    const auto& after = next_start.list();
    assert(after[0].reason == "before-restore");
    assert(read(next_start.root() / after[0].name / "save/user/cking.sav") == "progress-after-mod");
    assert(applied.message.find(after[0].name) != std::string::npos);
    // undo: restore the "before restore" backup
    assert(next_start.restore(after[0].name, error));
    auto undone = next_start.apply_pending();
    assert(undone.ok && read(save / "user/cking.sav") == "progress-after-mod" && read(save / "user/extra.sav") == "new file");
    // nothing staged: no-op
    auto idle = next_start.apply_pending();
    assert(idle.ok && !idle.created);
    // a staged backup that vanished: saves unchanged, reported once
    write(next_start.root() / "restore-pending.json", R"({"format_version":1,"backup":"2000-01-01_00-00-00-manual"})");
    auto missing = next_start.apply_pending();
    assert(!missing.ok && missing.error.find("not changed") != std::string::npos);
    assert(read(save / "user/cking.sav") == "progress-after-mod" && !fs::exists(next_start.root() / "restore-pending.json"));
    // cancel
    assert(next_start.restore(manual.name, error) && next_start.pending() == manual.name);
    assert(next_start.cancel_restore(error) && next_start.pending().empty());
    // a restore whose copy fails leaves the save as it was and no leftovers next to it
    assert(next_start.restore(manual.name, error));
    next_start.copy_file = [&](const fs::path& from, const fs::path& to) {
        if (from.string().find(manual.name) != std::string::npos) throw std::runtime_error("No space left on device");
        fs::copy_file(from, to);
    };
    auto broken = next_start.apply_pending();
    assert(!broken.ok && broken.error.find("No space left") != std::string::npos);
    assert(read(save / "user/cking.sav") == "progress-after-mod");
    for (const auto& e : fs::directory_iterator(root)) assert(!e.path().filename().string().starts_with(".save-"));

    // backup failures: no partial backup, no temporary folder left behind
    size_t before = entries(store.root());
    store.free_space = [](const fs::path&) { return uint64_t(1024); };
    auto full = store.create("manual");
    assert(!full.ok && full.error.find("Not enough free disk space") != std::string::npos);
    store.free_space = [](const fs::path&) { return UINT64_MAX; };
    store.copy_file = [](const fs::path&, const fs::path&) { throw std::runtime_error("copy error"); };
    auto failed = store.create("manual");
    assert(!failed.ok && failed.error.find("copy error") != std::string::npos);
    assert(entries(store.root()) == before);
    for (const auto& e : fs::directory_iterator(store.root())) assert(!e.path().filename().string().starts_with(".tmp-"));
    fs::remove_all(root);
    std::cout << "backup naming, no overwrite, retention, staged restore, undo and failure paths passed\n";
}

// Short folder names (issue #144): the mod id is cut to 7 characters, never ending with a dot.
void short_names() {
    auto root = temp_root("wwhd-save-names");
    saves::Store store(root / "save");
    store.log = [](const std::string&) {};
    std::time_t clock = 1760000000;
    store.now = [&] { return clock; };
    write(root / "save/user/cking.sav", "progress");
    auto mod = store.create("before-mod", "abcdef.ghij-long-mod-id");
    assert(mod.ok && mod.name.ends_with("-mod-abcdef"));
    clock += 60;
    assert(store.create("before-restore").name.ends_with("-restore"));
    assert(store.list().size() == 2 && store.list()[1].mod == "abcdef.ghij-long-mod-id");
    fs::remove_all(root);
}

void deletion() {
    auto root = temp_root("wwhd-save-delete");
    auto save = root / "save";
    write(save / "user/cking.sav", "progress");
    saves::Store store(save);
    store.log = [](const std::string&) {};
    std::string error;
    auto manual = store.create("manual");
    auto automatic = store.create("before-mod", "dragon");
    auto staged = store.create("before-restore");
    assert(manual.ok && automatic.ok && staged.ok && store.list().size() == 3);
    auto hidden_leftovers = [&] {
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(store.root())) if (e.path().filename().string().starts_with(".")) n++;
        return n;
    };
    // manual and automatic backups can be deleted; the folder is gone and the list follows
    assert(store.remove(manual.name, error) && !fs::exists(store.root() / manual.name));
    assert(store.remove(automatic.name, error) && !fs::exists(store.root() / automatic.name));
    assert(store.list().size() == 1 && hidden_leftovers() == 0);
    // the backup staged for restore cannot be deleted
    assert(store.restore(staged.name, error));
    assert(!store.remove(staged.name, error) && error.find("staged for restore") != std::string::npos);
    assert(fs::exists(store.root() / staged.name / "backup.json"));
    assert(store.cancel_restore(error) && store.list().size() == 1);
    // folders the store did not make, names outside save-backups and links are refused
    write(store.root() / "my-notes/readme.txt", "mine");
    assert(!store.remove("my-notes", error) && read(store.root() / "my-notes/readme.txt") == "mine");
    write(root / "outside/backup.json", R"({"format_version":1,"reason":"manual","created":1})");
    assert(!store.remove("../outside", error) && fs::exists(root / "outside/backup.json"));
    assert(!store.remove("", error) && !store.remove(".", error) && !store.remove("..", error));
    auto link = store.root() / "2026-01-01_00-00-00-manual";
    auto metadata = read(root / "outside/backup.json");
    bool symbolic = directory_link(root / "outside", link);
    assert(fs::equivalent(link, root / "outside"));
    assert(!store.remove("2026-01-01_00-00-00-manual", error));
    assert(error == "Could not delete the backup: Not a save backup");
    assert(read(root / "outside/backup.json") == metadata && fs::equivalent(link, root / "outside"));
    if (symbolic) assert(fs::is_symlink(link));
    assert(fs::remove(link));
    assert(read(root / "outside/backup.json") == metadata);
    // a failed rename leaves the backup complete and listed
    auto again = store.create("manual");
    store.move = [](const fs::path&, const fs::path&) { throw std::runtime_error("permission denied"); };
    assert(!store.remove(again.name, error) && error.find("permission denied") != std::string::npos);
    assert(fs::exists(store.root() / again.name / "backup.json") && store.list().size() == 2);
    // a removal interrupted after the rename: no longer listed, leftovers cleared on the next backup
    store.move = [](const fs::path& from, const fs::path& to) { fs::rename(from, to); };
    store.remove_tree = [](const fs::path&) { throw std::runtime_error("interrupted"); };
    assert(store.remove(again.name, error) && store.list().size() == 1 && hidden_leftovers() == 1);
    store.remove_tree = [](const fs::path& path) { fs::remove_all(path); };
    assert(store.create("manual").ok && hidden_leftovers() == 0 && store.list().size() == 2);
    fs::remove_all(root);
    std::cout << "delete manual/automatic, staged refused, foreign/escaping/link refused, failures consistent passed\n";
}

void unicode_paths() {
    auto root = temp_root("wwhd-save-unicode");
    auto save = root / fs::path(u8"Später ゼルダ") / "save";
    write(save / "user/cking.sav", "unicode");
    saves::Store store(save);
    store.log = [](const std::string&) {};
    auto made = store.create("manual");
    assert(made.ok && made.created && read(store.root() / made.name / "save/user/cking.sav") == "unicode");
    std::string error;
    write(save / "user/cking.sav", "changed");
    assert(store.restore(made.name, error) && store.apply_pending().ok && read(save / "user/cking.sav") == "unicode");
    fs::remove_all(root);
    std::cout << "non-ASCII save path passed\n";
}

void manager_flow() {
    auto root = temp_root("wwhd-experimental-manager");
    env("WWHD_NO_HOST_INPUT", "1");
    env("WWHD_MOD_MANAGER_DIR", (root / "manager").string().c_str());
    env("WWHD_TEST_TRUST_NATIVE_MODS", nullptr);
    auto save = root / "save";
    write(save / "user/cking.sav", "before the mod");
    {
        std::lock_guard guard(saves::mutex());
        saves::store().configure(save);
        saves::store().log = [](const std::string&) {};
    }
    packages::initialize();
    std::string error;
    auto package = [&](const char* id, const char* extra) {
        auto dir = root / "sources" / id;
        write(dir / "manifest.json", std::string(R"({"format_version":1,"id":")") + id +
              R"(","name":")" + id + R"(","version":"1.0.0","game_id":"wwhd-usa","kind":"settings")" + extra + "}");
        return dir.string();
    };
    // invalid flag type: refused
    assert(!packages::install(package("broken", R"(,"settings":{"quick-doors":true},"experimental":1)"), error) && error.find("experimental") != std::string::npos);
    assert(packages::install(package("risky", R"(,"settings":{"quick-doors":true},"experimental":true)"), error));
    assert(view("risky").experimental && !view("risky").enabled);
    // no confirmation: every enable path refuses
    assert(packages::unacknowledged_experimental("risky").size() == 1);
    assert(!packages::enable("risky", true, error) && error.find("experimental") != std::string::npos);
    assert(!packages::enable_after_code_rebuild("risky", error));
    assert(!view("risky").enabled);
    auto backups = save.parent_path() / "save-backups";
    assert(!fs::exists(backups) || entries(backups) == 0);
    // confirming makes the backup first, while the mod is still off
    assert(packages::acknowledge_experimental("risky", error));
    assert(!view("risky").enabled && packages::unacknowledged_experimental("risky").empty());
    std::string made;
    {
        std::lock_guard guard(saves::mutex());
        auto list = saves::store().list();
        assert(list.size() == 1 && list[0].mod == "risky" && list[0].reason == "before-mod");
        made = list[0].name;
    }
    assert(read(backups / made / "save/user/cking.sav") == "before the mod");
    assert(packages::enable("risky", true, error) && view("risky").enabled);
    // the confirmation is used up: off and on again asks (and backs up) again
    assert(packages::enable("risky", false, error));
    assert(!packages::enable("risky", true, error) && !packages::unacknowledged_experimental("risky").empty());
    // a backup that fails: the mod stays off, with a clear reason
    {
        std::lock_guard guard(saves::mutex());
        saves::store().free_space = [](const fs::path&) { return uint64_t(0); };
    }
    assert(!packages::acknowledge_experimental("risky", error));
    assert(error.find("Not enough free disk space") != std::string::npos && error.find("was not turned on") != std::string::npos);
    assert(!packages::enable("risky", true, error) && !view("risky").enabled);
    {
        std::lock_guard guard(saves::mutex());
        saves::store().free_space = [](const fs::path&) { return UINT64_MAX; };
        saves::store().copy_file = [](const fs::path&, const fs::path&) { throw std::runtime_error("copy error"); };
    }
    assert(!packages::acknowledge_experimental("risky", error) && error.find("copy error") != std::string::npos);
    assert(!packages::enable("risky", true, error) && !view("risky").enabled);
    {
        std::lock_guard guard(saves::mutex());
        saves::store().copy_file = [](const fs::path& from, const fs::path& to) { fs::copy_file(from, to); };
    }
    // a normal mod that requires the experimental one: the warning names the dependency
    assert(packages::install(package("needs-risky", R"(,"settings":{"fast-scenes":true},"dependencies":[{"id":"risky"}])"), error));
    assert(!view("needs-risky").experimental);
    auto names = packages::unacknowledged_experimental("needs-risky");
    assert(names.size() == 1 && names[0].first == "risky");
    assert(!packages::enable("needs-risky", true, error));
    assert(packages::acknowledge_experimental("needs-risky", error));
    assert(packages::enable("needs-risky", true, error) && view("risky").enabled && view("needs-risky").enabled);
    // a non-experimental mod never asks
    assert(packages::install(package("plain", R"(,"settings":{"direct-camera":true})"), error));
    assert(packages::unacknowledged_experimental("plain").empty() && !packages::acknowledge_experimental("plain", error));
    // a package built before its manifest had the flag, installed from an experimental catalogue entry
    packages::mark_experimental("plain", true);
    assert(view("plain").experimental && !packages::unacknowledged_experimental("plain").empty());
    assert(!packages::enable("plain", true, error));
    packages::mark_experimental("plain", false);
    assert(!view("plain").experimental && packages::enable("plain", true, error));
    // no save data yet: confirming works without a backup
    assert(packages::enable("needs-risky", false, error) && packages::enable("risky", false, error));
    fs::remove_all(save);
    assert(packages::acknowledge_experimental("risky", error) && packages::enable("risky", true, error));
    fs::remove_all(root);
    std::cout << "experimental enable needs confirmation, backs up first, failures keep the mod off\n";
}
}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "schema" || mode == "all") schema();
    if (mode == "store" || mode == "all") { store_rules(); short_names(); }
    if (mode == "unicode" || mode == "all") unicode_paths();
    if (mode == "delete" || mode == "all") deletion();
    if (mode == "manager" || mode == "all") manager_flow();
    return 0;
}

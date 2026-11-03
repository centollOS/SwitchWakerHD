// Mods > More > Install from a file: what a file chooser hands back (a desktop path, or Android's content://
// URI) made into a path the installer reads (runtime/src/mods/picked_source.h).
#include "mods/picked_source.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace mods::picked;

int main() {
    // classify: paths, documents, folders
    assert(classify("") == Kind::Empty);
    assert(classify("/home/me/Story Quests.wwhdmod") == Kind::Path);
    assert(classify("C:\\Mods\\pack.zip") == Kind::Path);
    assert(classify("content") == Kind::Path);
    assert(classify("contents://x/document/1") == Kind::Path);
    assert(classify("content://com.android.providers.downloads.documents/document/msf%3A42") == Kind::Document);
    assert(classify("CONTENT://com.android.externalstorage.documents/document/primary%3ADownload%2Fmod.zip") == Kind::Document);
    assert(classify("content://media/external/file/12") == Kind::Document);
    assert(classify("content://com.android.externalstorage.documents/tree/primary%3AMods") == Kind::Folder);
    assert(classify("content://com.android.externalstorage.documents/tree/primary%3AMods/document/primary%3AMods%2Fa.zip") == Kind::Document);
    assert(classify("content://authority") == Kind::Document);

    Local local;
    std::string error;
    int copies = 0;
    // a path is installed as it is: no copy, nothing to remove afterwards
    CopyDocument never = [&](const std::string&) -> std::string { copies++; return "unused"; };
    assert(resolve("/tmp/a mod.zip", never, local, error) && local.path == "/tmp/a mod.zip" && !local.temporary && error.empty());
    assert(copies == 0);
    // nothing picked: no install, no message
    assert(!resolve("", never, local, error) && error.empty());
    // a folder (Android's tree URI): said plainly, no copy
    assert(!resolve("content://com.android.externalstorage.documents/tree/primary%3AMods", never, local, error));
    assert(error == kFolderUnsupported && copies == 0);
    // a document on a host without a copier (a desktop chooser never answers this way)
    assert(!resolve("content://x/document/1", nullptr, local, error) && error == kDocumentUnsupported);

    // a document: copied, installed from the copy, and the copy and its folder removed afterwards
    const auto base = fs::temp_directory_path() /
                      ("picked_source_test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string asked;
    CopyDocument copy = [&](const std::string& uri) {
        asked = uri;
        auto folder = base / "mod-picks" / "1";
        fs::create_directories(folder);
        auto file = folder / "Story Quests.wwhdmod";
        std::ofstream(file) << "PK";
        return file.string();
    };
    const std::string uri = "content://com.android.providers.downloads.documents/document/msf%3A42";
    assert(resolve(uri, copy, local, error) && local.temporary && error.empty() && asked == uri);
    assert(fs::path(local.path).filename() == "Story Quests.wwhdmod" && fs::is_regular_file(local.path));
    release(local);
    assert(!fs::exists(local.path) && !fs::exists(base / "mod-picks" / "1") && fs::exists(base / "mod-picks"));
    // release leaves a picked path alone
    fs::create_directories(base);
    auto own = base / "own.zip";
    std::ofstream(own) << "PK";
    release(Local{own.string(), false});
    assert(fs::exists(own));

    // the copier's message (too large, unreadable) reaches the player
    CopyDocument refuses = [](const std::string&) -> std::string { throw std::runtime_error("This file is too large for a mod package (over 512 MB)"); };
    assert(!resolve(uri, refuses, local, error) && error == "This file is too large for a mod package (over 512 MB)" && !local.temporary);
    CopyDocument empty = [](const std::string&) { return std::string(); };
    assert(!resolve(uri, empty, local, error) && error == kDocumentUnsupported && !local.temporary);

    fs::remove_all(base);
    std::puts("picked_source_test: paths, documents copied and removed, folders and failures PASS");
}

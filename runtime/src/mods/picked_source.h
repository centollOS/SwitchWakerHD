#pragma once
// What a file chooser handed back for Mods > More > Install from a file or folder, made into a path
// the package installer (packages::install) can read. Desktop choosers answer with a path. Android's
// (SDL's ACTION_OPEN_DOCUMENT) answers with a content:// URI: the host copies that document to a
// temporary file first (copy_document), and release() removes the copy once it is installed.
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace mods::picked {

enum class Kind { Empty, Path, Document, Folder };

// content://<authority>/tree/<id> is a whole folder (ACTION_OPEN_DOCUMENT_TREE); any other content:// URI
// (content://<authority>/document/<id>, .../tree/<id>/document/<id>, a provider's own form) is one file.
inline Kind classify(std::string_view picked) {
    if (picked.empty()) return Kind::Empty;
    constexpr std::string_view scheme = "content://";
    if (picked.size() < scheme.size()) return Kind::Path;
    for (size_t i = 0; i < scheme.size(); i++) {
        char c = picked[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != scheme[i]) return Kind::Path;
    }
    auto rest = picked.substr(scheme.size());
    auto slash = rest.find('/');
    if (slash == std::string_view::npos) return Kind::Document;
    auto path = rest.substr(slash + 1);
    if (path.starts_with("tree/") && path.find("/document/") == std::string_view::npos) return Kind::Folder;
    return Kind::Document;
}

struct Local {
    std::string path;        // what to install
    bool temporary = false;  // a copy made for the install: release() removes it
};

// Copies a content:// document to a temporary file named as the document is (the name tells a ZIP from
// a replacement .pack) and returns its path; throws with a message for the player when it can't.
using CopyDocument = std::function<std::string(const std::string& uri)>;

inline constexpr const char* kFolderUnsupported =
    "Folders can't be installed on this device. Choose the mod's .wwhdmod or ZIP file instead.";
inline constexpr const char* kDocumentUnsupported = "This file can't be opened here. Choose a file stored on the device.";

// False (and why) when the pick can't be installed. Empty: nothing was picked (false, no error).
inline bool resolve(const std::string& picked, const CopyDocument& copy, Local& out, std::string& error) {
    out = {};
    error.clear();
    switch (classify(picked)) {
    case Kind::Empty: return false;
    case Kind::Path: out.path = picked; return true;
    case Kind::Folder: error = kFolderUnsupported; return false;
    case Kind::Document:
        if (!copy) { error = kDocumentUnsupported; return false; }
        try {
            out.path = copy(picked);
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        if (out.path.empty()) { error = kDocumentUnsupported; return false; }
        out.temporary = true;
        return true;
    }
    return false;
}

// Removes a temporary copy and the folder made for it (when that is empty then). Paths are left alone.
inline void release(const Local& local) {
    if (!local.temporary || local.path.empty()) return;
    std::error_code ignored;
    std::filesystem::path file(local.path);
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(file.parent_path(), ignored);  // only an empty folder goes
}

}  // namespace mods::picked

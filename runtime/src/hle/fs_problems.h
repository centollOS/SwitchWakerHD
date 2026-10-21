// (this fork, issue #47) Game files the game cannot find or open, logged once per path: the game's assert after a
// missing file (#46: d_s_room.cpp "fileList != (0)") does not say which file it was. Saves and writes are left out.
// Called from hle/fs.cpp (two marked hooks).
#pragma once
#include <functional>
#include <string>

namespace fs_problems {
// a failed FSGetStat (any guest thread). read_path: the host path fs.cpp reads for a guest path (content mods and
// the language pack included), called only for a line that is logged
void stat_missed(const std::string& gpath, int err, const std::function<std::string(const std::string&)>& read_path);
// a failed FSOpenFile of gpath at the host path hp
void open_failed(const std::string& gpath, const std::string& mode, int err, const std::string& hp);
}  // namespace fs_problems

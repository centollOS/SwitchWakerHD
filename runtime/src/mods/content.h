#pragma once
#include <filesystem>
#include <map>
#include <string>
namespace mods::content {
using Files=std::map<std::string,std::string>;
// Validate a replacement tree. Keys follow Wii U case-insensitive path semantics.
bool known_pack(const std::string& filename);
void import_legacy(const std::filesystem::path& stage,const std::string& source_name);
Files index(const std::filesystem::path& directory);
// Called once before guest execution. Never swap resources during a session.
void activate(Files files);
std::string replacement(const std::string& guest, const std::string& mode="rb");
}

#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace mods::archive {
// the largest package file stage() takes (and the most its files may expand to)
inline constexpr uint64_t kArchiveLimit=512ull*1024*1024;
void stage(const std::filesystem::path& source,const std::filesystem::path& destination);
bool relative_path(const std::string& path);
}

#pragma once
#include <atomic>
#include <optional>
#include <cstdlib>
#include "platform/host.h"
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
namespace gfxvk {
inline std::optional<std::string> path_environment_utf8(const char* name) {
#ifdef _WIN32
  std::wstring wideName;
  for(const char* c=name;*c;++c) wideName.push_back(wchar_t(*c));
  if(!GetEnvironmentVariableW(wideName.c_str(),nullptr,0)) return std::nullopt;
  return host::environment_utf8(wideName.c_str());
#else
  if(const char* value=std::getenv(name)) return std::string(value);
  return std::nullopt;
#endif
}
inline FILE* open_utf8_file(const std::string& path, const char* mode) {
#ifdef _WIN32
  auto native=std::filesystem::u8path(path).wstring();
  std::wstring wideMode;
  for(const char* c=mode;*c;++c) wideMode.push_back(wchar_t(*c));
  return _wfopen(native.c_str(),wideMode.c_str());
#else
  return std::fopen(path.c_str(),mode);
#endif
}
// Staged files are named <destination>.t<8 hex> (issue #144: short, so cache paths stay far below
// Windows' MAX_PATH; releases up to v0.2.12 used <destination>.tmp-<pid>-<clock>-<n>).
inline bool atomic_temporary_suffix(std::u8string_view suffix) {
  auto hex=[](char8_t c) { return (c>=u8'0'&&c<=u8'9')||(c>=u8'a'&&c<=u8'f'); };
  if(suffix.size()==10&&suffix.starts_with(u8".t")) {
    for(auto c:suffix.substr(2)) if(!hex(c)) return false;
    return true;
  }
  // The earlier form: exactly decimal PID-clock-sequence, no extension.
  if(!suffix.starts_with(u8".tmp-")) return false;
  unsigned fields=1;bool digit=false;
  for(auto c:suffix.substr(5)) {
    if(c>=u8'0'&&c<=u8'9') { digit=true; continue; }
    if(c!=u8'-'||!digit||fields==3) return false;
    ++fields;digit=false;
  }
  return digit&&fields==3;
}
// Remove only this destination's own uniquely named, regular staged files.
// Best effort: permissions/clock/filesystem errors cannot fail a cache write.
inline void remove_stale_atomic_temporaries(const std::string& destination) noexcept {
  try {
    const auto target=std::filesystem::u8path(destination);
    auto directory=target.parent_path();
    if(directory.empty()) directory=".";
    const auto prefix=target.filename().u8string();
    std::error_code error;
    std::filesystem::directory_iterator entries(directory,error), end;
    const auto cutoff=std::filesystem::file_time_type::clock::now()-std::chrono::hours(24);
    for(;!error && entries!=end;entries.increment(error)) {
      const auto name=entries->path().filename().u8string();
      if(!name.starts_with(prefix)||!atomic_temporary_suffix(std::u8string_view(name).substr(prefix.size()))) continue;
      std::error_code itemError;
      if(entries->symlink_status(itemError).type()!=std::filesystem::file_type::regular||itemError) continue;
      const auto written=entries->last_write_time(itemError);
      if(!itemError && written<cutoff) std::filesystem::remove(entries->path(),itemError);
    }
  } catch(...) {}
}
// Exclusive creation: concurrent processes never truncate each other's staged
// bytes. Replacement remains an atomic operation on the destination filesystem.
inline FILE* open_atomic_temporary(const std::string& destination, std::string& path) {
  remove_stale_atomic_temporaries(destination);
  static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
  const auto pid = _getpid();
#else
  const auto pid = getpid();
#endif
  const auto epoch = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  for (unsigned attempt=0; attempt<64; ++attempt) {
    // process, time and sequence mixed into 32 bits; a collision only means another attempt
    uint64_t x = (uint64_t(uint32_t(pid))<<32) ^ epoch ^ (sequence.fetch_add(1)*0x9e3779b97f4a7c15ull);
    x ^= x>>30; x *= 0xbf58476d1ce4e5b9ull; x ^= x>>27; x *= 0x94d049bb133111ebull; x ^= x>>31;
    char suffix[16];
    std::snprintf(suffix,sizeof suffix,".t%08x",unsigned(uint32_t(x)));
    path = destination + suffix;
    FILE* file = open_utf8_file(path,"wbx");
    if (file) return file;
    if (errno != EEXIST) break;
  }
  throw std::runtime_error("cannot create unique atomic output");
}
}

#pragma once
#include <cstdio>
#include "atomic_temporary.h"
#include <cstdint>
#include <string>
#include <filesystem>
#include <span>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace gfxvk {
// Access under the recipe-map mutex. Completing an older in-flight snapshot
// must not clear modifications made while its serialization or I/O ran.
struct RecipeSaveRevision {
  uint64_t current = 0, saved = 0;
  void changed() { ++current; }
  bool dirty() const { return current != saved; }
  void complete(uint64_t snapshot) { saved = snapshot; }
};
// Independent staged files per writer. The last complete database survives allocation,
// write, sync or rename failure; never expose a partially written recipe file.
template<class Replace>
void atomic_recipe_write(const std::string& path, std::span<const uint8_t> bytes, Replace replace) {
  std::string temporary;
  FILE* file = nullptr;
  try {
    auto parent = std::filesystem::u8path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    file = open_atomic_temporary(path, temporary);
    if (!file) throw std::runtime_error("cannot open recipe output");
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() || std::fflush(file))
      throw std::runtime_error("recipe write failed");
#ifdef _WIN32
    if (_commit(_fileno(file))) throw std::runtime_error("recipe sync failed");
#else
    if (fsync(fileno(file))) throw std::runtime_error("recipe sync failed");
#endif
    const int closed = std::fclose(file); file = nullptr;
    if (closed || !replace(temporary, path)) throw std::runtime_error("recipe replace failed");
  } catch (...) {
    if (file) std::fclose(file);
    std::error_code ignored; std::filesystem::remove(std::filesystem::u8path(temporary), ignored);
    throw;
  }
}
}

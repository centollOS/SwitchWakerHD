#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
namespace gfxvk {
// vk-<12 hex>.bin: vendor, device and the driver's pipeline cache UUID hashed into 48 bits (issue #144:
// short, so the cache and its recipe and staging files stay far below Windows' MAX_PATH). Releases up
// to v0.2.12 named it vulkan-<vendor>-<device>-<uuid>.bin (legacy_device_pipeline_cache_filename);
// adopt_legacy_pipeline_cache renames such a cache once.
inline std::string device_pipeline_cache_filename(uint32_t vendor,uint32_t device,
    const std::array<uint8_t,16>& uuid) {
  uint64_t h=0xcbf29ce484222325ull;  // FNV-1a 64
  auto mix=[&](uint8_t byte) { h^=byte; h*=0x100000001b3ull; };
  for(unsigned i=0;i<4;++i) mix(uint8_t(vendor>>(i*8)));
  for(unsigned i=0;i<4;++i) mix(uint8_t(device>>(i*8)));
  for(auto byte:uuid) mix(byte);
  char name[32];std::snprintf(name,sizeof name,"vk-%012llx.bin",static_cast<unsigned long long>(h>>16));
  return name;
}
inline std::string legacy_device_pipeline_cache_filename(uint32_t vendor,uint32_t device,
    const std::array<uint8_t,16>& uuid) {
  char prefix[32];std::snprintf(prefix,sizeof prefix,"vulkan-%08x-%08x-",vendor,device);
  std::string name(prefix);
  constexpr char hex[]="0123456789abcdef";
  for(auto byte:uuid) { name+=hex[byte>>4];name+=hex[byte&15]; }
  return name+".bin";
}
// The cache under its earlier name becomes the cache under the current one (with its .recipes file),
// unless a cache under the current name exists already. Never fails: a cache that cannot be renamed is rebuilt.
inline void adopt_legacy_pipeline_cache(const std::string& path,const std::string& legacy) noexcept {
  try {
    namespace fs=std::filesystem;
    if(path.empty()||legacy.empty()||path==legacy) return;
    std::error_code error;
    const auto current=fs::u8path(path), old=fs::u8path(legacy);
    if(fs::exists(current,error)||!fs::is_regular_file(old,error)) return;
    auto name=[](const fs::path& p) { auto u=p.filename().u8string(); return std::string(u.begin(),u.end()); };
    for(const char* extension:{"",".recipes"}) {
      auto from=old, to=current;
      from+=extension; to+=extension;
      if(*extension&&(!fs::is_regular_file(from,error)||fs::exists(to,error))) continue;
      fs::rename(from,to,error);
      if(error) {
        std::fprintf(stderr,"[vulkan] cannot rename the pipeline cache %s to %s (%s); it is rebuilt\n",
                     name(from).c_str(),name(to).c_str(),error.message().c_str());
        return;
      }
      std::fprintf(stderr,"[vulkan] pipeline cache %s is now %s\n",name(from).c_str(),name(to).c_str());
    }
  } catch(...) {}
}
// A present empty result disables caching; nullopt leaves existing defaults.
// Explicit file overrides always win over directory selection, including "0".
inline std::optional<std::string> pipeline_cache_override(
    const std::optional<std::string>& file,const std::optional<std::string>& directory,
    uint32_t vendor,uint32_t device,const std::array<uint8_t,16>& uuid,bool legacy=false) {
  if(file) return *file=="0" ? std::string{} : *file;
  if(!directory) return std::nullopt;
  if(directory->empty()||*directory=="0") return std::string{};
  std::string path=*directory;
  if(path.back()!='/'&&path.back()!='\\')path+='/';
  return path+(legacy ? legacy_device_pipeline_cache_filename(vendor,device,uuid)
                      : device_pipeline_cache_filename(vendor,device,uuid));
}
}

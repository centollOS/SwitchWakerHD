#include "gfx/vulkan/pipeline_identity.h"
#include "gfx/vulkan/pipeline_cache_path.h"
#include <cassert>
#include <memory>
#include <unordered_map>
#include <array>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
struct Shader {
  uint64_t pipelineId; bool vertex; int output; bool valid = true;
  bool ready() const { return valid; }
};
int main() {
  const std::array<uint8_t,16> uuid{};
  auto secondUuid=uuid;secondUuid.back()=0xff;
  const auto first=gfxvk::pipeline_cache_override({},"live",0x1002,0x1234,uuid);
  const auto second=gfxvk::pipeline_cache_override({},"live",0x10de,0x5678,uuid);
  assert(first&&second&&first!=second);
  // vk-<12 hex>.bin (issue #144), and the name of releases up to v0.2.12 for adopting their caches
  assert(first->size()==std::string("live/vk-0123456789ab.bin").size() && first->starts_with("live/vk-"));
  assert(*gfxvk::pipeline_cache_override({},"live",0x1002,0x1234,uuid,true)==
         "live/vulkan-00001002-00001234-00000000000000000000000000000000.bin");
  {
    namespace fs=std::filesystem;
    const auto dir=fs::temp_directory_path()/("wwhd-pipeline-adopt-"+std::to_string(std::rand()));
    fs::create_directories(dir);
    auto put=[](const fs::path& p,const char* text) { std::ofstream(p)<<text; };
    auto get=[](const fs::path& p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f),{}); };
    const auto current=*gfxvk::pipeline_cache_override({},dir.string(),0x1002,0x1234,uuid);
    const auto legacy=*gfxvk::pipeline_cache_override({},dir.string(),0x1002,0x1234,uuid,true);
    put(legacy,"cache");put(legacy+".recipes","recipes");
    gfxvk::adopt_legacy_pipeline_cache(current,legacy);
    assert(get(current)=="cache"&&get(current+".recipes")=="recipes"&&!fs::exists(legacy));
    // a cache under the current name wins; the earlier one is left alone
    put(legacy,"older");
    gfxvk::adopt_legacy_pipeline_cache(current,legacy);
    assert(get(current)=="cache"&&get(legacy)=="older");
    // nothing to adopt, or no cache: no change, no failure
    gfxvk::adopt_legacy_pipeline_cache((dir/"none.bin").string(),(dir/"missing.bin").string());
    gfxvk::adopt_legacy_pipeline_cache("","");
    assert(!fs::exists(dir/"none.bin"));
    fs::remove_all(dir);
  }
  assert(gfxvk::pipeline_cache_override({},"live/",0x1002,0x1234,uuid)==first);
  assert(gfxvk::pipeline_cache_override({},"live",0x1002,0x1234,secondUuid)!=first);
  assert(gfxvk::pipeline_cache_override("exact.bin","live",1,2,uuid)=="exact.bin");
  assert(gfxvk::pipeline_cache_override("0","live",1,2,uuid)=="");
  assert(!gfxvk::pipeline_cache_override({},{},1,2,uuid));

  std::unordered_map<uint64_t, std::unique_ptr<Shader>> canonical;
  std::unordered_map<uint64_t, Shader*> variants;
  auto equal = [](const Shader& a, const Shader& b) { return a.output == b.output; };
  auto add = [&](uint64_t id, bool vertex, int output) {
    auto shader = std::make_unique<Shader>(Shader{id, vertex, output});
    auto* ptr = shader.get(); canonical.emplace(id, std::move(shader)); variants.emplace(id, ptr); return ptr;
  };
  // Offline replay encounters equivalent variant 20 first; live recording had
  // encountered 10 first. The translator retains the exact proven alias 10.
  auto* offline = add(20, true, 7); variants.emplace(10, offline);
  assert(gfxvk::resolve_pipeline_shader_identity(10, true, canonical, variants, equal) == offline);
  assert(offline->pipelineId == 20); // worker must publish under current canonical identity
  assert(!gfxvk::resolve_pipeline_shader_identity(10, false, canonical, variants, equal));
  assert(!gfxvk::resolve_pipeline_shader_identity(30, true, canonical, variants, equal));
  // Equal descriptor layouts alone are never consulted; an actual colliding
  // canonical identity with different output makes the recorded key unusable.
  add(10, true, 99);
  assert(!gfxvk::resolve_pipeline_shader_identity(10, true, canonical, variants, equal));
  canonical.erase(10); offline->valid = false;
  assert(!gfxvk::resolve_pipeline_shader_identity(10, true, canonical, variants, equal));
  auto content=[](bool stage, uint32_t kind, std::string_view source, uint32_t metadata) {
    gfxvk::PortableShaderIdentity id(stage,kind,source); id.word(metadata); return id.value();
  };
  const auto stable=content(true,3,"synthetic GLSL",7);
  assert(stable==content(true,3,"synthetic GLSL",7));
  assert(stable!=content(false,3,"synthetic GLSL",7));
  assert(stable!=content(true,4,"synthetic GLSL",7));
  assert(stable!=content(true,3,"changed GLSL",7));
  assert(stable!=content(true,3,"synthetic GLSL",8));
  // Neither canonical variant key/replay order nor SPIR-V readiness participates.
  assert(stable!=uint64_t(10) && stable!=uint64_t(20));
  struct Binding { uint32_t binding,descriptorType,descriptorCount,stageFlags; const void* pImmutableSamplers=nullptr; };
  struct Recipe {
    std::array<std::vector<uint32_t>,2> spirv;
    std::array<std::vector<Binding>,2> descriptors;
    bool dynamicUniforms=false;
  };
  Recipe recipe{{std::vector<uint32_t>{1,2}, std::vector<uint32_t>{3,4}},
    {std::vector<Binding>{{0,6,1,1}}, std::vector<Binding>{{0,6,1,16}}},false};
  auto words=recipe.spirv; auto descriptors=recipe.descriptors;
  assert(gfxvk::portable_recipe_matches(recipe,words,descriptors,false));
  // Force equal portable-key selection with different actual content: production
  // take_warm_pipeline uses this exact helper before transferring native handles.
  words[0][1]=99;
  assert(!gfxvk::portable_recipe_matches(recipe,words,descriptors,false));
  words=recipe.spirv; descriptors[1][0].binding=1;
  assert(!gfxvk::portable_recipe_matches(recipe,words,descriptors,false));
  descriptors=recipe.descriptors;
  assert(!gfxvk::portable_recipe_matches(recipe,words,descriptors,true));
  descriptors[0][0].pImmutableSamplers=&recipe;
  assert(!gfxvk::portable_recipe_matches(recipe,words,descriptors,false));

}

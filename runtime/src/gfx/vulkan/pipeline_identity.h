#pragma once
#include <cstdint>

namespace gfxvk {
// Canonical output deduplication depends on replay order. A recorded variant
// identity may resolve only through the normal translator's proven alias map;
// never infer equivalence from program hashes or resource layouts alone.
template<class CanonicalMap, class VariantMap, class Equivalent>
auto resolve_pipeline_shader_identity(uint64_t id, bool vertex,
    const CanonicalMap& canonical, const VariantMap& variants, Equivalent equivalent)
    -> decltype(variants.begin()->second) {
  using Pointer = decltype(variants.begin()->second);
  Pointer found = nullptr;
  if (auto it = variants.find(id); it != variants.end() &&
      it->second->vertex == vertex && it->second->ready()) found = it->second;
  for (const auto& [key, shader] : canonical) {
    if (shader->pipelineId != id || shader->vertex != vertex || !shader->ready()) continue;
    if (found && !equivalent(*found, *shader)) return nullptr;
    found = shader.get();
  }
  return found;
}
}

#include <string_view>
namespace gfxvk {
// Versioned canonical little-endian scalars, with source length framing. No
// native struct padding or SPIR-V publication timing enters portable identity.
class PortableShaderIdentity {
  uint64_t hash_ = 14695981039346656037ull;
  void byte(uint8_t value) { hash_ = (hash_ ^ value) * 1099511628211ull; }
public:
  PortableShaderIdentity(bool vertex, uint32_t kind, std::string_view source) {
    word(2); word(vertex ? 1 : 0); word(kind);
    word(uint32_t(source.size())); word(uint32_t(uint64_t(source.size()) >> 32));
    for (unsigned char value : source) byte(value);
  }
  void word(uint32_t value) { for (int i=0;i<4;++i) byte(uint8_t(value >> (8*i))); }
  template<class Range> void words(const Range& values) { for(auto value : values) word(uint32_t(value)); }
  uint64_t value() const { return hash_; }
};

template<class Recipe, class Words, class Descriptors>
bool portable_recipe_matches(const Recipe& recipe, const Words& words,
    const Descriptors& descriptors, bool dynamicUniforms) {
  if (recipe.dynamicUniforms != dynamicUniforms) return false;
  for(size_t stage=0;stage<2;++stage) {
    if constexpr (requires { recipe.words(stage); }) { if(recipe.words(stage)!=words[stage]) return false; }
    else if(recipe.spirv[stage]!=words[stage]) return false;
  }
  for (size_t stage=0;stage<2;++stage) {
    const auto& a=recipe.descriptors[stage]; const auto& b=descriptors[stage];
    if (a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i)
      if(a[i].binding!=b[i].binding || a[i].descriptorType!=b[i].descriptorType ||
          a[i].descriptorCount!=b[i].descriptorCount || a[i].stageFlags!=b[i].stageFlags ||
          a[i].pImmutableSamplers || b[i].pImmutableSamplers) return false;
  }
  return true;
}
}

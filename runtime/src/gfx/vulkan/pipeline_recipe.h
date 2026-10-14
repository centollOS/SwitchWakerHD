#pragma once
#include "pipeline_state.h"
#include <array>
#include <memory>
#include <unordered_map>

namespace gfxvk {
inline constexpr size_t kPipelineRecipeMaxBytes = 256 * 1024 * 1024;
inline constexpr size_t kPipelineRecipeMaxCount = 16384;
// Device/driver/features and schema compatibility are checked before a recipe
// reaches Vulkan. Neither Vulkan handles nor host pointers are serialized.
using PipelineRecipeFingerprint = std::array<uint8_t, 48>;
struct PipelineRecipe {
  std::vector<uint8_t> key;
  std::array<std::vector<uint32_t>, 2> spirv;
  std::array<std::vector<VkDescriptorSetLayoutBinding>, 2> descriptors;
  std::unique_ptr<GraphicsPipelineState> state;
  bool dynamicUniforms = false;
  std::array<std::shared_ptr<const std::vector<uint32_t>>,2> sharedSpirv;
  const std::vector<uint32_t>& words(size_t stage) const {
    return sharedSpirv[stage] ? *sharedSpirv[stage] : spirv[stage];
  }
  std::array<std::vector<uint32_t>,2> shader_words() const { return {words(0),words(1)}; }
};
// Hash collisions are checked by full word equality, never by digest alone.
class RecipeShaderPool {
  std::unordered_multimap<uint64_t,std::weak_ptr<const std::vector<uint32_t>>> entries;
public:
  void freeze(PipelineRecipe& recipe) {
    if(entries.size()>32768) std::erase_if(entries,[](const auto& entry){ return entry.second.expired(); });
    for(size_t stage=0;stage<2;++stage) {
      if(recipe.sharedSpirv[stage]) continue; // Published recipes remain immutable.
      const auto& words = recipe.words(stage);
      if(words.empty()) continue;
      uint64_t hash=1469598103934665603ull;
      for(auto word:words) hash=(hash ^ word)*1099511628211ull;
      std::shared_ptr<const std::vector<uint32_t>> shared;
      auto [first,last]=entries.equal_range(hash);
      for(auto it=first;it!=last;) {
        auto candidate=it->second.lock();
        if(!candidate) { it=entries.erase(it); continue; }
        if(*candidate==words) { shared=std::move(candidate); break; }
        ++it;
      }
      if(!shared) {
        shared=recipe.sharedSpirv[stage] ? recipe.sharedSpirv[stage] :
            std::make_shared<const std::vector<uint32_t>>(std::move(recipe.spirv[stage]));
        entries.emplace(hash,shared);
      }
      recipe.sharedSpirv[stage]=std::move(shared);
      std::vector<uint32_t>().swap(recipe.spirv[stage]);
    }
  }

};
// Strict portable state only: shader words must both be empty. Resolve locally before Vulkan.
std::shared_ptr<PipelineRecipe> decode_code_free_pipeline_recipe(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encode_pipeline_fixed_state(const GraphicsPipelineState& state);
std::vector<uint8_t> encode_pipeline_recipe(PipelineRecipe& recipe);
std::shared_ptr<PipelineRecipe> decode_pipeline_recipe(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encode_pipeline_recipes(const PipelineRecipeFingerprint& fingerprint,
    const std::vector<std::shared_ptr<PipelineRecipe>>& recipes);
std::vector<std::shared_ptr<PipelineRecipe>> decode_pipeline_recipes(
    const PipelineRecipeFingerprint& fingerprint, const std::vector<uint8_t>& bytes);
}

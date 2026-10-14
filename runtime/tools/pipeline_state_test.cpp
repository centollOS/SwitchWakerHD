#include "pipeline_state_fixture.h"
#include <cassert>
#include <thread>

using gfxvk::GraphicsPipelineState;
using pipeline_test::state;
static void check(GraphicsPipelineState& snapshot) {
  const auto* ci = snapshot.get();
  const auto* rc = static_cast<const VkPipelineRenderingCreateInfo*>(ci->pNext);
  assert(rc->colorAttachmentCount == 1 && rc->pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM);
  assert(ci->pVertexInputState->pVertexBindingDescriptions[0].stride == 24);
  assert(ci->pVertexInputState->pVertexAttributeDescriptions[0].offset == 8);
  assert(ci->pInputAssemblyState->primitiveRestartEnable);
  assert(ci->pRasterizationState->depthBiasConstantFactor == 2.5f);
  assert(ci->pDepthStencilState->front.compareMask == 0x57);
  assert(ci->pColorBlendState->pAttachments[0].colorWriteMask == 5);
  assert(ci->pDynamicState->pDynamicStates[0] == VK_DYNAMIC_STATE_VIEWPORT);
  assert(std::string_view(ci->pStages[0].pName) == "main");
}
int main() {
  auto original = state();
  check(original);
  auto copy = original;
  auto moved = std::move(copy);
  // All source stack arrays are already dead. Destroy the original's vectors
  // before the moved snapshot is consumed on a different thread.
  original = state();
  std::thread worker([snapshot = std::move(moved)]() mutable { check(snapshot); });
  worker.join();
  // Utility pipelines can omit depth/dynamic state and use static arrays.
  // Mutate the originals before consuming a copied snapshot on another thread.
  auto fixture = state();
  auto ci = *fixture.get();
  auto vp = *ci.pViewportState;
  auto ms = *ci.pMultisampleState;
  VkViewport staticViewport{1, 2, 640, 480, 0, 1};
  VkRect2D staticScissor{{3, 4}, {320, 240}};
  VkSampleMask masks[2] = {0x12345678, 0x87654321};
  vp.pViewports = &staticViewport; vp.pScissors = &staticScissor;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT; ms.pSampleMask = masks;
  ci.pViewportState = &vp; ci.pMultisampleState = &ms;
  ci.pDepthStencilState = nullptr; ci.pDynamicState = nullptr;
  GraphicsPipelineState utility(ci);
  staticViewport.width = 0; staticScissor.extent.width = 0; masks[1] = 0;
  std::thread utilityWorker([snapshot = utility]() mutable {
    const auto* owned = snapshot.get();
    assert(!owned->pDepthStencilState && !owned->pDynamicState);
    assert(owned->pViewportState->pViewports[0].width == 640);
    assert(owned->pViewportState->pScissors[0].extent.width == 320);
    assert(owned->pMultisampleState->pSampleMask[1] == 0x87654321);
  });
  utilityWorker.join();
  // Classic pass snapshots retain borrowed compatibility handles and owned
  // portable target formats without exposing dynamic-rendering pNext to Vulkan.
  auto targets = fixture.rendering_targets();
  ci = *fixture.get(); ci.pNext = nullptr;
  ci.renderPass = reinterpret_cast<VkRenderPass>(uintptr_t(123));
  GraphicsPipelineState classic(ci, &targets);
  auto classicCopy = classic;
  assert(classicCopy.get()->renderPass == ci.renderPass && !classicCopy.get()->pNext);
  assert(classicCopy.rendering_targets().pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM);
  GraphicsPipelineState classicUtility(ci);
  assert(classicUtility.get()->renderPass == ci.renderPass && !classicUtility.get()->pNext);
  VkGraphicsPipelineCreateInfo invalid{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  bool rejected = false;
  try { GraphicsPipelineState unsupported(invalid); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
}

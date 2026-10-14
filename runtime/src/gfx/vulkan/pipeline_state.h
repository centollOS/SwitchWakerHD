#pragma once
#include "loader.h"
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gfxvk {
// Owns the CPU-side state consumed by vkCreateGraphicsPipelines. Vulkan handles
// (layout/modules) remain borrowed: the eventual job owner must retain those
// objects until creation finishes, and join jobs before device destruction.
// Accepts guest and utility states for dynamic rendering or compatible render passes. Unknown extension
// chains and specialization data are rejected rather than borrowed.
class GraphicsPipelineState {
  VkGraphicsPipelineCreateInfo info;
  VkPipelineRenderingCreateInfo rendering{};
  VkPipelineVertexInputStateCreateInfo vertex;
  VkPipelineInputAssemblyStateCreateInfo assembly;
  VkPipelineViewportStateCreateInfo viewport;
  VkPipelineRasterizationStateCreateInfo raster;
  VkPipelineMultisampleStateCreateInfo multisample;
  VkPipelineDepthStencilStateCreateInfo depth;
  VkPipelineColorBlendStateCreateInfo blend;
  VkPipelineDynamicStateCreateInfo dynamic;
  std::vector<VkPipelineShaderStageCreateInfo> stages;
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  std::vector<VkPipelineColorBlendAttachmentState> attachments;
  std::vector<VkDynamicState> dynamics;
  std::vector<VkFormat> formats;
  std::vector<VkViewport> viewports;
  std::vector<VkRect2D> scissors;
  std::vector<VkSampleMask> sampleMasks;
  bool hasDepth = false, hasDynamic = false;

  template<class T> static std::vector<T> copy(const T* data, uint32_t count) {
    if (!count) return {};
    if (!data) throw std::runtime_error("missing pipeline state array");
    return {data, data + count};
  }
public:
  // Mutate only the owned stage array; module lifetime belongs to the job owner.
  void set_stage_module(uint32_t index, VkShaderModule module) {
    if(index >= stages.size()) throw std::out_of_range("pipeline shader stage index");
    if(!module) throw std::invalid_argument("null pipeline shader module");
    stages[index].module = module;
  }
  explicit GraphicsPipelineState(const VkGraphicsPipelineCreateInfo& source,
      const VkPipelineRenderingCreateInfo* targetMetadata = nullptr)
      : info(source) {
    if (source.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        (!source.pNext && !source.renderPass) || !source.pVertexInputState || !source.pInputAssemblyState ||
        !source.pViewportState || !source.pRasterizationState ||
        !source.pMultisampleState ||
        !source.pColorBlendState || source.pTessellationState)
      throw std::runtime_error("unsupported pipeline state snapshot");
    if (source.pNext && static_cast<const VkBaseInStructure*>(source.pNext)->sType !=
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
      throw std::runtime_error("unsupported pipeline snapshot extension chain");
    rendering = source.pNext ? *static_cast<const VkPipelineRenderingCreateInfo*>(source.pNext) :
        targetMetadata ? *targetMetadata : VkPipelineRenderingCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    vertex = *source.pVertexInputState;
    assembly = *source.pInputAssemblyState;
    viewport = *source.pViewportState;
    raster = *source.pRasterizationState;
    multisample = *source.pMultisampleState;
    hasDepth = source.pDepthStencilState != nullptr;
    depth = hasDepth ? *source.pDepthStencilState : VkPipelineDepthStencilStateCreateInfo{};
    blend = *source.pColorBlendState;
    hasDynamic = source.pDynamicState != nullptr;
    dynamic = hasDynamic ? *source.pDynamicState : VkPipelineDynamicStateCreateInfo{};
    if (rendering.sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
        rendering.pNext || vertex.pNext || assembly.pNext || viewport.pNext ||
        raster.pNext || multisample.pNext || depth.pNext || blend.pNext || dynamic.pNext)
      throw std::runtime_error("unsupported pipeline snapshot extension");
    if (viewport.pViewports) viewports = copy(viewport.pViewports, viewport.viewportCount);
    if (viewport.pScissors) scissors = copy(viewport.pScissors, viewport.scissorCount);
    if (multisample.pSampleMask) {
      const auto samples = uint32_t(multisample.rasterizationSamples);
      if (!samples || samples > 64 || (samples & (samples - 1)))
        throw std::runtime_error("invalid pipeline sample count");
      sampleMasks = copy(multisample.pSampleMask, (samples + 31) / 32);
    }
    stages = copy(source.pStages, source.stageCount);
    for (auto& stage : stages) {
      if (stage.pNext || stage.pSpecializationInfo || !stage.pName ||
          std::string_view(stage.pName) != "main")
        throw std::runtime_error("unsupported pipeline shader snapshot");
      stage.pName = "main";
    }
    bindings = copy(vertex.pVertexBindingDescriptions, vertex.vertexBindingDescriptionCount);
    attributes = copy(vertex.pVertexAttributeDescriptions, vertex.vertexAttributeDescriptionCount);
    attachments = copy(blend.pAttachments, blend.attachmentCount);
    dynamics = copy(dynamic.pDynamicStates, dynamic.dynamicStateCount);
    formats = copy(rendering.pColorAttachmentFormats, rendering.colorAttachmentCount);
  }

  // Portable recipes retain formats, never process-local render-pass handles.
  // Metadata remains available even when the Vulkan create info has no pNext.
  const VkPipelineRenderingCreateInfo& rendering_targets() {
    rendering.pColorAttachmentFormats = formats.data();
    return rendering;
  }

  // Returned pointers are valid until this snapshot is moved or destroyed.
  // Rebind on every use so default copy/move cannot retain pointers into a source.
  const VkGraphicsPipelineCreateInfo* get() {
    rendering.pColorAttachmentFormats = formats.data();
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.pVertexAttributeDescriptions = attributes.data();
    blend.pAttachments = attachments.data();
    dynamic.pDynamicStates = dynamics.data();
    viewport.pViewports = viewports.empty() ? nullptr : viewports.data();
    viewport.pScissors = scissors.empty() ? nullptr : scissors.data();
    multisample.pSampleMask = sampleMasks.empty() ? nullptr : sampleMasks.data();
    info.pNext = info.renderPass ? nullptr : &rendering;
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = hasDepth ? &depth : nullptr;
    info.pColorBlendState = &blend;
    info.pDynamicState = hasDynamic ? &dynamic : nullptr;
    return &info;
  }
};
}

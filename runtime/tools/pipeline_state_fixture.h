#pragma once
#include "gfx/vulkan/pipeline_state.h"
namespace pipeline_test {
inline gfxvk::GraphicsPipelineState state() {
  VkPipelineRenderingCreateInfo rc{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  rc.colorAttachmentCount = 1; rc.pColorAttachmentFormats = &format;
  VkVertexInputBindingDescription binding{3, 24, VK_VERTEX_INPUT_RATE_INSTANCE};
  VkVertexInputAttributeDescription attribute{2, 3, VK_FORMAT_R32G32_SFLOAT, 8};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = vi.vertexAttributeDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding; vi.pVertexAttributeDescriptions = &attribute;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; ia.primitiveRestartEnable = VK_TRUE;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.lineWidth = 1; rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.depthBiasConstantFactor = 2.5f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthWriteEnable = VK_TRUE; ds.front.compareMask = 0x57;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.blendEnable = VK_TRUE; attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  attachment.colorWriteMask = 5;
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  bs.attachmentCount = 1; bs.pAttachments = &attachment;
  VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 4; dy.pDynamicStates = dynamic;
  VkPipelineShaderStageCreateInfo stages[2]{};
  for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rc; ci.stageCount = 2; ci.pStages = stages;
  ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
  ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &bs; ci.pDynamicState = &dy;
  return gfxvk::GraphicsPipelineState(ci);
}

}

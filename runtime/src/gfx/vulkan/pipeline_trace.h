#pragma once
#include "loader.h"
#include "stutter_trace.h"
#include "pipeline_demand.h"

namespace gfxvk::stutter {
inline VkResult create_graphics(uint64_t frame, const char* kind,
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines,
    const std::function<void()>& presentWhilePending = {}) {
  if (async_pipeline_enabled()) {
    // All audited utility sites create one pipeline, using the default allocator.
    // Reject unsupported batches instead of silently reverting to render-thread
    // compilation. Handle lifetimes are retained by the caller until return.
    if (count != 1 || allocator || !info || !pipelines)
      throw std::runtime_error("unsupported asynchronous utility pipeline request");
    auto result = build_pipeline_async(GraphicsPipelineState(*info), frame, 0,
                                      kind, presentWhilePending);
    *pipelines = result.pipeline;
    return result.result;
  }
  const auto started = trace().enabled() ? now() : 0;
  auto result = vkCreateGraphicsPipelines(device, cache, count, info, allocator, pipelines);
  if (started) trace().add(kind, frame, 0, started, now(), int(result));
  return result;
}
}

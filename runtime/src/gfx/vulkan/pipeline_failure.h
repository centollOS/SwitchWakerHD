#pragma once
#include "loader.h"
namespace gfxvk {
// Preserve devel's driver-workaround policy. A failed pipeline is terminal,
// never pending; allocation exhaustion and device loss remain fatal.
inline bool recoverable_pipeline_failure(VkResult result) {
  return result != VK_SUCCESS && result != VK_ERROR_OUT_OF_HOST_MEMORY &&
      result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_DEVICE_LOST;
}
}

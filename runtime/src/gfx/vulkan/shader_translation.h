// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <functional>
#include <future>
namespace gfxvk::vk {
// The callback must own every input/output lifetime and must not recursively
// submit or shut down this worker. No guest/cache/render state is captured here.
// Accepted jobs always complete exactly once, even if their future is discarded.
// Admission throws before acceptance for an empty task, eight queued jobs, or
// concurrent shutdown. The returned future propagates callback exceptions.
std::future<void> submit_shader_translation(std::function<void()> ownedTask);
bool shader_translation_worker_active();
// Drain accepted jobs and join before shader/cache inputs are destroyed. Submit
// may restart afterward. Call before finishing the downstream SPIR-V compiler.
void finish_shader_translation_worker();
}

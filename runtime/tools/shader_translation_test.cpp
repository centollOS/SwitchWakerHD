// SPDX-License-Identifier: MPL-2.0
#include "gfx/vulkan/shader_translation.h"
#include <atomic>
#include <cassert>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace gfxvk::vk;
int main() {
  const auto caller=std::this_thread::get_id();
  std::promise<void> started, release;
  auto gate=release.get_future().share();std::atomic<int> calls{0};
  auto first=submit_shader_translation([&]{assert(std::this_thread::get_id()!=caller);started.set_value();gate.wait();++calls;});
  started.get_future().get();assert(shader_translation_worker_active());
  std::vector<std::future<void>> queued;
  for(int i=0;i<8;++i)queued.push_back(submit_shader_translation([&]{++calls;}));
  bool rejected=false;try {submit_shader_translation([&]{calls+=100;});}catch(const std::runtime_error&){rejected=true;}
  assert(rejected);release.set_value();finish_shader_translation_worker();
  first.get();for(auto& ticket:queued)ticket.get();assert(calls==9 && !shader_translation_worker_active());
  // Restart and terminal exception propagation, followed by a successful job.
  auto failed=submit_shader_translation([]{throw std::runtime_error("synthetic translation failure");});
  auto success=submit_shader_translation([&]{++calls;});
  bool propagated=false;try{failed.get();}catch(const std::runtime_error&){propagated=true;}
  assert(propagated);success.get();finish_shader_translation_worker();assert(calls==10);
  // Discarded futures do not abandon accepted work or its owned inputs.
  auto owned=std::make_shared<int>(37);std::weak_ptr<int> lifetime=owned;
  submit_shader_translation([owned,&calls]{assert(*owned==37);++calls;});owned.reset();
  finish_shader_translation_worker();assert(lifetime.expired() && calls==11);
  // Queue owns the callback snapshot, independent of subsequent caller edits.
  int source=19;auto snapshot=submit_shader_translation([value=source,&calls]{assert(value==19);++calls;});
  source=99;snapshot.get();finish_shader_translation_worker();assert(calls==12);
  auto recursive=submit_shader_translation([]{submit_shader_translation([]{});});
  bool recursionRejected=false;try{recursive.get();}catch(const std::logic_error&){recursionRejected=true;}
  assert(recursionRejected);finish_shader_translation_worker();
  bool emptyRejected=false;try{submit_shader_translation({});}catch(const std::invalid_argument&){emptyRejected=true;}
  assert(emptyRejected);finish_shader_translation_worker();
}

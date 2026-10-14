// SPDX-License-Identifier: MPL-2.0
#include "shader_translation.h"
#include "platform/host.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace gfxvk::vk {
namespace {
struct Job { std::function<void()> task; std::promise<void> done; };
struct Worker {
  std::mutex mutex, finishMutex;
  std::condition_variable wake;
  std::deque<std::shared_ptr<Job>> queue;
  std::thread thread;
  std::atomic<size_t> active{0};
  bool stopping=false, joining=false;
  void finish() {
    std::lock_guard lifecycle(finishMutex);
    std::thread joiningThread;
    {
      std::lock_guard lock(mutex);
      if(thread.joinable() && thread.get_id()==std::this_thread::get_id())
        throw std::logic_error("translation worker cannot join itself");
      stopping=true; joining=true; joiningThread=std::move(thread);
    }
    wake.notify_all();
    if(joiningThread.joinable())joiningThread.join();
    {std::lock_guard lock(mutex);joining=false;}
  }
  ~Worker(){finish();}
};
Worker& worker(){static Worker value;return value;}
void run(Worker& w) {
  try {host::set_thread_name("Vulkan translate");} catch(...) {}
  for(;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(w.mutex);
      w.wake.wait(lock,[&]{return w.stopping || !w.queue.empty();});
      if(w.queue.empty())return;
      job=std::move(w.queue.front());w.queue.pop_front();
    }
    try {job->task();job->done.set_value();}
    catch(...) {job->done.set_exception(std::current_exception());}
    w.active.fetch_sub(1,std::memory_order_release);
  }
}
}
std::future<void> submit_shader_translation(std::function<void()> task) {
  if(!task)throw std::invalid_argument("empty shader translation task");
  auto job=std::make_shared<Job>();job->task=std::move(task);
  auto future=job->done.get_future();auto& w=worker();
  {
    std::lock_guard lock(w.mutex);
    if(w.joining)throw std::runtime_error("shader translation shutdown in progress");
    if(w.thread.joinable() && w.thread.get_id()==std::this_thread::get_id())
      throw std::logic_error("recursive shader translation submission");
    if(w.queue.size()>=8)throw std::runtime_error("shader translation queue capacity exceeded");
    if(!w.thread.joinable()) {w.stopping=false;w.thread=std::thread(run,std::ref(w));}
    w.queue.push_back(job);w.active.fetch_add(1,std::memory_order_release);
  }
  w.wake.notify_one();return future;
}
bool shader_translation_worker_active(){return worker().active.load(std::memory_order_acquire)!=0;}
void finish_shader_translation_worker(){worker().finish();}
}

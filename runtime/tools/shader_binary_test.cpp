#include "gfx/vulkan/shader_binary.h"
#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace gfxvk::vk;
int main() {
    const std::string source = "#version 450\nvoid main(){gl_Position=vec4(0,0,0,1);}";
    auto reference = compile_shader(source, ShaderStage::Vertex, 1, 1, {}, false);
    auto make = [](const std::string& text, ShaderStage stage) {
        auto ticket = request_shader_compile(text, stage, 1, 2);
        assert(ticket);
        return std::make_shared<ShaderBinary>(std::move(*ticket));
    };
    auto owner = make(source, ShaderStage::Vertex);
    auto retained = owner;
    std::vector<uint32_t> first, second;
    std::thread a([snapshot=owner,&first] { first=snapshot->get().words; });
    std::thread b([snapshot=owner,&second] { second=snapshot->get().words; });
    owner.reset(); // Both workers retain their detached prerequisite.
    a.join();b.join();
    assert(first==reference.words && second==first && !first.empty());
    assert(retained->ready() && !retained->ticket && retained->result);
    assert(!retained->accounted); // CPU consumers never mutate render accounting.
    retained->accounted=true;
    assert(retained->get().words==first && retained->accounted);

    // Deterministic contention: get() holds this same mutex while consuming a
    // pending ticket. ready() must not wait for that critical section to finish.
    auto polling=make(source,ShaderStage::Vertex);
    std::unique_lock held(polling->mutex);
    std::promise<void> started;
    auto nonblocking=std::async(std::launch::async,[&] {
        started.set_value(); return polling->ready();
    });
    started.get_future().wait();
    const bool completed=nonblocking.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    held.unlock();
    assert(completed && !nonblocking.get());
    assert(polling->get().words==reference.words);

    auto failed=make("invalid GLSL",ShaderStage::Fragment);
    for(unsigned i=0;i<2;++i) {
        auto result=failed->get();
        assert(result.words.empty() && !result.error.empty());
    }
    assert(failed->ready() && !failed->ticket && !failed->accounted);
    auto exceptional=make(source,static_cast<ShaderStage>(99));
    for(unsigned i=0;i<2;++i) {
        bool threw=false;
        try { exceptional->get(); } catch(const std::runtime_error&) { threw=true; }
        assert(threw);
    }
    assert(exceptional->ready() && !exceptional->ticket && exceptional->failure);
    finish_shader_compiler();
    assert(!shader_compiler_active());
}

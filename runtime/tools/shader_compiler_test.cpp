#include "gfx/vulkan/shader_compiler.h"
#include <cassert>
#include <thread>
#include <stdexcept>

using namespace gfxvk::vk;
int main() {
    const std::string vertex = "#version 450\nvoid main(){gl_Position=vec4(0,0,0,1);}";
    const auto caller = uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    auto reference = compile_shader(vertex, ShaderStage::Vertex, 1, 2, {}, false);
    auto background = compile_shader(vertex, ShaderStage::Vertex, 1, 2);
    assert(reference.error.empty() && background.error.empty());
    assert(!reference.words.empty() && reference.words == background.words);
    assert(reference.thread == caller && background.thread != caller);
    auto invalid = compile_shader("invalid GLSL", ShaderStage::Fragment, 1, 3);
    assert(invalid.words.empty() && !invalid.error.empty());
    auto compute = compile_shader("#version 450\nlayout(local_size_x=1) in;void main(){}",
                                  ShaderStage::Compute, 1, 4);
    assert(!compute.words.empty() && compute.error.empty() && compute.thread != caller);
    // Source ownership is independent of the caller's string while waiting.
    std::string owned = "#version 450\nvoid main(){vec4 p=vec4(0);";
    for (unsigned i = 0; i < 5000; ++i) owned += "p+=vec4(" + std::to_string(i) + ".0);";
    owned += "gl_Position=p;}";
    const std::string heavy = owned;
    auto expected = compile_shader(owned, ShaderStage::Vertex, 1, 5, {}, false);
    auto copied = compile_shader(owned, ShaderStage::Vertex, 1, 5, [&] { owned = "invalid"; });
    assert(!expected.words.empty() && copied.words == expected.words);
    // An admitted source is copied before the caller mutates its storage.
    std::string queued = vertex;
    auto blocker = request_shader_compile(heavy, ShaderStage::Vertex, 2, 6);
    assert(blocker);
    auto ticket = request_shader_compile(queued, ShaderStage::Vertex, 2, 7);
    assert(ticket);
    queued = "invalid";
    // This job sits behind a substantial real compile. A zero-budget poll must
    // not consume when pending; a fast worker is also a valid ready outcome.
    auto early = ticket->wait_for(0);
    if (early) assert(early->words == reference.words);
    (void)ticket->ready(); (void)ticket->ready();
    auto handedOff = std::move(*ticket);
    std::thread consumer([&] {
        if (early) return;
        auto result = handedOff.consume();
        assert(result.words == reference.words && result.thread != caller);
    });
    consumer.join();
    bool consumedTwice = false;
    try { handedOff.consume(); } catch (const std::logic_error&) { consumedTwice = true; }
    assert(consumedTwice);
    (void)blocker->consume();
    auto failure = request_shader_compile("invalid GLSL", ShaderStage::Fragment, 2, 8);
    assert(failure);
    auto terminal = failure->consume();
    assert(terminal.words.empty() && !terminal.error.empty());
    auto inlineTicket = request_shader_compile(vertex, ShaderStage::Vertex, 2, 9, false);
    assert(inlineTicket && inlineTicket->ready());
    auto inlineResult = inlineTicket->wait_for(0);
    assert(inlineResult && inlineResult->words == reference.words && inlineResult->thread == caller);
    auto exceptionTicket = request_shader_compile(vertex, static_cast<ShaderStage>(99), 2, 10);
    assert(exceptionTicket);
    bool terminalException = false;
    try { exceptionTicket->consume(); } catch (const std::runtime_error&) { terminalException = true; }
    assert(terminalException);
    auto drainTicket = request_shader_compile(vertex, ShaderStage::Vertex, 2, 11);
    assert(drainTicket);
    finish_shader_compiler();
    assert(drainTicket->ready() && drainTicket->consume().words == reference.words);
    // Abandonment owns no native handles; worker drains its retained job/result.
    auto abandoned = request_shader_compile(vertex, ShaderStage::Vertex, 2, 12);
    assert(abandoned); abandoned.reset();
    finish_shader_compiler();
    assert(!shader_compiler_active());
    finish_shader_compiler();
    auto restarted = compile_shader(vertex, ShaderStage::Vertex, 1, 2);
    assert(restarted.words == reference.words && restarted.thread != caller);
    finish_shader_compiler();
    bool rejected = false;
    try { compile_shader(std::string(4 * 1024 * 1024 + 1, ' '), ShaderStage::Vertex, 1, 2); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}

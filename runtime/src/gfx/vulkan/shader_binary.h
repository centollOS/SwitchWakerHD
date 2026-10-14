#pragma once
#include "shader_compiler.h"
#include <exception>
#include <mutex>
#include <stdexcept>

namespace gfxvk::vk {
// Detached CPU result shared by pipeline workers. Cache publication and the
// accounted flag remain render-thread-only; consumers own shared_ptr snapshots.
struct ShaderBinary {
    std::mutex mutex;
    std::optional<ShaderCompileTicket> ticket;
    std::optional<ShaderCompileResult> result;
    std::exception_ptr failure;
    bool accounted = false;
    explicit ShaderBinary(ShaderCompileTicket value) : ticket(std::move(value)) {}
    ShaderCompileResult get() {
        std::lock_guard lock(mutex);
        if (failure) std::rethrow_exception(failure);
        if (!result) try {
            result = ticket->consume();
            ticket.reset();
        } catch (...) { failure = std::current_exception(); ticket.reset(); throw; }
        return *result;
    }
    bool ready() {
        std::unique_lock lock(mutex, std::try_to_lock);
        return lock.owns_lock() && (result || failure || (ticket && ticket->ready()));
    }
};
}

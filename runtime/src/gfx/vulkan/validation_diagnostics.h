#pragma once
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace gfxvk {
// Retaining a bounded diagnostic copy must never consume normal validation output.
class ValidationDiagnostics {
public:
    template<class Output>
    void record(std::string_view message, Output&& output) {
        output(message);
        std::lock_guard lock(mutex_);
        if (messages_.size() < 64) messages_.emplace_back(message.substr(0, 4096));
    }
    std::vector<std::string> snapshot() const {
        std::lock_guard lock(mutex_);
        return messages_;
    }
private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};
}

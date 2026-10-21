#include "gfx/vulkan/validation_diagnostics.h"
#include <cassert>
#include <string>
#include <vector>
int main() {
    gfxvk::ValidationDiagnostics diagnostics;
    std::vector<std::string> output;
    auto normal = [&](std::string_view message) { output.emplace_back(message); };
    for (unsigned i = 0; i < 80; ++i) diagnostics.record("warning " + std::to_string(i), normal);
    diagnostics.record("SYNC-HAZARD-WRITE-AFTER-WRITE", normal);
    auto retained = diagnostics.snapshot();
    assert(retained.size() == 64);
    assert(retained.front() == "warning 0" && retained.back() == "warning 63");
    assert(output.size() == 81 && output.back() == "SYNC-HAZARD-WRITE-AFTER-WRITE");
    gfxvk::ValidationDiagnostics longMessage;
    longMessage.record(std::string(8192, 'x'), normal);
    assert(output.back().size() == 8192);
    assert(longMessage.snapshot().front().size() == 4096);
}

// SwitchWakerHD (this fork): upstream's desktop services the Switch build leaves out, as no-ops.
// crash_addr.cpp (dladdr, backtrace): the Switch's crash report is main_switch.cpp's, with the game
// function and module offsets of its own. HTTPS downloads (the mods catalogue): none on the console.
#include <stdexcept>

#include "../crash_addr.h"
#include "https.h"

namespace crash_addr {
int describe(char* buf, size_t cap, uintptr_t, char*, size_t) {
    if (cap) buf[0] = 0;
    return 0;
}
void set_game_functions(const uintptr_t*, const uint32_t*, size_t) {}
void host_backtrace(int, Out, const void*) {}
uintptr_t context_pc(const void*) { return 0; }
void prime() {}
}  // namespace crash_addr

namespace host {
void download_https(const std::string&, const std::filesystem::path&, uint64_t) {
    throw std::runtime_error("downloads are not available on the Switch");
}
}  // namespace host

// (this fork) weak stand-ins for fork symbols that upstream's tests don't link: the main-thread HLE marker
// (runtime.h HleMark, threads.cpp), the warp tour's clock (mods.cpp), the command staging hand-over
// (gx2_core.cpp), the screen-mode hooks (screen_mode.cpp) and the guest-mod hook check (guest_builtin.cpp).
// A test that links the real file gets the real definition.
#include <atomic>
#include <cstdint>
#include <string>

struct Cpu;
struct WWHDGuestModuleV1;

__attribute__((weak)) std::atomic<const char*> g_main_hle{nullptr};
__attribute__((weak)) std::atomic<bool> g_main_sampler_on{false};
namespace mods { __attribute__((weak)) double game_time() { return 0; } }
namespace gx2 { __attribute__((weak)) void publish_staged() {} }
namespace screen_mode { __attribute__((weak)) bool hidden_pane(uint32_t) { return false; } }
namespace guestbuiltin {
__attribute__((weak)) bool hooks_checked(const WWHDGuestModuleV1&, std::string&) { return true; }
}
extern "C" __attribute__((weak)) void f_02618094_orig(Cpu*) {}

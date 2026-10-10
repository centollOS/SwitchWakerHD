// Switch-only settings: the handheld GPU profile (apm) and the picture and counter options the
// settings overlay's Switch tab changes, saved in settings.ini (settings_ini.h; the variables these stand for,
// such as WWHD_CPU_CLOCK, are ignored in its [dev] section).
#pragma once
#include <string>

namespace switch_settings {

// GPU profiles offered in handheld (apm's official configurations; docked is left to the system)
// kGpu614: not an apm configuration: memory 1600 through apm (0x92220007), then the GPU clock set to
// 614.4 MHz through clkrst, as sys-clk does (an overclock in handheld; docked keeps the docked clocks)
enum GpuProfile : int { kGpuDefault, kGpu384, kGpu460, kGpu460Mem1600, kGpu614, kGpuProfiles };
const char* gpu_profile_label(int p);
int gpu_profile();                // the one chosen (as the menu shows it)
// apm's handheld configuration in use ("" before the first change); its id for the log and the menu
std::string gpu_profile_status();
void set_gpu_profile(int p);      // applies it now and saves it (host loop thread)

// CPU clock: the steps of the system's CPU table from stock 1020 MHz to 1785 MHz (the CPU boost clock games
// get during loading), set through clkrst in handheld and docked, as sys-clk does. Default: the stock 1020 MHz
// (2026-10-08); 1224 MHz is marked Recommended (it steadies the busiest scenes at 30 fps).
enum CpuClock : int { kCpu1020, kCpu1122, kCpu1224, kCpu1326, kCpu1428, kCpu1581, kCpu1683, kCpu1785, kCpuClocks };
constexpr int kCpuDefault = kCpu1020;
constexpr int kCpuRecommended = kCpu1224;
const char* cpu_clock_label(int c);
int cpu_clock();
void set_cpu_clock(int c);        // applies it now and saves it (host loop thread)

// Picture profiles, one per mode (as SwitchWaker's [handheld]/[docked] options): the internal resolution and
// dynamic resolution. The active mode's profile applies by itself on dock / undock. Defaults: handheld 1x
// (1280x720), docked 1.5x (1920x1080), dynamic resolution on in both.
enum Mode : int { kHandheld, kDocked, kModes };
const char* mode_label(int m);    // "Handheld" / "Docked"
int active_mode();
struct ResProfile {
    float scale = 1.0f;
    bool dynamic = true;
};
ResProfile res_profile(int m);
void set_res_profile(int m, ResProfile p);  // saves it; applies it now when m is the active mode

// host loop, often: once a second, sets the clocks chosen above again when the system has changed
// them (dock / undock, sleep, apm): what sys-clk does for its profiles; and the active mode's picture profile
void tick();

// at start (main.cpp): the GPU profile, then the saved picture and counter options
void apply_at_start();

// settings.ini keys of the Switch tab
constexpr const char* kKeyGpuProfile = "switchGpuProfile";
constexpr const char* kKeyCpuClock = "switchCpuClock";
constexpr const char* kKeyExposure = "switchExposure";
constexpr const char* kKeyContrast = "switchContrast";
constexpr const char* kKeySaturation = "switchSaturation";
constexpr const char* kKeyGamma = "switchGamma";
constexpr const char* kKeyFpsCounter = "switchFpsCounter";
constexpr const char* kKeyCaptureCombo = "switchCaptureCombo";
constexpr const char* kKeyAniso = "switchAniso";
// which Wii U controller the Switch controller is (Debug section): single (the GamePad, single screen; the default),
// gamepad, pro; single <-> the others from the next start (screen_mode::single_screen)
enum class ControllerMode : int { kSingle, kGamePad, kPro };
constexpr const char* kKeyControllerMode = "controllerMode";
constexpr const char* kKeyGamepadPip = "switchGamepadPip";  // the GamePad picture in a corner (gfxsw::gamepad_pip)
// Debug: the network debug server (debug_switch.h) and the main thread's runtime-call sampler (threads.cpp),
// both off by default and read at the next start, before anything else (main.cpp)
constexpr const char* kKeyDebugServer = "switchDebugServer";
constexpr const char* kKeyMainSampler = "switchMainSampler";
// per mode: <key>.handheld / <key>.docked
constexpr const char* kKeyResScale = "switchResScale";
constexpr const char* kKeyDynamicRes = "switchDynamicRes";
void save_picture();       // the current picture adjustments into settings.ini
// debug: both sticks clicked capture the next frame (PNGs on the SD card, a few seconds' freeze); off by
// default so it cannot happen by accident (Switch tab; saved)
bool capture_combo();
void set_capture_combo(bool on);  // saves it
// the GamePad picture in a corner of the TV picture (gfxsw::gamepad_pip); off by default
void set_gamepad_pip(bool on);    // saves it

}  // namespace switch_settings

// Switch-only settings: the handheld GPU profile (apm) and the picture and counter options the
// settings overlay's Switch tab changes, saved in settings.ini. env.txt values win at start.
#pragma once
#include <string>

namespace switch_settings {

// GPU profiles offered in handheld (apm's official configurations; docked is left to the system)
// kGpu614: not an apm configuration: memory 1600 through apm (0x92220007), then the GPU clock set to
// 614.4 MHz through clkrst, as sys-clk does (an overclock in handheld; docked keeps the docked clocks)
enum GpuProfile : int { kGpuDefault, kGpu384, kGpu460, kGpu460Mem1600, kGpu614, kGpuProfiles };
const char* gpu_profile_label(int p);
int gpu_profile();                // the one chosen (as the menu shows it)
bool gpu_profile_env();           // WWHD_GPU_PROFILE in env.txt chose it at start
// apm's handheld configuration in use ("" before the first change); its id for the log and the menu
std::string gpu_profile_status();
void set_gpu_profile(int p);      // applies it now and saves it (host loop thread)

// CPU clock: the steps of the system's CPU table from stock 1020 MHz to 1785 MHz (the CPU boost clock games
// get during loading), set through clkrst in handheld and docked, as sys-clk does. Default 1224 MHz.
enum CpuClock : int { kCpu1020, kCpu1122, kCpu1224, kCpu1326, kCpu1428, kCpu1581, kCpu1683, kCpu1785, kCpuClocks };
constexpr int kCpuDefault = kCpu1224;
const char* cpu_clock_label(int c);
int cpu_clock();
bool cpu_clock_env();             // WWHD_CPU_CLOCK in env.txt chose it at start
void set_cpu_clock(int c);        // applies it now and saves it (host loop thread)

// host loop, often: once a second, sets the clocks chosen above again when the system has changed
// them (dock / undock, sleep, apm): what sys-clk does for its profiles
void tick();

// at start, after env.txt (main.cpp): the GPU profile, then the saved picture and counter options
void apply_at_start();

// settings.ini keys of the Switch tab
constexpr const char* kKeyGpuProfile = "switchGpuProfile";
constexpr const char* kKeyCpuClock = "switchCpuClock";
constexpr const char* kKeyExposure = "switchExposure";
constexpr const char* kKeyContrast = "switchContrast";
constexpr const char* kKeySaturation = "switchSaturation";
constexpr const char* kKeyGamma = "switchGamma";
constexpr const char* kKeyFpsCounter = "switchFpsCounter";
void save_picture();       // the current picture adjustments into settings.ini
bool picture_env();        // one of WWHD_EXPOSURE/CONTRAST/SATURATION/GAMMA is in env.txt
bool fps_counter_env();    // WWHD_FPS is in env.txt

}  // namespace switch_settings

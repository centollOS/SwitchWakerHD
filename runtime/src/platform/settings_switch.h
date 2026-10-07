// Switch-only settings: the handheld GPU profile (apm) and the picture and counter options the
// settings overlay's Switch tab changes, saved in settings.ini. env.txt values win at start.
#pragma once
#include <string>

namespace switch_settings {

// GPU profiles offered in handheld (apm's official configurations; docked is left to the system)
enum GpuProfile : int { kGpuDefault, kGpu384, kGpu460, kGpu460Mem1600, kGpuProfiles };
const char* gpu_profile_label(int p);
int gpu_profile();                // the one chosen (as the menu shows it)
bool gpu_profile_env();           // WWHD_GPU_PROFILE in env.txt chose it at start
// apm's handheld configuration in use ("" before the first change); its id for the log and the menu
std::string gpu_profile_status();
void set_gpu_profile(int p);      // applies it now and saves it (host loop thread)

// at start, after env.txt (main.cpp): the GPU profile, then the saved picture and counter options
void apply_at_start();

// settings.ini keys of the Switch tab
constexpr const char* kKeyGpuProfile = "switchGpuProfile";
constexpr const char* kKeyExposure = "switchExposure";
constexpr const char* kKeyContrast = "switchContrast";
constexpr const char* kKeySaturation = "switchSaturation";
constexpr const char* kKeyGamma = "switchGamma";
constexpr const char* kKeyFpsCounter = "switchFpsCounter";
void save_picture();       // the current picture adjustments into settings.ini
bool picture_env();        // one of WWHD_EXPOSURE/CONTRAST/SATURATION/GAMMA is in env.txt
bool fps_counter_env();    // WWHD_FPS is in env.txt

}  // namespace switch_settings

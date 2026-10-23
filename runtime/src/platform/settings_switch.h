// Switch-only settings: the handheld GPU profile (apm) and the picture and counter options the
// settings overlay's Switch tab changes, saved in settings.ini (settings_ini.h; the variables these stand for,
// such as WWHD_CPU_CLOCK, are ignored in its [dev] section).
#pragma once
#include <string>

namespace switch_settings {

// GPU profiles offered in handheld (apm's official configurations; docked is left to the system).
// No clock is set through clkrst any more (the CPU steps and GPU 614 MHz were removed 2026-10-13: a CPU clock
// set that way kept an Erista console from waking from sleep, #33); overclocks are sys-clk's job.
enum GpuProfile : int { kGpuStock, kGpu384, kGpu460, kGpu460Mem1600, kGpuProfiles };
constexpr int kGpuDefaultChoice = kGpu460;  // without a saved choice (Nintendo's highest handheld profile)
const char* gpu_profile_label(int p);
int gpu_profile();                // the one chosen (as the menu shows it)
// apm's handheld configuration in use ("" before the first change); its id for the log and the menu
std::string gpu_profile_status();
void set_gpu_profile(int p);      // applies it now and saves it (host loop thread)

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

// host loop, often: once a second, the active mode's picture profile (dock / undock)
void tick();

// at start (main.cpp): the GPU profile, then the saved picture and counter options
void apply_at_start();

// settings.ini keys of the Switch tab
constexpr const char* kKeyGpuProfile = "switchGpuProfile";
constexpr const char* kKeyCpuClock = "switchCpuClock";  // removed option: only read to log that it is ignored
constexpr const char* kKeyExposure = "switchExposure";
constexpr const char* kKeyContrast = "switchContrast";
constexpr const char* kKeySaturation = "switchSaturation";
constexpr const char* kKeyGamma = "switchGamma";
constexpr const char* kKeyFpsCounter = "switchFpsCounter";
constexpr const char* kKeyCaptureCombo = "switchCaptureCombo";
constexpr const char* kKeyAniso = "switchAniso";
constexpr const char* kKeyFxaa = "switchFxaa";                   // gfxsw::fxaa (issue #21), off by default
constexpr const char* kKeyBloom = "switchBloom";                 // gfxsw::bloom
constexpr const char* kKeyDistanceBlur = "switchDistanceBlur";  // gfxsw::distance_blur
// which Wii U controller the Switch controller is (Debug section): single (the GamePad, single screen; the default),
// gamepad, pro; single <-> the others from the next start (screen_mode::single_screen)
enum class ControllerMode : int { kSingle, kGamePad, kPro };
constexpr const char* kKeyControllerMode = "controllerMode";
constexpr const char* kKeyGamepadPip = "switchGamepadPip";  // the GamePad picture in a corner (gfxsw::gamepad_pip)
// Debug: the network debug server (debug_switch.h) and the main thread's runtime-call sampler (threads.cpp),
// both off by default and read at the next start, before anything else (main.cpp)
constexpr const char* kKeyDebugServer = "switchDebugServer";
constexpr const char* kKeyMainSampler = "switchMainSampler";
// Debug: draws wait for their shaders (shader_wait_switch.h, #49): 0 off (the default) .. 3 always
constexpr const char* kKeyShaderWait = "switchShaderWait";
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

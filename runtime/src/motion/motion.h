// Gyro (motion) input for the virtual Wii U GamePad: VPADRead's acc, gyro, angle and dir fields.
//
// Sources (one at a time, Settings > Controls > Gyro; saved as "gyro.*"):
//   off         VPAD reports a GamePad at rest, lying flat (the port's behaviour before gyro support)
//   controller  the motion sensors of a host controller: DualSense, DualShock 4, Switch Pro, Joy-Con,
//               Steam Deck / Steam Controller and others (SDL3 sensors; GameController.framework on the
//               macOS AppKit host)
//   cemuhook    a Cemuhook (DSU) server over UDP, default 127.0.0.1:26760 (dsu.h): DS4Windows,
//               BetterJoy, SteamDeckGyroDSU, phone apps
//   mouse       mouse movement turns the GamePad while the game aims (bow, telescope, ...): for Steam
//               Input's "gyro to mouse" and plain mice
// The game decides itself when it uses the gyro (its own Options "Gyro" switch, aiming); this module
// only reports how the virtual GamePad moves. WWHD reads only the direction matrix and turns its
// first-person camera by its change from one frame to the next (docs/gyro.md), so no source needs
// an absolute heading.
//
// Threads: sources feed from the host's main thread (SDL events, GameController handlers) or the DSU
// thread; vpad() runs on the game thread inside VPADRead. Everything is behind one mutex and short.
//
// Test aid: WWHD_TEST_GYRO=from-to:yaw:pitch,... turns the virtual GamePad at yaw / pitch degrees
// per second during game-time seconds (as WWHD_TEST_MOUSE), with any source (also off).
#pragma once
#include <cstdint>
#include <string>

#include "fusion.h"

namespace motion {

enum Source : int { kOff, kController, kCemuhook, kMouse, kSourceCount };
const char* source_id(int s);     // "off", "controller", "cemuhook", "mouse"
const char* source_label(int s);
int source_from_id(const std::string& id);  // -1 if unknown

struct Settings {
    int source = kOff;
    Tuning tuning;                 // sensitivity and invert, all sources
    float mouse_degrees = 0.1f;    // mouse source: degrees the GamePad turns per mouse point
    std::string dsu_host = "127.0.0.1";
    int dsu_port = 26760;
    int dsu_slot = 0;              // 0..3
    int recenter_pad = 0;          // input_map::Pad that recenters (0 = none)
    int recenter_key = -1;         // keyboard key (macOS virtual key code) that recenters (-1 = none)
    bool operator==(const Settings&) const = default;
};
// key=value lines, the form hostui keeps settings in (also used by the tests)
std::string to_ini(const Settings& s);
void from_kv(Settings& s, const std::string& key, const std::string& value);  // unknown keys ignored
const char* const kKeys[] = {"gyro.source", "gyro.sensitivityX", "gyro.sensitivityY", "gyro.invertX", "gyro.invertY",
                             "gyro.mouseDegrees", "gyro.dsuHost", "gyro.dsuPort", "gyro.dsuSlot", "gyro.recenterPad",
                             "gyro.recenterKey"};
std::string value_of(const Settings& s, const std::string& key);

Settings settings();
// applies at once (starts or stops the Cemuhook client, controller sensors follow on the next host update)
void set_settings(const Settings& s);
// WWHD_GYRO=off|controller|cemuhook|mouse overrides the saved source at startup
bool env_override();

// ---- sources ----
// a controller's sensor sample (SDL frame: gyro rad/s, accel m/s^2); `device` tells controllers apart,
// `timestamp_ns` is the sensor's time (0: the time it arrived)
void controller_sample(uint64_t device, uint64_t timestamp_ns, const float gyro[3], const float accel[3]);
void controller_gone(uint64_t device);
bool wants_controller_sensors();  // the hosts enable the sensors only for the controller source
void set_gyro_controllers(int n); // hosts: connected controllers that have a gyro and an accelerometer
int gyro_controllers();
// mouse movement in the game window (points, dy > 0 = down), captured or not
void mouse_motion(float dx, float dy);
// the mouse source is active and the game aims now: mouse movement belongs to the gyro (the mouse
// camera mod leaves it alone) and the hosts capture the pointer while this holds
bool mouse_drives_gyro();
// mods/camera.cpp: the game's aiming / first-person state, once per logic step
void set_aiming(bool aiming);
bool aiming();

// recenter: the virtual GamePad points forward again (also drops drift)
void recenter();
// hosts, every input update: the bound controller input (values: input_map::kPadCount entries 0..1) or key
void poll_recenter(const float* pad_values, const bool* keys);

// ---- VPADRead (game thread) ----
// the motion fields of this read; repeat = the read repeats the previous sample (frame interpolation)
VpadMotion vpad(bool repeat);
// overlay status line: what the source does right now
std::string status();

}  // namespace motion

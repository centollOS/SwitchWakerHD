// Motion sensor math for the virtual GamePad gyro (see motion.h): axis mapping of the host sources,
// orientation integration with gravity correction and gyro bias estimation, and the VPAD values.
//
// Plain C++ (no SDL, no sockets), unit-tested in runtime/tools/motion_test.cpp.
//
// Frames. The VPAD values follow the conventions Cemu established by recording a real GamePad
// (cemu/src/input/motion/MotionSample.h); WWHD's gyro aiming works with them in Cemu, so they are
// what this port reproduces. Adapted from Cemu (MPL-2.0, https://github.com/cemu-project/Cemu):
// the axis signs of the SDL and DSU sources, the attitude matrix layout and the gravity-feedback
// gain; the code here is our own.
//   - Host frame "H" (what Fusion integrates), right-handed: the SDL controller frame turned by 180
//     degrees about X: x right, y down out of the face, z away from the player. acc_H is the gravity
//     direction in g (at rest flat on a table: (0, 1, 0)); gyro_H is in radians per second.
//   - World frame: the pose "flat on the table, top away from the player" is the 90 degree turn of
//     H about X; gravity is world +Z.
//   - GamePad frame (VPAD): x = H.x, y = H.y, z = -H.z (a mirror of H, as the hardware reports).
//     VPAD gyro is in revolutions per second (1.0 = 360 deg/s), angle in revolutions, acc in g
//     (specific force), dir = the GamePad's X, Y and Z axes in world coordinates (x, z, y swizzle),
//     the identity when it lies flat with its top away from the player.
#pragma once
#include <cmath>
#include <cstdint>

namespace motion {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length() const { return std::sqrt(dot(*this)); }
};

// unit quaternion, Hamilton product; rotates body (H) vectors into the world frame
struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat() = default;
    Quat(float a, float b, float c, float d) : w(a), x(b), y(c), z(d) {}
    static Quat axis_angle(Vec3 axis, float radians);
    Quat operator*(const Quat& r) const {
        return {w * r.w - x * r.x - y * r.y - z * r.z, w * r.x + x * r.w + y * r.z - z * r.y,
                w * r.y - x * r.z + y * r.w + z * r.x, w * r.z + x * r.y - y * r.x + z * r.w};
    }
    Quat conj() const { return {w, -x, -y, -z}; }
    void normalize();
    Vec3 rotate(Vec3 v) const;      // body -> world
    Vec3 unrotate(Vec3 v) const;    // world -> body
};

// The values VPADRead reports (VPADStatus +0x1C..+0x8F).
struct VpadMotion {
    Vec3 acc{0, -1, 0};          // +0x1C, g (flat on a table: gravity along -y)
    float acc_magnitude = 1;     // +0x28
    float acc_variation = 0;     // +0x2C, change of acc since the previous sample
    float acc_xy[2] = {1, 0};    // +0x30
    Vec3 gyro;                   // +0x38, revolutions per second
    Vec3 angle;                  // +0x44, revolutions (gyro integrated)
    Vec3 dir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // +0x6C, +0x78, +0x84
};

// ---- host sources -> frame H ----
// SDL: gyro rad/s and acceleration m/s^2 in SDL's frame (x right, y up, z towards the player)
void from_sdl(const float gyro[3], const float accel[3], Vec3& gyro_h, Vec3& acc_h);
// Cemuhook (DSU): gyro deg/s (pitch, yaw, roll) and acceleration in g, as DS4Windows and the others send
void from_dsu(const float gyro_deg[3], const float accel_g[3], Vec3& gyro_h, Vec3& acc_h);

// Settings the fusion applies to every source (Gyro settings in the overlay's Controls tab)
struct Tuning {
    float sensitivity_x = 1, sensitivity_y = 1;  // multiplier on yaw (horizontal) and pitch (vertical) turning
    bool invert_x = false, invert_y = false;
    bool operator==(const Tuning&) const = default;
};

// Orientation integration (Mahony-style complementary filter): gyro integrated in the body frame,
// pulled towards the measured gravity (no drift in pitch and roll), and a gyro bias estimated while
// the controller rests (no slow yaw drift). One instance per source.
class Fusion {
public:
    Fusion() { reset(); }
    void reset();          // flat, top away from the player; bias and angle kept
    void recenter();       // heading back to "forward" (turn about gravity), angle zeroed, tilt kept
    // one sensor sample: dt seconds since the previous one, gyro_h rad/s, acc_h in g (zero vector: no
    // accelerometer, e.g. the mouse source)
    void update(float dt, Vec3 gyro_h, Vec3 acc_h, const Tuning& t);
    // the VPAD values now; the gyro is the mean rate since the previous call (the game reads 30 or
    // 60 times a second, sensors send 100-1000 samples), or the latest rate when nothing came
    VpadMotion vpad();
    Quat orientation() const { return q_; }
    Vec3 bias() const { return bias_; }
    bool calibrated() const { return bias_samples_ >= kBiasSamples; }
    // the reported gyro is zero below this rate (rad/s, after bias removal): sensor noise
    static constexpr float kNoise = 0.008f;
    static constexpr int kBiasSamples = 100;

private:
    Quat q_;
    Vec3 bias_;
    int bias_samples_ = 0;       // samples in the running bias average (capped: it follows slow changes)
    float still_time_ = 0;       // seconds the controller has rested
    Vec3 last_acc_h_, last_gyro_h_, last_rate_v_;
    Vec3 acc_v_{0, -1, 0};
    float acc_var_ = 0;
    Vec3 angle_, angle_read_;    // revolutions, VPAD frame
    double window_ = 0;          // seconds integrated since the last vpad()
    bool have_acc_ = false;
};

// ---- the mouse as a gyro (Steam Input "gyro to mouse", or a plain mouse) ----
// points moved since the previous call and the seconds between -> rate in frame H. The mouse turns the
// controller about the world's vertical (left/right) and its own X axis (up/down), so it works for any
// pose. degrees_per_point: sensitivity.
Vec3 mouse_rate(const Quat& q, float dx, float dy, float dt, float degrees_per_point);

}  // namespace motion

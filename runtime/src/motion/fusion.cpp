// Motion sensor math (see fusion.h).
#include "fusion.h"

#include <algorithm>

namespace motion {

static constexpr float kPi = 3.14159265358979f;
static constexpr float kTwoPi = 2 * kPi;
static constexpr float kStandardGravity = 9.80665f;
// gravity feedback gain (rad/s per unit of tilt error). WWHD turns its first-person camera by the
// frame-to-frame change of the GamePad's direction (see motion.h), so every correction shows up as camera
// movement: the gain is small (Cemu uses 0.5) and the feedback only runs while the accelerometer
// measures plain gravity (no shaking).
static constexpr float kGravityGain = 0.05f;

Quat Quat::axis_angle(Vec3 a, float r) {
    float len = a.length();
    if (len <= 0) return {};
    float s = std::sin(r * 0.5f) / len;
    return {std::cos(r * 0.5f), a.x * s, a.y * s, a.z * s};
}
void Quat::normalize() {
    float n = std::sqrt(w * w + x * x + y * y + z * z);
    if (!(n > 0)) { *this = {}; return; }
    w /= n; x /= n; y /= n; z /= n;
}
Vec3 Quat::rotate(Vec3 v) const {
    Vec3 u{x, y, z};
    Vec3 t = u.cross(v) * 2.0f;
    return v + t * w + u.cross(t);
}
Vec3 Quat::unrotate(Vec3 v) const { return conj().rotate(v); }

// SDL: x right, y up, z towards the player; H = SDL turned 180 degrees about x. Accelerometers measure
// the specific force (up at rest); acc_H is the gravity direction, so the sign flips once more.
void from_sdl(const float g[3], const float a[3], Vec3& gyro_h, Vec3& acc_h) {
    gyro_h = {g[0], -g[1], -g[2]};
    acc_h = Vec3{-a[0], a[1], a[2]} * (1.0f / kStandardGravity);
}
// DSU servers send DS4 conventions: gyro (pitch, yaw, roll) deg/s, acceleration in g
void from_dsu(const float g[3], const float a[3], Vec3& gyro_h, Vec3& acc_h) {
    const float k = kPi / 180.0f;
    gyro_h = {g[0] * k, g[1] * k, g[2] * k};
    acc_h = {a[0], -a[1], -a[2]};
}

// flat on the table, top away from the player: H turned 90 degrees about x
static Quat flat_pose() { return Quat::axis_angle({1, 0, 0}, kPi / 2); }
static const Vec3 kWorldDown{0, 0, 1}, kWorldForward{0, -1, 0};

void Fusion::reset() {
    q_ = flat_pose();
    angle_ = angle_read_ = {};
    window_ = 0;
}

void Fusion::recenter() {
    // turn about the vertical so that the controller's forward (H z) points to world forward
    Vec3 f = q_.rotate({0, 0, 1});
    Vec3 h{f.x, f.y, 0};
    if (h.length() > 0.05f) {
        float yaw = std::atan2(h.x, -h.y);  // angle of f from world forward, about world z (down)
        q_ = Quat::axis_angle(kWorldDown, -yaw) * q_;
        q_.normalize();
    }
    angle_ = angle_read_ = {};
}

void Fusion::update(float dt, Vec3 gyro, Vec3 acc, const Tuning& t) {
    if (!(dt > 0)) return;
    dt = std::min(dt, 0.1f);  // a stall (or a reconnected source) must not throw the pose around
    have_acc_ = acc.length() > 1e-4f;
    // bias: running average of the rate while the controller rests (rate small and steady, gravity
    // steady near 1 g). Before the first estimate the rate limit is loose (some controllers rest at
    // 5 deg/s), afterwards tight, so slow aiming never ends up in the bias.
    if (have_acc_) {
        float acc_change = (acc - last_acc_h_).length();
        float limit = calibrated() ? 0.06f : 0.2f;
        bool still = (gyro - bias_).length() < limit && (gyro - last_gyro_h_).length() < 0.03f &&
                     std::fabs(acc.length() - 1.0f) < 0.1f && acc_change < 0.02f;
        still_time_ = still ? still_time_ + dt : 0;
        if (still_time_ > 0.3f) {
            bias_samples_ = std::min(bias_samples_ + 1, kBiasSamples * 10);
            bias_ = bias_ + (gyro - bias_) * (1.0f / bias_samples_);
        }
        acc_var_ = acc_change;
        last_acc_h_ = acc;
        acc_v_ = {acc.x, acc.y, -acc.z};  // gravity direction in the GamePad frame
    }
    last_gyro_h_ = gyro;
    Vec3 w = gyro - bias_;
    if (w.length() < kNoise) w = {};  // noise floor: a controller held still reports nothing
    // tuning, applied to the turning the player sees: yaw about the world vertical (in body
    // coordinates), pitch about the body's horizontal right axis
    Vec3 down = q_.unrotate(kWorldDown);
    Vec3 right = Vec3{1, 0, 0} - down * down.x;
    if (right.length() > 0.1f) {
        right = right * (1.0f / right.length());
        float yaw = w.dot(down), pitch = w.dot(right);
        float yaw2 = yaw * t.sensitivity_x * (t.invert_x ? -1.0f : 1.0f);
        float pitch2 = pitch * t.sensitivity_y * (t.invert_y ? -1.0f : 1.0f);
        w = w + down * (yaw2 - yaw) + right * (pitch2 - pitch);
    }
    Vec3 w_int = w;
    // gravity feedback (tilt drift), only with the true pitch: it would pull a scaled or inverted pitch
    // back to the physical one. Yaw is unobservable from gravity; the bias keeps it from drifting.
    if (have_acc_ && t.sensitivity_y == 1.0f && !t.invert_y && std::fabs(acc.length() - 1.0f) < 0.05f) w_int = w_int - down.cross(acc * (1.0f / acc.length())) * kGravityGain;
    // q' = q + q * (0, w dt / 2)
    Quat d = q_ * Quat(0, w_int.x * 0.5f * dt, w_int.y * 0.5f * dt, w_int.z * 0.5f * dt);
    q_ = {q_.w + d.w, q_.x + d.x, q_.y + d.y, q_.z + d.z};
    q_.normalize();
    // the GamePad frame is a mirror of H: the rate (a pseudo-vector) flips once more
    Vec3 rate_v = Vec3{w.x, w.y, -w.z} * (-1.0f / kTwoPi);
    last_rate_v_ = rate_v;
    angle_ = angle_ + rate_v * dt;
    window_ += dt;
}

VpadMotion Fusion::vpad() {
    VpadMotion m;
    // gyro: mean rate since the previous read
    if (window_ > 0) m.gyro = (angle_ - angle_read_) * (float)(1.0 / window_);
    else m.gyro = last_rate_v_;
    angle_read_ = angle_;
    window_ = 0;
    m.angle = angle_;
    // acc: the accelerometer, or (mouse source) gravity from the pose
    Vec3 g = have_acc_ ? acc_v_ : [&] { Vec3 d = q_.unrotate(kWorldDown); return Vec3{d.x, d.y, -d.z}; }();
    // VPAD acc is the specific force, gravity points the other way: flat on a table it reads (0, -1, 0)
    m.acc = {-g.x, -g.y, -g.z};
    m.acc_magnitude = m.acc.length();
    m.acc_variation = have_acc_ ? acc_var_ : 0;
    // acc_xy: the GamePad's tilt in its screen plane (cos, sin), 1, 0 when upright
    float xy = std::sqrt(m.acc.x * m.acc.x + m.acc.y * m.acc.y);
    if (xy > 0.1f) { m.acc_xy[0] = -m.acc.y / xy; m.acc_xy[1] = m.acc.x / xy; }
    // dir: the GamePad's axes (x = H x, y = H y, z = -H z) in world coordinates, swizzled (x, z, y)
    auto sw = [](Vec3 v) { return Vec3{v.x, v.z, v.y}; };
    m.dir[0] = sw(q_.rotate({1, 0, 0}));
    m.dir[1] = sw(q_.rotate({0, 1, 0}));
    m.dir[2] = sw(q_.rotate({0, 0, -1}));
    return m;
}

Vec3 mouse_rate(const Quat& q, float dx, float dy, float dt, float degrees_per_point) {
    if (!(dt > 0)) return {};
    const float k = degrees_per_point * kPi / 180.0f / dt;
    Vec3 down = q.unrotate(kWorldDown);
    // mouse right turns right: about the down axis, positive (right-handed about down = clockwise
    // seen from above); mouse up (dy < 0) tilts the top up: about the right axis, positive
    return down * (dx * k) + Vec3{1, 0, 0} * (-dy * k);
}

}  // namespace motion

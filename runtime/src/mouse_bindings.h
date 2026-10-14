#pragma once
#include "input_map.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <deque>
#include <utility>

namespace input {
// Event-driven, per physical device release gates. A consumed down stays consumed until its up,
// even if capture/overlay state changes in between. Call consume() on focus/overlay transitions.
class MouseBindings {
    struct Device { std::array<bool,5> held{}, blocked{}; };
    std::map<uint64_t,Device> devices_;
    std::deque<std::pair<int,int>> wheel_; // direction and notch count, in event order
    float remainder_ = 0;
    bool wheel_release_ = false;
    std::array<bool,7> previous_{}, app_taken_{};
public:
    void button(uint64_t id, int button, bool down, bool eligible) {
        if (button < 0 || button >= 5) return;
        auto& d = devices_[id];
        if (!down) {
            d.held[button] = d.blocked[button] = false;
            if (std::none_of(d.held.begin(), d.held.end(), [](bool held){return held;})) devices_.erase(id);
            return;
        }
        if (!d.held[button]) d.blocked[button] = !eligible;
        else if (!eligible) d.blocked[button] = true;
        d.held[button] = true;
    }
    void remove_device(uint64_t id) { devices_.erase(id); }
    void consume() {
        for (auto& [id,d] : devices_) for (int b=0;b<5;++b) d.blocked[b] = d.blocked[b] || d.held[b];
        wheel_.clear(); remainder_ = 0; wheel_release_ = false; previous_.fill(false); app_taken_.fill(false);
    }
    void wheel(float delta, bool eligible) {
        if (!eligible) { remainder_ = 0; return; }
        remainder_ += delta;
        int n = static_cast<int>(remainder_);
        remainder_ -= n;
        if (n) {
            const int direction = n > 0 ? 0 : 1;
            if (!wheel_.empty() && wheel_.back().first == direction) wheel_.back().second += std::abs(n);
            else wheel_.emplace_back(direction, std::abs(n));
        }
    }
    PadState read(const input_map::Mapping& m, bool eligible, const bool* keyboard = nullptr,
                  bool (*app_key_down)(int) = nullptr, bool wheel_allowed = true) {
        if (!wheel_allowed) { wheel_.clear(); remainder_ = 0; wheel_release_ = false; }
        if (!eligible) consume();
        bool values[input_map::kInputCount]{};
        if (keyboard) std::copy(keyboard, keyboard+256, values);
        if (eligible) {
            for (auto& [id,d] : devices_) for (int b=0;b<5;++b)
                values[input_map::kMouse1+b] = values[input_map::kMouse1+b] || (d.held[b] && !d.blocked[b]);
            bool pulse = false;
            if (!wheel_release_ && !wheel_.empty()) {
                auto& [direction,notches] = wheel_.front();
                values[input_map::kWheelUp+direction] = true;
                if (--notches == 0) wheel_.pop_front();
                pulse = true;
            }
            wheel_release_ = pulse;
        }
        for (int i=0;i<7;++i) {
            const bool held = values[input_map::kMouse1+i];
            if (held && !previous_[i] && app_key_down) app_taken_[i] = app_key_down(input_map::kMouse1+i);
            if (!held) app_taken_[i] = false;
            previous_[i] = held;
            if (app_taken_[i]) values[input_map::kMouse1+i] = false;
        }
        return input_map::keyboard_state(m, values, input_map::kInputCount);
    }
};
} // namespace input

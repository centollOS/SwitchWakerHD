// SPDX-License-Identifier: MPL-2.0
// touch_options.h: the Android touch-control options, and their JNI getter for TouchControls.
#include "touch_options.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include "../overlay/hostui.h"
#ifdef __ANDROID__
#include <jni.h>
#endif

namespace touch_options {
namespace {
std::mutex g_mu;
touch_style::Options g_options;
std::atomic<bool> g_style_saved{false}, g_asked{false};
std::string num(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.2f", v);
    return b;
}
}  // namespace

touch_style::Options get() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_options;
}

void set(const touch_style::Options& in) {
    const touch_style::Options o = touch_style::sanitize(in);
    touch_style::Options was;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        was = g_options;
        g_options = o;
    }
    if (o.style != was.style) hostui::set("touchStyle", std::to_string(o.style));
    if (o.show_sticks != was.show_sticks) hostui::set("touchShowSticks", o.show_sticks ? "1" : "0");
    if (o.follow != was.follow) hostui::set("touchFollow", o.follow ? "1" : "0");
    if (o.right != was.right) hostui::set("touchRight", std::to_string(o.right));
    if (o.swipe_speed != was.swipe_speed) hostui::set("touchSwipeSpeed", num(o.swipe_speed));
    if (o.swipe_boat != was.swipe_boat) hostui::set("touchSwipeBoat", num(o.swipe_boat));
}

bool style_saved() { return g_style_saved.load(); }
bool first_start_asked() { return g_asked.load(); }
void answer_first_start(bool flexible) {
    set(touch_style::first_start_answer(get(), flexible));
    g_asked = true;
    hostui::set("touchStyleAsked", "1");
}

void load(const std::map<std::string, std::string>& saved) {
    touch_style::Options o;
    auto has = [&](const char* k) { return saved.count(k) != 0; };
    auto f = [&](const char* k) { return float(atof(saved.at(k).c_str())); };
    if (has("touchStyle")) o.style = int(f("touchStyle"));
    g_style_saved = has("touchStyle");
    g_asked = has("touchStyleAsked") && f("touchStyleAsked") != 0;
    if (has("touchShowSticks")) o.show_sticks = f("touchShowSticks") != 0;
    if (has("touchFollow")) o.follow = f("touchFollow") != 0;
    if (has("touchRight")) o.right = int(f("touchRight"));
    if (has("touchSwipeSpeed")) o.swipe_speed = f("touchSwipeSpeed");
    if (has("touchSwipeBoat")) o.swipe_boat = f("touchSwipeBoat");
    std::lock_guard<std::mutex> lk(g_mu);
    g_options = touch_style::sanitize(o);
}
}  // namespace touch_options

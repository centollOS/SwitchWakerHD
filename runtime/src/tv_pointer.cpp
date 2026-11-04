// The mouse on the TV picture for the port's own pages (tv_pointer.h).
#include "tv_pointer.h"

#include "input_map.h"

#include <mutex>

namespace tv_pointer {
namespace {
std::atomic<unsigned> g_wanted{0};
std::mutex g_mu;
State g_state;
float g_wheel = 0;
State g_wheel_at;
}  // namespace

void set_wanted(unsigned owner, bool on) {
    const unsigned was = on ? g_wanted.fetch_or(owner) : g_wanted.fetch_and(~owner);
    if (on || !(was & owner) || (was & ~owner)) return;
    std::lock_guard lock(g_mu);
    g_state.down = false;  // no longer wanted by anyone: a held click ends
}
void set_wanted(bool on) { set_wanted(kOwnerPage, on); }
bool wanted() { return g_wanted.load(std::memory_order_relaxed) != 0; }
bool wanted(unsigned owner) { return (g_wanted.load(std::memory_order_relaxed) & owner) != 0; }
void set(bool down, float x, float y, float aspect) {
    std::lock_guard lock(g_mu);
    g_state = {down, x, y, aspect};
}
bool left_button_bound() { return !input_map::key_users(input_map::current(), input_map::kMouse1).empty(); }
State read() {
    std::lock_guard lock(g_mu);
    return g_state;
}
void add_wheel(float notches, float x, float y, float aspect) {
    std::lock_guard lock(g_mu);
    g_wheel += notches;
    g_wheel_at = {false, x, y, aspect};
}
bool take_wheel(float& notches, State& at) {
    std::lock_guard lock(g_mu);
    if (g_wheel == 0) return false;
    notches = g_wheel, at = g_wheel_at;
    g_wheel = 0;
    return true;
}

}  // namespace tv_pointer

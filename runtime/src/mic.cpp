// SPDX-License-Identifier: MPL-2.0
// The microphone service's core (mic.h): who wants it, its state, the pitch of the newest window.
#include "mic.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <vector>

#include "fixed_seed.h"
#include "gfx/renderer.h"
#include "runtime.h"

namespace mic {
namespace {
std::mutex g_mu;  // g_owners, g_stream, g_latest
std::set<int> g_owners;
std::atomic<int> g_state{kClosed};
std::atomic<bool> g_running{false};  // the backend was started (and not stopped)
std::atomic<bool> g_denied_edge{false};
PitchStream* g_stream = nullptr;
int g_stream_rate = 0;
Pitch g_latest;
uint64_t g_serial = 0;

// ---- test aids
struct Tone {
    uint64_t from, to;
    float hz;
};
const std::vector<Tone>& test_tones() {
    static const std::vector<Tone> v = [] {
        std::vector<Tone> out;
        const char* e = getenv("WWHD_TEST_MIC");
        unsigned long long a, b;
        float hz;
        int n = 0;
        while (e && sscanf(e, "%llu-%llu:%f%n", &a, &b, &hz, &n) == 3) {
            out.push_back({a, b, hz});
            e += n;
            if (*e != ',') break;
            e++;
        }
        return out;
    }();
    return v;
}
bool test_mode() { return getenv("WWHD_TEST_MIC") != nullptr || getenv("WWHD_TEST_MIC_DENY") != nullptr; }
void test_feed() {
    // one controller read's worth of a tone (1/30 s at 48 kHz) at the scripted frame's pitch, silence between
    static double phase = 0;
    const uint64_t f = fixed_seed::script_frame(render::frame_count());
    float hz = 0;
    for (const Tone& t : test_tones())
        if (f >= t.from && f < t.to) hz = t.hz;
    float buf[1600];
    for (float& s : buf) {
        s = hz > 0 ? 0.3f * float(std::sin(phase)) : 0.0f;
        phase += 2 * M_PI * hz / 48000.0;
        if (phase > 2 * M_PI) phase -= 2 * M_PI;
    }
    feed(buf, 1600, 48000);
}
}  // namespace

void want(int owner, bool on) {
    std::lock_guard lock(g_mu);
    if (on) g_owners.insert(owner);
    else g_owners.erase(owner);
}
bool wanted(int owner) {
    std::lock_guard lock(g_mu);
    return g_owners.count(owner) != 0;
}
State state() { return State(g_state.load()); }

bool latest(Pitch& p, uint64_t& serial) {
    std::lock_guard lock(g_mu);
    p = g_latest;
    serial = g_serial;
    return g_serial != 0;
}

bool take_denied() { return g_denied_edge.exchange(false); }

void feed(const float* mono, size_t frames, int rate) {
    std::lock_guard lock(g_mu);
    if (!g_running.load() || rate <= 0) return;
    if (!g_stream || g_stream_rate != rate) {
        delete g_stream;
        g_stream = new PitchStream(rate);
        g_stream_rate = rate;
    }
    Pitch p;
    if (g_stream->push(mono, frames, p) > 0) {  // (the samples stop here: only the pitch is kept)
        g_latest = p;
        g_serial++;
    }
}

void started(State s) {
    if (!g_running.load()) {  // stopped meanwhile: the device the backend opened goes again
        if (s == kOpen) backend_stop();
        return;
    }
    g_state = s;
    if (s == kDenied) g_denied_edge = true;
    if (s != kOpen) g_running = false;
    LOG("[mic] %s", s == kOpen ? "listening" : s == kDenied ? "permission declined" : "no microphone");
}

void service() {
    bool want_now;
    {
        std::lock_guard lock(g_mu);
        want_now = !g_owners.empty();
    }
    const bool test = test_mode();
    if (want_now && !g_running.load()) {
        // a new ask after "declined" or "unavailable" only when the option was turned on again (the port turned it
        // off, so its mod let go): the state is then kClosed
        if (g_state.load() == kDenied || g_state.load() == kUnavailable) return;
        g_running = true;
        g_state = kAsking;
        if (!test) backend_start();
        else started(getenv("WWHD_TEST_MIC_DENY") ? kDenied : kOpen);
    } else if (!want_now && (g_running.load() || g_state.load() != kClosed)) {
        const bool was = g_running.exchange(false);
        if (was && !test) backend_stop();
        g_state = kClosed;
        std::lock_guard lock(g_mu);
        if (g_stream) g_stream->reset();
        g_latest = {};
        if (was) LOG("[mic] closed");
    }
    if (test && g_running.load() && g_state.load() == kOpen) test_feed();
}

}  // namespace mic

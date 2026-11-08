// Adapted from GreenNaugahyde/ZeldaWWHDRecompAndroid commit 73b54e1.
// Depth peeks (dDlst_peekZ_c): the game asks for the depth at a few screen points and decides from
// the answer whether something is in front. The sun does this every step for five points around it
// (dKyr_sun_move 02565E0C: a point counts as visible when its depth is >= 0xFFFFFF, nothing drawn
// there); its corona and lens flare fade in only when enough points are visible.
//
// dDlst_peekZ_c (the draw list's member at +0x60EC): u8 count at +0, then up to 64 cells of
// { s16 x, s16 y, u32* result } from +4, in the 640x480 screen space of the GameCube game.
// newData (0252E34C) adds a cell; peekData (0252E388), called once a frame after the 3D scene is
// drawn (025F03F0), answers them: the HD game reads the depth surface's memory on the CPU. This
// renderer never writes GPU results back to guest memory, so that read found no depth and the sun
// counted as hidden. The hook sends the cells to the render thread instead (OP_PEEK_Z), which
// copies those pixels of the main depth buffer and writes the answers once the GPU is done: a
// frame later, which the game expects anyway (it judges the previous step's answers and asks
// again). WWHD_PEEKZ=0 keeps the game's own function.
//
// (this fork, #94) On the Switch the answers are not written to guest memory by the timing below (a fixed later
// swap): by then the object that asked may be gone and its memory another object's (the title screen's demo, 11
// minutes in: a depth over a vtable pointer, a null call). They wait (publish) until the game's next peekData, which
// writes those whose address it asks for again (its object is still there) on the game's own thread, as the game's
// function does, and drops the others. deko3d answers at a later peek (gfx/deko/depth_peek_dk.cpp), so nothing is
// counted and the game never waits at its swap there. The other platforms keep upstream's timing.
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "gfx/depth_peek.h"
#include "gfx/renderer.h"
#include "gx2/gx2_cmd.h"
#include "runtime.h"
#include "interp.h"

extern "C" void f_0252E388_orig(Cpu* c);  // dDlst_peekZ_c::peekData

extern "C" void hook_0252E388(Cpu* c) {
    static const bool off = [] { const char* e = getenv("WWHD_PEEKZ"); return e && atoi(e) == 0; }();
    static const bool log = getenv("WWHD_PEEKZ_LOG") != nullptr;
    if (off || !render::can_peek_z()) { f_0252E388_orig(c); return; }  // (this fork) a backend without peeks
    const uint32_t obj = c->r[3];
    const uint32_t n = std::min<uint32_t>(ld8(obj), 64);
#ifdef __SWITCH__
    gfx::depth_peek::write_answers(obj + 4, n);  // (this fork, #94) the answers that came for these cells
#endif
    std::vector<uint32_t> cells;
    cells.reserve(n * 3);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cell = obj + 4 + i * 8;
        uint32_t dst = ld32(cell + 4);
        if (!dst) continue;
        const int32_t x = int16_t(ld16(cell)), y = int16_t(ld16(cell + 2));
        cells.push_back(uint32_t(x));
        cells.push_back(uint32_t(y));
        cells.push_back(dst);
        // with the step and the answer the game has at dst now (from an earlier frame): runs whose
        // games see the same answers at the same steps behave the same
        if (log) LOG("[peekz] step %llu request (%d,%d) in 640x480 -> %08X (has %06X)",
                     (unsigned long long)interp::logic_steps(), int(x), int(y), dst, ld32(dst));
    }
    if (!cells.empty()) {
#ifndef __SWITCH__  // (this fork, #94) deko3d answers at a later peek: nothing to wait for at the swap
        gfx::depth_peek::issued();
#endif
        gx2::emit(gx2::OP_PEEK_Z, cells.data(), (uint32_t)cells.size());
    }
    st8(obj, 0);  // as the game's function: the cells are taken
}

#include "gfx/depth_peek.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
namespace gfx::depth_peek {
namespace {
AnswerGate g_gate;
std::mutex g_publishMutex;
std::unordered_map<uint32_t, uint64_t> g_completed;  // per destination: the newest ticket written
struct Held { uint64_t ticket; uint32_t dst, depth; };
std::vector<Held> g_held;  // the default (3): answers kept back until the game's swap after the next one
void write_answer(uint64_t ticket, uint32_t dst, uint32_t depth) {  // under g_publishMutex
    // Submission drains can retire newer fences before older ones. Never overwrite a newer answer.
    if (!dst || g_completed[dst] >= ticket) return;
    g_completed[dst] = ticket;
    st32(dst, depth);
    static const bool log = getenv("WWHD_PEEKZ_LOG") != nullptr;
    if (log) LOG("[peekz] %08X: %06X", dst, depth);
}
}
static std::atomic<int> g_forced_mode{-1};
void force_mode_for_test(int m) { g_forced_mode = m; }
int mode() {
    if (const int f = g_forced_mode.load(); f >= 0) return f;
    static const int m = [] {
        const char* e = getenv("WWHD_PEEKZ_SYNC");
        const int v = e && *e ? atoi(e) : 3;
        if (v != 3) LOG("[peekz] WWHD_PEEKZ_SYNC=%d: %s", v, v == 0 ? "answers whenever the GPU frame completes (not deterministic)"
                        : v == 1 ? "test: answers one step after the request, as on the Wii U"
                        : v == 2 ? "test: one step after, with a synchronous copy at each peek" : "unknown, using 3");
        return v >= 0 && v <= 3 ? v : 3;
    }();
    return m;
}
bool synchronous() { return mode() != 0; }
void issued() { g_gate.issue(); }
void answered() { g_gate.answer(); }
bool outstanding() { return g_gate.open(); }
bool wanted() { return g_gate.wanted(); }
void set_render_wake(void (*wake)()) { std::lock_guard lock(g_gate.mutex); g_gate.wakeRender = wake; }
void wait_answered() {
#ifdef __SWITCH__
    return;  // (this fork, #94) the answers are written at the game's next peekData (write_answers)
#endif
    if (!synchronous()) return;
    if (mode() == 3) {
        // the answers of the previous frame's requests are written now: the game reads them at its next
        // step, two steps after it asked
        static uint64_t previous = 0;
        const uint64_t target = previous;
        previous = g_gate.issued_count();
        if (!g_gate.wait_until(target, std::chrono::seconds(5)))
            LOG("[peekz] depth peeks not answered after 5 s; going on without them");
        std::lock_guard lock(g_publishMutex);
        auto keep = g_held.begin();
        for (auto& h : g_held)
            if (h.ticket <= target) write_answer(h.ticket, h.dst, h.depth);
            else *keep++ = h;
        g_held.erase(keep, g_held.end());
        return;
    }
    // the render thread answers in command order; a generous limit guards against a lost request
    if (!g_gate.wait(std::chrono::seconds(5)))
        LOG("[peekz] depth peeks not answered after 5 s; going on without them");
}
uint64_t next_ticket() { static std::atomic<uint64_t> serial{0}; return ++serial; }
#ifdef __SWITCH__
// (this fork, #94) the newest answer for each address, until the game's thread writes it (write_answers)
namespace {
struct Answer {
    uint32_t depth;
    uint64_t ticket;
};
std::unordered_map<uint32_t, Answer> g_waiting;  // under g_publishMutex
std::unordered_map<uint32_t, uint64_t> g_written;  // the ticket last written at each address
}  // namespace
void publish(uint64_t ticket, const std::vector<uint32_t>& destinations, const std::vector<uint32_t>& depths) {
    // Submission drains can retire newer fences before older ones. Never overwrite a newer answer.
    std::lock_guard lock(g_publishMutex);
    for (size_t i = 0; i < destinations.size(); i++) {
        const uint32_t dst = destinations[i];
        if (!dst) continue;
        auto w = g_written.find(dst);
        if (w != g_written.end() && w->second >= ticket) continue;
        auto [it, added] = g_waiting.try_emplace(dst, Answer{depths[i], ticket});
        if (!added && it->second.ticket < ticket) it->second = Answer{depths[i], ticket};
    }
}
void write_answers(uint32_t cells, uint32_t n) {
    static const bool log = getenv("WWHD_PEEKZ_LOG") != nullptr;
    std::lock_guard lock(g_publishMutex);
    if (g_waiting.empty()) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t dst = ld32(cells + i * 8 + 4);
        auto it = dst ? g_waiting.find(dst) : g_waiting.end();
        if (it == g_waiting.end()) continue;
        st32(dst, it->second.depth);
        g_written[dst] = it->second.ticket;
        if (log) LOG("[peekz] %08X: %06X", dst, it->second.depth);
        g_waiting.erase(it);
    }
    g_waiting.clear();  // addresses the game no longer asks for: their objects may be gone
    if (g_written.size() > 4096) g_written.clear();  // (addresses of objects long gone)
}
#else
void publish(uint64_t ticket, const std::vector<uint32_t>& destinations, const std::vector<uint32_t>& depths) {
    std::lock_guard lock(g_publishMutex);
    for (size_t i = 0; i < destinations.size(); i++) {
        if (mode() == 3) g_held.push_back({ticket, destinations[i], depths[i]});
        else write_answer(ticket, destinations[i], depths[i]);
    }
}
#endif
}

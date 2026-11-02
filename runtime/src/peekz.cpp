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
// (this fork) The answers are not written to guest memory where they arrive (the render thread, or a GPU
// completion, frames after the request): by then the object that asked may be gone and its memory another
// object's (the title screen's demo, 11 minutes in: a depth over a vtable pointer, a null call). They wait
// (publish) until the game's next peekData, which writes those whose address it asks for again (its object is
// still there) on the game's own thread, as the game's function does, and drops the others.
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "gfx/depth_peek.h"
#include "gfx/renderer.h"
#include "gx2/gx2_cmd.h"
#include "runtime.h"

extern "C" void f_0252E388_orig(Cpu* c);  // dDlst_peekZ_c::peekData

extern "C" void hook_0252E388(Cpu* c) {
    static const bool off = [] { const char* e = getenv("WWHD_PEEKZ"); return e && atoi(e) == 0; }();
    static const bool log = getenv("WWHD_PEEKZ_LOG") != nullptr;
    if (off || !render::can_peek_z()) { f_0252E388_orig(c); return; }
    const uint32_t obj = c->r[3];
    const uint32_t n = std::min<uint32_t>(ld8(obj), 64);
    gfx::depth_peek::write_answers(obj + 4, n);  // (this fork) the answers that came for these cells
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
        if (log) LOG("[peekz] request (%d,%d) in 640x480 -> %08X",
                     int(x), int(y), dst);
    }
    if (!cells.empty()) gx2::emit(gx2::OP_PEEK_Z, cells.data(), (uint32_t)cells.size());
    st8(obj, 0);  // as the game's function: the cells are taken
}

#include <atomic>
#include <mutex>
#include <unordered_map>
namespace gfx::depth_peek {
namespace {
// (this fork) the newest answer for each address, until the game's thread writes it (write_answers)
struct Answer {
    uint32_t depth;
    uint64_t ticket;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Answer> g_waiting;
std::unordered_map<uint32_t, uint64_t> g_written;  // the ticket last written at each address
}  // namespace
uint64_t next_ticket() { static std::atomic<uint64_t> serial{0}; return ++serial; }
void publish(uint64_t ticket, const std::vector<uint32_t>& destinations, const std::vector<uint32_t>& depths) {
    // Submission drains can retire newer fences before older ones. Never overwrite a newer answer.
    std::lock_guard lock(g_mutex);
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
    std::lock_guard lock(g_mutex);
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
}

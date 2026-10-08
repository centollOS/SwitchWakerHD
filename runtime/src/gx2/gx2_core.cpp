#include "../platform/host.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "gx2_texture_regs.h"
#include "shader_key_dirty.h"
#ifdef WWHD_HAS_VULKAN
#include "gfx/vulkan/api.h"
#endif
#include "runtime.h"
#include "../aspect.h"
#include "gfx/renderer.h"
#include "platform/perf_hint.h"
#include "render_prof.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
static std::recursive_mutex g_exec_mutex;

uint32* regs() { return g_regs; }

// Context loads (GX2SetContextState, ~23 a frame in Outset) copy only the blocks of 256 registers that
// something has written: every other block is zero in g_regs and in every context's shadow alike (a context
// starts zeroed, and only apply_regs writes either). They copied the whole 256 KB file each time (~70 us at
// 1020 MHz). The block of VGT_PRIMITIVE_TYPE, which draw() writes into g_regs directly, always counts.
static uint64 g_written_blocks[kNumRegs / 256 / 64];
static inline void mark_written(uint32 first, uint32 n) {
    for (uint32 b = first >> 8, last = (first + n - 1) >> 8; b <= last; b++) g_written_blocks[b >> 6] |= 1ull << (b & 63);
}
static const bool g_written_init = [] {
    mark_written(REGADDR::VGT_PRIMITIVE_TYPE, 1);
    return true;
}();

uint64_t g_reg_gen[kRegCats];
// the categories of each register (gx2.h RegCategory), a bit each
static const std::vector<uint8> g_reg_cat = [] {
    std::vector<uint8> c(kNumRegs, 0);
    auto add = [&](uint32 reg, int cat) { c[reg] |= uint8(1u << cat); };
    for (uint32 r = mmDB_DEPTH_SIZE; r <= mmDB_HTILE_DATA_BASE; r++) add(r, kRegCatTargets);  // (+ kDepthSlicesReg)
    for (uint32 r = mmCB_COLOR0_BASE; r < mmCB_COLOR0_FRAG + 8; r++) add(r, kRegCatTargets);  // BASE..FRAG, 8 each
    for (uint32 r : {uint32(REGADDR::CB_COLOR_CONTROL), uint32(REGADDR::CB_TARGET_MASK), uint32(REGADDR::DB_DEPTH_CONTROL),
                     uint32(REGADDR::PA_SC_GENERIC_SCISSOR_BR)})
        add(r, kRegCatTargets);
    for (uint32 r : {uint32(REGADDR::PA_SU_SC_MODE_CNTL), uint32(REGADDR::PA_CL_CLIP_CNTL),
                     uint32(REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET), uint32(REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE),
                     uint32(REGADDR::PA_SU_POLY_OFFSET_CLAMP), uint32(REGADDR::DB_DEPTH_CONTROL),
                     uint32(REGADDR::DB_STENCILREFMASK), uint32(REGADDR::DB_STENCILREFMASK_BF),
                     uint32(REGADDR::CB_COLOR_CONTROL), uint32(REGADDR::CB_TARGET_MASK), uint32(REGADDR::PA_SU_POINT_SIZE)})
        add(r, kRegCatFixed);
    for (uint32 i = 0; i < 8; i++) add(REGADDR::CB_BLEND0_CONTROL + i, kRegCatFixed);
    for (uint32 r = REGADDR::CB_BLEND_RED; r <= REGADDR::CB_BLEND_ALPHA; r++) add(r, kRegCatFixed);
    for (uint32 r = REGADDR::PA_CL_VPORT_XSCALE; r <= REGADDR::PA_CL_VPORT_ZOFFSET; r++) add(r, kRegCatViewport);
    for (uint32 r : {uint32(REGADDR::PA_CL_CLIP_CNTL), uint32(REGADDR::PA_SC_GENERIC_SCISSOR_TL),
                     uint32(REGADDR::PA_SC_GENERIC_SCISSOR_BR)})
        add(r, kRegCatViewport);
    return c;
}();
static inline void bump_categories(uint32 cats) {
    for (int c = 0; c < kRegCats; c++)
        if (cats >> c & 1) g_reg_gen[c]++;
}
static inline void bump_all_categories() { bump_categories((1u << kRegCats) - 1); }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }
// Like g_shader_state_gen, but not bumped when only shader program registers change (start address,
// size, resources, exports: none of them is in the renderer's state hash), so the OpenGL renderer can
// keep its hash of the other registers when a draw just switches programs.
extern "C" { uint64_t g_shader_regs_gen = 1; }
static bool program_reg(uint32 reg) {
    return (reg >= mmSQ_PGM_START_PS && reg <= mmSQ_PGM_RESOURCES_VS) || (reg >= mmSQ_PGM_START_FS && reg <= mmSQ_PGM_RESOURCES_FS) ||
           (reg >= mmSQ_PGM_CF_OFFSET_PS && reg <= mmSQ_PGM_CF_OFFSET_FS);
}

static bool shader_irrelevant(uint32 reg) {
#ifdef WWHD_HAS_VULKAN
    // Vulkan resolves texture addresses freshly in bind_stage. These words do
    // not participate in shader translation; keep Metal's broader dirty gate.
    static const bool addressMemo = [] {
        const char* e = getenv("WWHD_VK_SHADER_ADDRESS_MEMO");
        return render::vulkan() && e && !strcmp(e, "1");
    }();
    if (addressMemo)
        for (uint32 base : {uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
            if (reg >= base && reg < base + 7 * 18) {
                uint32 word = (reg - base) % 7;
                if (word == 2 || word == 3) return true;
            }
#endif
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32 base : {(uint32)mmSQ_VTX_UNIFORM_BLOCK_START, (uint32)mmSQ_PS_UNIFORM_BLOCK_START, (uint32)mmSQ_GS_UNIFORM_BLOCK_START})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32 w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}
// How a register change counts for the shader-state counters, looked up per register (the game sets
// millions of registers per second): 0 = not at all, else counted; kRegMasked: only changes of the
// bits in mask[reg]; kRegProgram: a program register (not counted by g_shader_regs_gen).
// OpenGL: like WWHD_VK_SHADER_KEY_DIRTY, register fields that its shader key ignores (texture
// addresses, viewport, blend, scissor, the alpha-test reference...) don't count, so consecutive draws
// that only change those reuse the shader lookup. WWHD_GL_SHADER_KEY_DIRTY=0 counts every change.
// (bits 3-5: the register's categories, gx2.h RegCategory: one table and one pass in apply_regs)
enum : uint8 { kRegCounted = 1, kRegMasked = 2, kRegProgram = 4, kRegShaderBits = 7, kRegCatShift = 3 };
struct RegClasses {
    std::vector<uint8> kind = std::vector<uint8>(kNumRegs);
    std::vector<uint32> mask = std::vector<uint32>(kNumRegs);
};
static const RegClasses& reg_classes() {
    static const RegClasses classes = [] {
        const char* e = getenv("WWHD_GL_SHADER_KEY_DIRTY");
        // deko3d translates shaders as the OpenGL renderer does (the decompiler's OpenGL mode)
        const bool glKeyMask = render::deko3d() && (!e || strcmp(e, "0") != 0);
        RegClasses c;
        for (uint32 reg = 0; reg < kNumRegs; reg++) {
            uint32 mask = ~0u;
            uint8 kind = shader_irrelevant(reg) ? 0 : kRegCounted;
            if (kind && glKeyMask) {
                if (reg == REGADDR::SX_ALPHA_REF) kind = 0;  // a uniform (uf_alphaTestRef)
                else if (vulkan_shader_key_mask(reg, mask)) kind = mask ? kRegCounted | kRegMasked : 0;
            }
            if (kind && program_reg(reg)) kind |= kRegProgram;
            c.kind[reg] = uint8(kind | g_reg_cat[reg] << kRegCatShift);
            c.mask[reg] = mask;
        }
        return c;
    }();
    return classes;
}

static ShaderKeyDirtyStats shaderKeyDirtyStats;
ShaderKeyDirtyStats shader_key_dirty_stats() { return shaderKeyDirtyStats; }

#ifdef WWHD_HAS_VULKAN
static void apply_small_regs(uint32 first, const uint32* v, uint32 n) {
    static const bool keyDirty = [] {
        const char* e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
        return e && !strcmp(e, "1");
    }();
    static const bool collectStats = getenv("WWHD_VK_STATS") != nullptr;
    const bool classify = rprof::enabled();
    bool changed = false, baselineBump = false, actualBump = false;
    uint64_t maskedWords = 0;
    for (uint32 i = 0; i < n; ++i) {
        const uint32 reg = first + i, value = v[i], old = g_regs[reg];
        if (old != value) {
            changed = true;
            if (const uint32 cats = g_reg_cat[reg]) bump_categories(cats);
            if (classify) {
                if (rprof::fast_class_reg(reg)) rprof::g_reg_dirty |= 1;
                else rprof::note_other_reg(reg);
            }
            if ((collectStats || !actualBump) && !shader_irrelevant(reg)) {
                baselineBump = true;
                uint32 mask;
                if (keyDirty && vulkan_shader_key_mask(reg, mask) && !((old ^ value) & mask)) {
                    if (collectStats) ++maskedWords;
                } else actualBump = true;
            }
            g_regs[reg] = value;
        }
        // Shadow can differ even when registers already match: draw mutates
        // primitive type directly, and context setup initializes only shadow.
        if (g_shadow) g_shadow[reg] = value;
    }
    if (actualBump) { ++g_shader_state_gen; ++g_shader_regs_gen; }
    if (changed && collectStats) {
        ++shaderKeyDirtyStats.changedBatches;
        shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
        shaderKeyDirtyStats.actualBumps += actualBump;
        shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
        shaderKeyDirtyStats.maskedWords += maskedWords;
    }
}
#endif

static void apply_regs(uint32 first, const uint32* v, uint32 n) {
    if (first + n > kNumRegs) return;
    // uniform registers (often hundreds of words a call) never move the shader-state counters, so
    // the comparison that decides those is skipped: compared and then copied, they were ~15% of the
    // render thread on the desktop
    if (!n) return;
    mark_written(first, n);
    if (first >= (uint32)mmSQ_ALU_CONSTANT0_0 && first + n <= (uint32)mmSQ_ALU_CONSTANT0_0 + 0x1000) {
        memcpy(&g_regs[first], v, n * 4);
        if (g_shadow) memcpy(&g_shadow[first], v, n * 4);
        return;
    }
#ifdef WWHD_HAS_VULKAN
    // Vulkan renderer only (the Metal renderer keeps the original bulk path)
    static const bool fusedSmall = [] {
        const char* e = getenv("WWHD_VK_FUSE_SMALL_REGS");
        // Enabled by default; explicit zero retains the original bulk path.
        return render::vulkan() && (!e || strcmp(e, "0") != 0);
    }();
    if (fusedSmall && n <= 16) {
        apply_small_regs(first, v, n);
        return;
    }
#endif
    if (memcmp(&g_regs[first], v, n * 4) != 0) {
        if (rprof::enabled())  // draw classifier (render_prof.h)
            for (uint32 i = 0; i < n; i++)
                if (g_regs[first + i] != v[i]) {
                    if (rprof::fast_class_reg(first + i)) rprof::g_reg_dirty |= 1;
                    else rprof::note_other_reg(first + i);
                }
#ifdef WWHD_HAS_VULKAN
        static const bool keyDirty = [] {
            const char* value = getenv("WWHD_VK_SHADER_KEY_DIRTY");
            return render::vulkan() && value && !strcmp(value, "1");
        }();
        static const bool collectStats = render::vulkan() && getenv("WWHD_VK_STATS") != nullptr;
        if(keyDirty || collectStats) {
            bool baselineBump = false, actualBump = false;
            uint64_t maskedWords = 0;
            for(uint32 i = 0; i < n; ++i) {
                uint32 reg = first + i;
                if(g_regs[reg] == v[i] || shader_irrelevant(reg)) continue;
                baselineBump = true;
                uint32 mask;
                if(keyDirty && vulkan_shader_key_mask(reg, mask) && !((g_regs[reg] ^ v[i]) & mask)) {
                    if(collectStats) ++maskedWords;
                } else actualBump = true;
                if(actualBump && !collectStats) break;
            }
            if(actualBump) { ++g_shader_state_gen; ++g_shader_regs_gen; }
            if(collectStats) {
                ++shaderKeyDirtyStats.changedBatches;
                shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
                shaderKeyDirtyStats.actualBumps += actualBump;
                shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
                shaderKeyDirtyStats.maskedWords += maskedWords;
            }
        } else
#endif
        // (writes that lie wholly in the uniform registers took the fast path above): the shader-state counters
        // and the register categories (gx2.h) from one table, in one pass over the changed registers
        {
            const RegClasses& classes = reg_classes();
            bool stateBump = false, regsBump = false;
            uint32 cats = 0;
            for (uint32 i = 0; i < n; i++) {
                const uint32 reg = first + i, old = g_regs[reg];
                if (old == v[i]) continue;
                const uint8 c = classes.kind[reg];
                cats |= c >> kRegCatShift;
                if (!(c & kRegShaderBits) || ((c & kRegMasked) && !((old ^ v[i]) & classes.mask[reg]))) continue;
                stateBump = true;
                if (!(c & kRegProgram)) regsBump = true;
            }
            g_shader_state_gen += stateBump;
            g_shader_regs_gen += regsBump;
            if (cats) bump_categories(cats);
        }
        memcpy(&g_regs[first], v, n * 4);
    }
    if (g_shadow) memcpy(&g_shadow[first], v, n * 4);
}

// ---------------------------------------------------------------- display list recording
struct Recording {
    uint32 start = 0, pos = 0, end = 0;
    uint32 last = 0;  // the last recorded command's header (round 45: register writes merge into it), 0 = none
    uint32 recorded = 0, merged = 0;  // (the [gx2] line's counts: per thread, added up at GX2EndDisplayList)
};
static thread_local Recording t_rec;

static void execute_one(Op op, const uint32* p, uint32 n);

// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into Metal work. WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv, g_q_done_cv;
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;
static uint64_t g_fence_issued = 0, g_fence_done = 0;
static std::atomic<uint64_t> g_render_wait_ns{0};
uint64_t render_thread_wait_ns() { return g_render_wait_ns.load(std::memory_order_relaxed); }

std::atomic<const char*> g_render_stage{nullptr};
// The render thread's progress for the watchdog, on a cache line of its own: these are written for
// every command, and next to the queue's mutex and vectors (which the game's main thread writes for
// every GX2 call) each write took the line away from the main thread. Only the render thread writes
// them, so the count is a plain load and store (the locked increment was ~12% of the render thread).
struct alignas(64) RenderProgress {
    std::atomic<uint32> op{0xFF};          // command being executed (0xFF: none)
    std::atomic<bool> idle{true};          // waiting for commands
    std::atomic<uint64_t> commands{0};     // commands executed so far
};
static RenderProgress g_progress;

static void render_thread_main() {
    host::set_thread_name("GX2 render");
    host::place_thread(2);  // the render thread: host core 2, or 0 and 2 with WWHD_CORE_LAYOUT
    host::boost_thread_priority();
    perf_hint::add_current_thread();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            if (g_q_pending.empty()) {
                g_progress.idle.store(true, std::memory_order_relaxed);
                auto waitStart = std::chrono::steady_clock::now();
                const uint64_t idle = rprof::enabled() ? rprof::now_ns() : 0;
                g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
                if (idle) rprof::add_idle(rprof::now_ns() - idle);
                g_progress.idle.store(false, std::memory_order_relaxed);
                g_render_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - waitStart).count()),
                                           std::memory_order_relaxed);
            }
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            render::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
    }
}

// Staged commands (WWHD_GX2_STAGING, on unless =0). Every GX2 call of the game's main thread took the
// queue's mutex to append its command (tens of thousands a frame), and the vectors' cache lines
// moved between it and the render thread. The guest threads of emulated core 1 (the main thread and
// the game's Prepare Thread, the only ones that issue GX2 commands) now collect commands in a buffer
// of their own and hand them over in blocks: when the block is large, when the render thread has
// run out of work, at commands that wait for or present the GPU's work, and whenever the thread gives
// up its emulated core (threads.cpp core_release). Two threads of one core never run at once, so
// the commands keep the order they were issued in. Threads of other cores append directly as before.
namespace {
constexpr size_t kStageWords = 1024;
struct Staging {
    std::vector<uint32> words;
    int eligible = -1;  // -1: not decided yet
    size_t last = SIZE_MAX;  // the last staged command's header (round 45: register writes merge into it)
    uint32 staged = 0, merged = 0;  // (the [gx2] line's counts, added up when the block is handed over)
};
// Round 45. While the render thread waits for work, a block was handed over at every command: near Dragon Roost
// and the volcano (the render thread ~75% busy, ~55,000 commands a frame) that was a queue lock and often a kernel
// wake-up for nearly every GX2 call, on the main thread, which limits the frame rate there. An idle render thread
// now gets the block once it holds WWHD_GX2_IDLE_PUBLISH words (default 512; 0 = at once, as before), or at a
// flush, wait, fence or swap, or when the thread gives up its core.
const size_t g_idlePublishWords = [] {
    const char* e = getenv("WWHD_GX2_IDLE_PUBLISH");
    return e && *e ? size_t(strtoul(e, nullptr, 10)) : size_t(512);
}();
// Round 45: single-register writes (shader binds write 10-16 of them one at a time) merge into one OP_SET_REG_PAIRS
// command, and a write of the registers right after the previous OP_SET_REGS command's extends it
// (WWHD_GX2_COALESCE=0: every write its own command, as before)
const bool g_coalesce = [] {
    const char* e = getenv("WWHD_GX2_COALESCE");
    return !(e && *e == '0');
}();
std::atomic<uint64_t> g_publishes{0}, g_wakes{0}, g_staged{0}, g_merged{0}, g_recorded{0};
thread_local Staging t_stage;
bool staging_on() {
    static const bool on = [] {
        const char* e = getenv("WWHD_GX2_STAGING");
        return !(e && *e == '0');
    }();
    return on;
}
}  // namespace

void publish_staged() {
    Staging& s = t_stage;
    if (s.words.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_q_pending.insert(g_q_pending.end(), s.words.begin(), s.words.end());
        g_publishes.fetch_add(1, std::memory_order_relaxed);
        g_staged.fetch_add(s.staged, std::memory_order_relaxed);
        g_merged.fetch_add(s.merged, std::memory_order_relaxed);
        if (g_q_waiting) {
            g_q_cv.notify_one();
            g_wakes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    s.words.clear();
    s.last = SIZE_MAX;
    s.staged = s.merged = 0;
}

// (the command-stream figures of the 5 s [gx2] line: hand-overs to the render thread, kernel wake-ups among them,
// commands staged, register writes merged into an earlier command)
void staging_stats(uint64_t& publishes, uint64_t& wakes, uint64_t& staged, uint64_t& merged, uint64_t& recorded) {
    recorded = g_recorded.exchange(0, std::memory_order_relaxed);
    publishes = g_publishes.exchange(0, std::memory_order_relaxed);
    wakes = g_wakes.exchange(0, std::memory_order_relaxed);
    staged = g_staged.exchange(0, std::memory_order_relaxed);
    merged = g_merged.exchange(0, std::memory_order_relaxed);
}

// a register write into the staging buffer: merged into the last command where it can be (round 45)
static bool stage_regs(Staging& s, uint32 first, const uint32* values, uint32 count) {
    if (!g_coalesce || s.last == SIZE_MAX) return false;
    uint32& hdr = s.words[s.last];
    const uint32 lastOp = hdr & 0xFF, lastN = hdr >> 8;
    if (lastOp == OP_SET_REGS && lastN >= 1 && s.words[s.last + 1] + (lastN - 1) == first && lastN + count < (1u << 24)) {
        s.words.insert(s.words.end(), values, values + count);  // the next registers: one longer write
        hdr = OP_SET_REGS | ((lastN + count) << 8);
        s.merged++;
        return true;
    }
    if (count == 1 && lastOp == OP_SET_REG_PAIRS && lastN + 2 < (1u << 24)) {
        s.words.push_back(first);
        s.words.push_back(values[0]);
        hdr = OP_SET_REG_PAIRS | ((lastN + 2) << 8);
        s.merged++;
        return true;
    }
    if (count == 1 && lastOp != OP_SET_REG_PAIRS) {  // a single write starts a pairs command (the next ones join it)
        s.last = s.words.size();
        s.words.push_back(OP_SET_REG_PAIRS | (2u << 8));
        s.words.push_back(first);
        s.words.push_back(values[0]);
        return true;
    }
    return false;
}

static void enqueue(Op op, const uint32* payload, uint32 n) {
    static std::once_flag once;
    std::call_once(once, [] { host::start_thread(render_thread_main, 8 << 20); });
    Staging& s = t_stage;
    if (s.eligible < 0) {
        s.eligible = staging_on() && threads::current_core() == 1;
        if (s.eligible) s.words.reserve(kStageWords + 1024);
    }
    if (s.eligible) {
        s.staged++;
        if (!(op == OP_SET_REGS && n >= 2 && stage_regs(s, payload[0], payload + 1, n - 1))) {
            s.last = s.words.size();
            s.words.push_back(op | (n << 8));
            s.words.insert(s.words.end(), payload, payload + n);
        }
        if (s.words.size() >= kStageWords || (op >= OP_FLUSH && op <= OP_FENCE) ||
            (g_progress.idle.load(std::memory_order_relaxed) && s.words.size() >= g_idlePublishWords))
            publish_staged();
        return;
    }
    publish_staged();  // (a thread that stages never gets here)
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_q_pending.push_back(op | (n << 8));
    g_q_pending.insert(g_q_pending.end(), payload, payload + n);
    if (g_q_waiting) g_q_cv.notify_one();
}

// block the game thread until the render thread has executed everything queued so far
static std::atomic<uint64_t> g_sync_wait_ns{0}, g_syncs{0};
uint64_t game_sync_wait_ns() { return g_sync_wait_ns.load(std::memory_order_relaxed); }
uint64_t game_syncs() { return g_syncs.load(std::memory_order_relaxed); }
// debug: WWHD_SYNC_STATS=1 logs, every 5 s, how often each caller waited for the render thread to
// catch up (render_sync) and for how long
enum SyncSite { kSyncShutdown, kSyncFlip, kSyncDrawDone, kSyncVsyncUncapped, kSyncVsyncFlip, kSyncSaveState, kSyncCopySurface, kSyncSites };
static void sync_stat(int site, std::chrono::steady_clock::duration waited) {
    static const bool on = getenv("WWHD_SYNC_STATS") != nullptr;
    if (!on) return;
    static std::mutex mu;
    static uint64_t count[kSyncSites], ns[kSyncSites];
    static auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    count[site]++;
    ns[site] += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - t0).count();
    static const char* names[kSyncSites] = {"shutdown", "flip", "DrawDone", "vsync-uncapped", "vsync-flip", "savestate", "CopySurface"};
    char buf[400];
    int k = snprintf(buf, sizeof buf, "[gx2] render_sync per second:");
    for (int i = 0; i < kSyncSites; i++)
        if (count[i]) k += snprintf(buf + k, sizeof buf - k, " %s %.1f x %.2f ms", names[i], count[i] / secs, ns[i] / 1e6 / count[i]);
    LOG("%s", buf);
    memset(count, 0, sizeof count); memset(ns, 0, sizeof ns);
    t0 = now;
}
static void render_sync(int site = kSyncShutdown) {
    if (!g_render_thread) return;
    struct Timed {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~Timed() {
            g_sync_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start).count()),
                                     std::memory_order_relaxed);
            g_syncs.fetch_add(1, std::memory_order_relaxed);
        }
    } timed;
    const auto started = std::chrono::steady_clock::now();
    struct Done {
        int site;
        std::chrono::steady_clock::time_point t;
        ~Done() {
            const auto waited = std::chrono::steady_clock::now() - t;
            sync_stat(site, waited);
            if (rprof::enabled())
                rprof::add_sync(site == kSyncDrawDone ? rprof::kSyncDrawDone : site == kSyncCopySurface ? rprof::kSyncCopySurface
                                : site == kSyncFlip || site == kSyncVsyncFlip ? rprof::kSyncFlip : rprof::kSyncOther,
                                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
        }
    } done{site, started};
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        id = ++g_fence_issued;
    }
    uint32 w = (uint32)id;
    enqueue(OP_FENCE, &w, 1);
    std::unique_lock<std::mutex> lk(g_q_mutex);
    g_q_done_cv.wait(lk, [&] { return g_fence_done >= id; });
}

void emit(Op op, const uint32* payload, uint32 n) {
    if (t_rec.start) {
        t_rec.recorded++;
        // (round 45) a register write merged into the list's last command, as in the staging buffer: most of the
        // game's commands come from display lists (~44,000 executed a frame at Outset for ~85 staged calls)
        if (g_coalesce && op == OP_SET_REGS && n >= 2 && t_rec.last) {
            uint32* hdr = (uint32*)mem::ptr(t_rec.last);
            const uint32 lastOp = *hdr & 0xFF, lastN = *hdr >> 8, count = n - 1;
            if (lastOp == OP_SET_REGS && lastN >= 1 && hdr[1] + (lastN - 1) == payload[0] &&
                t_rec.pos + 4 * count <= t_rec.end && lastN + count < (1u << 24)) {
                memcpy(mem::ptr(t_rec.pos), payload + 1, count * 4);  // the next registers: one longer write
                t_rec.pos += 4 * count;
                *hdr = OP_SET_REGS | ((lastN + count) << 8);
                t_rec.merged++;
                return;
            }
            if (count == 1 && lastOp == OP_SET_REG_PAIRS && t_rec.pos + 8 <= t_rec.end && lastN + 2 < (1u << 24)) {
                uint32* w = (uint32*)mem::ptr(t_rec.pos);
                w[0] = payload[0];
                w[1] = payload[1];
                t_rec.pos += 8;
                *hdr = OP_SET_REG_PAIRS | ((lastN + 2) << 8);
                t_rec.merged++;
                return;
            }
            if (count == 1) op = OP_SET_REG_PAIRS;  // (same words: the next single writes can join it)
        }
        uint32 bytes = 4 * (n + 1);
        if (t_rec.pos + bytes > t_rec.end) {
            LOG("[gx2] display list overflow at %08X", t_rec.start);
            return;
        }
        uint32* w = (uint32*)mem::ptr(t_rec.pos);
        w[0] = op | (n << 8);
        memcpy(w + 1, payload, n * 4);
        t_rec.last = t_rec.pos;
        t_rec.pos += bytes;
        return;
    }
    if (g_render_thread) {
        enqueue(op, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload, n); });
}

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload.begin(), (uint32)payload.size()); });
}

#ifdef WWHD_HAS_VULKAN
void checkpoint_vulkan_caches() {
    // Wait for queued work, then exclude further renderer mutations while the
    // SDL thread writes the final cache checkpoint during orderly shutdown.
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] {
        gfxvk::wait_idle();
        gfxvk::save_renderer_caches();
    });
}
#endif

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (!count) return;
    uint32 small[1 + 64];  // (most writes: up to 64 registers without the thread's heap buffer)
    if (count <= 64) {
        small[0] = first;
        memcpy(&small[1], values, count * 4);
        emit(OP_SET_REGS, small, count + 1);
        return;
    }
    static thread_local std::vector<uint32> buf;
    buf.resize(count + 1);
    buf[0] = first;
    memcpy(&buf[1], values, count * 4);
    emit(OP_SET_REGS, buf.data(), count + 1);
}
void set_reg(uint32 reg, uint32 value) { emit(OP_SET_REGS, {reg, value}); }

void execute(const uint32* words, uint32 count) {
    uint32 i = 0;
    uint64_t executed = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || i + 1 + n > count) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        g_progress.op.store(op, std::memory_order_relaxed);
        // register writes (~80% of the commands) straight to apply_regs: no dispatch through execute_one and
        // execute_op unless the profiler counts them (round 43)
        if (op == OP_SET_REGS && n && !rprof::enabled()) apply_regs(words[i + 1], &words[i + 2], n - 1);
        else if (op == OP_SET_REG_PAIRS && !rprof::enabled())
            for (uint32 k = 0; k + 1 < n; k += 2) apply_regs(words[i + 1 + k], &words[i + 2 + k], 1);
        else execute_one(op, &words[i + 1], n);
        i += 1 + n;
        executed++;
    }
    // (once per batch: the watchdog only needs to see progress; op is left at the last command, 0xFF after)
    g_progress.op.store(0xFF, std::memory_order_relaxed);
    g_progress.commands.store(g_progress.commands.load(std::memory_order_relaxed) + executed, std::memory_order_relaxed);
}

static void set_context(uint32 ctx) {
    if (!ctx) {
        g_shadow = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        return;
    }
    g_shadow = it->second.data();
    for (uint32 w = 0; w < sizeof g_written_blocks / sizeof g_written_blocks[0]; w++)
        for (uint64 bits = g_written_blocks[w]; bits; bits &= bits - 1) {
            const uint32 block = w * 64 + uint32(__builtin_ctzll(bits));
            memcpy(&g_regs[block * 256], &g_shadow[block * 256], 256 * 4);
        }
    bump_all_categories();
    g_shader_state_gen++;
    g_shader_regs_gen++;
    rprof::g_reg_dirty |= 2;  // draw classifier: a context load counts as a full state change
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}

// Vulkan: GX2DrawDone queues the work instead of waiting for an idle device (the default on every
// platform since 2026-10-07; WWHD_VK_LAZY_DRAW_DONE=0 restores the full wait). The Metal renderer
// reads large vertex buffers straight from guest memory, so it keeps the real GPU wait.
static bool lazy_draw_done() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_LAZY_DRAW_DONE");
        return render::vulkan() && (!e || atoi(e) != 0);
    }();
    return on;
}

static void execute_op(Op op, const uint32* p, uint32 n);
// every op is counted and (sampled) timed for the render-thread profiler (render_prof.h); display-list
// calls are not timed themselves: their ops are
static void execute_one(Op op, const uint32* p, uint32 n) {
    rprof::Op kind;
    switch (op) {
    case OP_CALL: execute_op(op, p, n); return;
    case OP_SET_REGS: case OP_SET_PROJ_REGS: case OP_SET_REG_PAIRS: kind = rprof::kOpRegs; break;
    case OP_DRAW: case OP_DRAW_INDEXED: kind = rprof::kOpDraw; rprof::classify_draw(); break;
    case OP_CLEAR_COLOR: case OP_CLEAR_DEPTH: case OP_CLEAR_BUFFERS: kind = rprof::kOpClear; break;
    case OP_COPY_SURFACE: kind = rprof::kOpCopy; break;
    case OP_COPY_TO_SCAN: kind = rprof::kOpScan; break;
    case OP_INVALIDATE: kind = rprof::kOpInvalidate; break;
    case OP_FLUSH: kind = rprof::kOpFlush; break;
    case OP_DRAW_DONE: kind = rprof::kOpDrawDone; break;
    case OP_SWAP: kind = rprof::kOpSwap; break;
    default: kind = rprof::kOpOther; break;
    }
    const uint64_t started = rprof::op_begin(kind);
    execute_op(op, p, n);
    rprof::op_end(kind, started);
    if (op == OP_SWAP) rprof::frame_end(n >= 2 && p[1]);
}

static void execute_op(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS: apply_regs(p[0], p + 1, n - 1); break;
    case OP_SET_REG_PAIRS:
        for (uint32 i = 0; i + 1 < n; i += 2) apply_regs(p[i], &p[i + 1], 1);
        break;
    case OP_DRAW: render::draw(g_regs, p[0], p[1], 0, 0, p[2], p[3]); break;
    case OP_DRAW_INDEXED: render::draw(g_regs, p[0], p[1], p[2], p[3], p[4], p[5]); break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        render::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 0), bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        uint32 cb = unpack_struct(p, kColorBufferWords, 0), db = unpack_struct(p + kColorBufferWords, kDepthBufferWords, 1);
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, cb, rgba);
        render::clear_depth_stencil(g_regs, db, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        // debug: WWHD_GX2_DELAY_COPY=ms stalls the render thread before each surface copy (a slow
        // or busy render thread; reproduced the agl boot crash every time before GX2CopySurface waited)
        static const int delay = getenv("WWHD_GX2_DELAY_COPY") ? atoi(getenv("WWHD_GX2_DELAY_COPY")) : 0;
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        uint32 src = unpack_struct(p, kSurfaceWords, 0);
        const uint32* q = p + kSurfaceWords;
        uint32 dst = unpack_struct(q + 2, kSurfaceWords, 1);
        const uint32* r = q + 2 + kSurfaceWords;
        render::copy_surface(src, q[0], q[1], dst, r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN: render::copy_to_scan(unpack_struct(p, kColorBufferWords, 0), p[kColorBufferWords]); break;
    case OP_CALL: execute((const uint32*)mem::ptr(p[0]), p[1] / 4); break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: render::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_FLUSH: render::guest_flush(); break;  // Vulkan: asynchronous submission
    case OP_DRAW_DONE:
        // The Vulkan renderer writes GPU results back to guest memory only for linear surfaces (guest data is copied
        // into fenced upload slices when work is recorded), so GX2DrawDone needs this op executed
        // (render_sync in the HLE), not an idle GPU. Lazy DrawDone (lazy_draw_done(), the default)
        // queues the work instead of waiting for the whole device every frame. A payload word of 1
        // (save states) always waits for the idle GPU.
        if (lazy_draw_done() && !(n && p[0])) render::guest_flush();
        else render::wait_idle();
        // except what the CPU reads back: linear render targets (the Picto Box picture, issue #53)
        render::write_back();
        break;
    case OP_SWAP:
        if (n) render::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        render::swap();
        break;
    case OP_SET_PROJ_REGS: {
        uint32 v[16];
        memcpy(v, p + 1, sizeof v);
        float kx, ky;
        if (n == 17 && render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky))
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = fbits(bitsf(v[i]) / kx);
                v[4 + i] = fbits(bitsf(v[4 + i]) / ky);
            }
        apply_regs(p[0], v, std::min<uint32>(n - 1, 16));
        break;
    }
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        aspect::layout_root_target(p[0], render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky));
        break;
    }
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        break;
    case OP_FENCE: {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_fence_done = std::max<uint64_t>(g_fence_done, p[0]);
        g_q_done_cv.notify_all();
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
// 120/240 fps frame interpolation needs 4/8 flips per logic step: the virtual vsync then ticks 2/4
// times per 59.94 Hz vsync (interp::vsync_rate()). The clock counts quarter vsyncs ("ticks"); a
// vsync at the current rate is a granule of 4/rate ticks, and every vsync count below is in ticks,
// rounded to granules, so at 30/60 fps (granule 4) the timing is the 59.94 Hz one exactly.
static uint64_t g_swap_count = 0, g_flip_count = 0;

// Hang watchdog: when no frame has been swapped for 3 s, log what the render thread and every guest
// thread are doing (again every 15 s, at most 6 times), and write the log out at once.
static std::atomic<uint64_t> g_watch_swaps{0};
static void watchdog_thread() {
    host::set_thread_name("watchdog");
    uint64_t lastSwaps = 0, lastCommands = 0;
    auto lastChange = std::chrono::steady_clock::now();
    auto nextReport = lastChange;
    int reports = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = std::chrono::steady_clock::now();
        const uint64_t swaps = g_watch_swaps.load(std::memory_order_relaxed);
        if (swaps != lastSwaps) {
            lastSwaps = swaps;
            lastChange = now;
            nextReport = now + std::chrono::seconds(3);
            continue;
        }
        if (now < nextReport || reports >= 6) continue;
        reports++;
        nextReport = now + std::chrono::seconds(15);
        const uint64_t commands = g_progress.commands.load(std::memory_order_relaxed);
        const char* stage = g_render_stage.load(std::memory_order_relaxed);
        const uint32 op = g_progress.op.load(std::memory_order_relaxed);
        size_t pending = 0;
        uint64_t issued = 0, done = 0;
        {
            std::unique_lock<std::mutex> lk(g_q_mutex, std::defer_lock);
            if (lk.try_lock()) {
                pending = g_q_pending.size();
                issued = g_fence_issued;
                done = g_fence_done;
            }
        }
        LOG("[watchdog] no frame for %.1f s (frame %llu). Render thread: %s%s, command %u, %llu commands since the last "
            "report; queued words %zu, game syncs issued %llu done %llu",
            std::chrono::duration<double>(now - lastChange).count(), (unsigned long long)swaps,
            g_progress.idle.load() ? "waiting for commands" : "executing", stage ? (std::string(" (") + stage + ")").c_str() : "",
            op, (unsigned long long)(commands - lastCommands), pending, (unsigned long long)issued, (unsigned long long)done);
        lastCommands = commands;
        threads::dump_state();
        log_flush();
    }
}
namespace gx2 { uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); } }  // live fps in the title
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); uint64_t logic_steps(); int vsync_rate(); }
static std::mutex g_flip_mutex;
static const auto g_vsync_epoch = std::chrono::steady_clock::now();
static constexpr std::chrono::nanoseconds kVsyncPeriod(16683333);  // 59.94 Hz
static constexpr uint64_t kTicksPerVsync = 4;
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
struct PendingFlip { uint64_t vsync, swap; };  // (vsync: tick of the swap)
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;  // tick (a granule boundary)
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

static uint64_t vsync_index() {  // in ticks
    return uint64_t((std::chrono::steady_clock::now() - g_vsync_epoch).count()) * kTicksPerVsync / uint64_t(kVsyncPeriod.count());
}
// WWHD_RELAXED_VSYNC (on by default on the Switch, =0 for the hardware's timing): as on the Wii U, a
// frame flips at the first vblank after its swap and at least swap-interval vblanks after the
// previous flip, and the game waits for that flip (vblank by vblank) before it starts the next
// frame. So a frame that takes a little longer than two vblanks costs three: 30 fps drops straight
// to 20, and the game (whose logic runs per frame) plays at two thirds of its speed. Relaxed: a
// frame that missed its vblank flips as soon as it is rendered, and GX2WaitForVsync returns then,
// so a 35 ms frame gives ~28 fps. Frames are still at least a swap interval apart.
static bool relaxed_vsync() {
    static const bool on = [] {
        const char* e = getenv("WWHD_RELAXED_VSYNC");
#ifdef __SWITCH__
        return !(e && *e == '0');
#else
        return e && *e == '1';
#endif
    }();
    return on;
}
static uint64_t g_late_flips = 0;  // flips the relaxed timing let through before their vblank (for the log)
static std::chrono::steady_clock::time_point g_last_flip_tp = g_vsync_epoch;
static uint64_t vsync_granule() {  // ticks per vsync at the current rate: 4 (30/60 fps), 2 (120), 1 (240)
    const int rate = interp::vsync_rate();
    return rate >= 4 ? 1 : rate >= 2 ? 2 : kTicksPerVsync;
}
static std::chrono::steady_clock::time_point tick_time(uint64_t tick) {
    return g_vsync_epoch + std::chrono::nanoseconds(tick * uint64_t(kVsyncPeriod.count()) / kTicksPerVsync);
}
// the first tick at which this flip may execute: the vsync after its swap, and `swap interval`
// vsyncs after the previous flip
static uint64_t flip_due(const PendingFlip& f) {
    const uint64_t g = vsync_granule();
    return std::max((f.vsync / g + 1) * g, g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval) * g);
}
// Uncapped (gx2.h): debug, to see how fast the renderer can go; GPU completion ordering is kept
static std::atomic<int> g_uncapped{-1};  // -1: not read from the environment yet
bool gx2::uncapped() {
    int v = g_uncapped.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* any = getenv("WWHD_UNCAPPED");
        const char* vk = getenv("WWHD_VK_UNCAPPED");
        v = (any && !strcmp(any, "1")) || (vk && !strcmp(vk, "1") && render::vulkan()) ? 1 : 0;
        int expected = -1;
        g_uncapped.compare_exchange_strong(expected, v);
        v = g_uncapped.load(std::memory_order_relaxed);
    }
    return v == 1;
}
void gx2::set_uncapped(bool on) {
    if (uncapped() == on) return;
    g_uncapped = on ? 1 : 0;
    LOG("[gx2] uncapped %s", on ? "on (debug: no frame limit, no vsync; the game runs faster than real time)" : "off");
}
static bool uncapped_benchmark() { return gx2::uncapped(); }

static void update_flips() {  // g_flip_mutex held
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        uint64_t at = flip_due(g_pending_flips.front());
        // relaxed: a rendered frame flips once a whole swap interval has passed since the previous
        // flip, on a vblank or not (a frame on time is rendered before then and flips at that moment,
        // as on its vblank; a late one flips as soon as it is rendered)
        bool late = false;
        if (relaxed_vsync()) {
            const uint64_t interval = interp::effective_swap_interval(g_swap_interval);
            const auto since = std::chrono::steady_clock::now() - g_last_flip_tp;
            if (since + std::chrono::microseconds(500) < kVsyncPeriod * interval) break;
            late = at > now;
            at = now;
        }
        if ((!uncapped_benchmark() && at > now) || render::frames_completed() < g_pending_flips.front().swap) break;
        g_late_flips += late;
        at = now / vsync_granule() * vsync_granule();
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_tp = std::chrono::steady_clock::now();
        g_last_flip_time = timebase::now();
        g_flip_count++;
    }
}
#ifdef WWHD_HAS_VULKAN
static void ready_flip_before_resume() {
    bool needsSync;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        if(g_pending_flips.empty()) return;
        const auto& front = g_pending_flips.front();
        const uint64_t at = flip_due(front);
        if(at > vsync_index()) return;
        needsSync = render::frames_completed() < front.swap;
    }
    if(needsSync) render_sync(kSyncFlip); // Core already released; no flip lock held.
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
}
#endif

HLE(gx2, GX2Init) {
    set_default_state();
    LOG("[gx2] initialized (native GX2 -> %s)", render::api_name(render::active()));
    if(uncapped_benchmark())
        LOG("[gx2 benchmark] uncapped guest flips; GPU completion ordering retained; frame-based simulation accelerates while timebase/audio clocks remain real-time");
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    t_rec.start = t_rec.pos = arg(c, 0);
    t_rec.end = arg(c, 0) + arg(c, 1);
    t_rec.last = 0;
    t_rec.recorded = t_rec.merged = 0;
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = t_rec.pos - t_rec.start;
    if (t_rec.recorded) g_recorded.fetch_add(t_rec.recorded, std::memory_order_relaxed);
    if (t_rec.merged) g_merged.fetch_add(t_rec.merged, std::memory_order_relaxed);
    t_rec = Recording{};
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    if (arg(c, 0)) st32(arg(c, 0), t_rec.start);
    if (arg(c, 1)) st32(arg(c, 1), t_rec.start ? t_rec.end - t_rec.start : 0);
    ret(c, t_rec.start != 0);
}
HLE(gx2, GX2CallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }
HLE(gx2, GX2DirectCallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(std::vector<uint32>& p, uint32 addr, uint32 words) {
    size_t at = p.size();
    p.resize(at + words);
    if (addr) memcpy(&p[at], mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p.push_back(fbits(farg(c, i)));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), arg(c, 1) & 0xFF, arg(c, 2)});
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    put_struct(p, arg(c, 1), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)), fbits(farg(c, 4)),
                       arg(c, 2) & 0xFF, arg(c, 3)});
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
HLE(gx2, GX2CopySurface) {
    // debug: WWHD_COPYDBG=1 logs each copy as issued (thread, caller, source and destination images)
    static const bool dbg = getenv("WWHD_COPYDBG") != nullptr;
    if (dbg)
        LOG("[copydbg] issue t=%.3f thread %08X lr %08X src %08X img %08X dst %08X img %08X size %X", timebase::now() / (double)timebase::kTicksPerSec,
            threads::current_thread(), c->lr, arg(c, 0), ld32(arg(c, 0) + 0x24), arg(c, 3), ld32(arg(c, 3) + 0x24), ld32(arg(c, 3) + 0x20));
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kSurfaceWords);
    p.insert(p.end(), {arg(c, 1), arg(c, 2)});
    put_struct(p, arg(c, 3), kSurfaceWords);
    p.insert(p.end(), {arg(c, 4), arg(c, 5)});
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // The copy is complete when GX2CopySurface returns: the game uses the result (and frees the
    // surfaces) right away. agl's tile-mode conversion (027B5EEC) copies into a temporary surface,
    // OSBlockMoves it back and frees it at once; executed later on the render thread, the copy
    // wrote into the freed memory after the heap had reused it (boot crash: agl shader program
    // array 21EFE28C, program 0's +0x7c zeroed). Not for display lists (they run when called).
    if (!t_rec.start) {
        BlockingScope b;
        render_sync(kSyncCopySurface);
    }
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    p.push_back(arg(c, 1));
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) { emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)}); }

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { emit_host(OP_FLUSH, {}); }
HLE(gx2, GX2DrawDone) {
    BlockingScope b;
    emit_host(OP_DRAW_DONE, {});
    render_sync(kSyncDrawDone);
    ret(c, 1);
}
HLE(gx2, GX2SwapScanBuffers) {
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    uint32 ab;
    memcpy(&ab, &a, 4);
    // debug: WWHD_DEBUG_FRAME_MS=n adds n ms of work to every frame on the game thread (frame pacing tests
    // on a fast machine, e.g. WWHD_RELAXED_VSYNC with frames just over two vblanks)
    if (static const int d = getenv("WWHD_DEBUG_FRAME_MS") ? atoi(getenv("WWHD_DEBUG_FRAME_MS")) : 0; d) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(d);
        while (std::chrono::steady_clock::now() < until) {}
    }
    // the frame drew no new logic step (an interpolation hold pass): for the render-thread profiler
    static uint64_t lastSteps = ~0ull;
    const uint64_t steps = interp::logic_steps();
    const bool hold = steps == lastSteps;
    lastSteps = steps;
    emit_host(OP_SWAP, {ab, hold ? 1u : 0u});
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        g_swap_count++;
        g_pending_flips.push_back({vsync_index(), g_swap_count});
    }
    g_watch_swaps.store(g_swap_count, std::memory_order_relaxed);
    static std::once_flag watchdog;
    std::call_once(watchdog, [] {
        if (!getenv("WWHD_NO_WATCHDOG")) host::start_thread(watchdog_thread, 256 << 10);
    });
    // debug: WWHD_LOG_SLOW_SWAP=ms logs swaps that came more than ms after the previous one
    static const double slow_ms = getenv("WWHD_LOG_SLOW_SWAP") ? atof(getenv("WWHD_LOG_SLOW_SWAP")) : 0;
    if (slow_ms > 0) {
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        if (ms > slow_ms) LOG("[gx2] slow swap %llu: %.1f ms", (unsigned long long)g_swap_count, ms);
    }
    if (g_swap_count % 300 == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        static uint64_t lastLate = 0;
        static uint64_t lastCommands = 0;
        const uint64_t commands = g_progress.commands.load(std::memory_order_relaxed);
        uint64_t publishes, wakes, staged, merged, recorded;
        staging_stats(publishes, wakes, staged, merged, recorded);
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u, late frames flipped at once %llu (WWHD_RELAXED_VSYNC %s); "
            "%.0f GX2 commands per frame executed; per frame: %.0f GX2 calls recorded into display lists, %.0f staged "
            "by the main thread, %.0f register writes merged into the command before (WWHD_GX2_COALESCE), %.0f "
            "hand-overs to the render thread (%.0f woke it)",
            (unsigned long long)g_swap_count, 300 / s, g_swap_interval, (unsigned long long)(g_late_flips - lastLate),
            relaxed_vsync() ? "on" : "off", double(commands - lastCommands) / 300.0, double(recorded) / 300.0,
            double(staged) / 300.0, double(merged) / 300.0, double(publishes) / 300.0, double(wakes) / 300.0);
        lastCommands = commands;
        lastLate = g_late_flips;
        // WWHD_SCHED_STATS=1: report here, every 300 frames; =2: the scheduler thread reports every 5 s
        // (not both: two reporters split each other's measuring spans)
        static const char* sched = getenv("WWHD_SCHED_STATS");
        if (sched && atoi(sched) != 2) threads::report_sched();
    }
}
HLE(gx2, GX2GetSwapStatus) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(g_flip_count + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
HLE(gx2, GX2WaitForVsync) {
    if(uncapped_benchmark()) {
        BlockingScope b;
        // The queue fence follows earlier swaps, whose presentation path waits
        // for GPU completion. Never wait while holding the flip mutex.
        render_sync(kSyncVsyncUncapped);
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        return;
    }
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipWait = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_WAIT");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    if(readyFlipWait) {
        bool eligible = false, needsSync = false;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            if(!g_pending_flips.empty()) {
                const auto& front = g_pending_flips.front();
                const uint64_t at = flip_due(front);
                eligible = at <= vsync_index();
                if(eligible) needsSync = render::frames_completed() < front.swap;
            }
        }
        if(eligible) {
            if(needsSync) {
                BlockingScope b;
                render_sync(kSyncVsyncFlip); // Queued swap completion; never hold flip mutex.
            }
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips(); // Retains minimum interval and FIFO GPU guards.
            return;
        }
    }
#endif
    static const bool preciseSleep = [] {
#ifdef WWHD_HAS_VULKAN
        // Vulkan renderer's pacing (docs/vulkan.md); the Metal renderer keeps plain sleeping
        if (!render::vulkan()) return false;
        const char* value = getenv("WWHD_VSYNC_PRECISE");
#ifdef __APPLE__
        // Avoid the measured macOS sleep overshoot; explicit zero opts out.
        return !value || atoi(value) != 0;
#else
        return value && atoi(value) != 0;
#endif
#else
        return false;
#endif
    }();
    const uint64_t granule = vsync_granule();
    const auto deadline = tick_time((vsync_index() / granule + 1) * granule);  // the next (virtual) vsync
    if (relaxed_vsync()) {
        // a late frame's flip happens when the renderer finishes it: return then instead of at the
        // next vblank (the game's wait loop checks the flip count after each call)
        // a flip since the previous call returned: the game's flip wait is over (a late frame often
        // flips before the game asks); without one, a real vblank wait as before
        static uint64_t flipsSeen = 0;
        bool pending;
        uint64_t flips;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            pending = !g_pending_flips.empty();
            flips = g_flip_count;
            if (flips != flipsSeen) {
                flipsSeen = flips;
                return;
            }
        }
        if (pending) {
            for (;;) {
                const std::chrono::steady_clock::time_point limit = deadline,
                                                            soon = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
                const auto step = std::min(limit, soon);
                threads::park_sleep_until(step, false);
                std::lock_guard<std::mutex> lk(g_flip_mutex);
                update_flips();
                if (g_flip_count != flips || g_pending_flips.empty() || std::chrono::steady_clock::now() >= deadline) {
                    flipsSeen = g_flip_count;
                    return;
                }
            }
        }
    }
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipPark = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_PARK");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    threads::park_sleep_until(deadline, preciseSleep,
        readyFlipPark ? ready_flip_before_resume : nullptr);
#else
    threads::park_sleep_until(deadline, preciseSleep);
#endif
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    static uint64_t calls = 0;
    if (getenv("WWHD_LOG_VSYNC") && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    // mode: GX2TVRenderMode (1/2 = 480p, 3 = 720p, 5 = 1080p)
    LOG("[gx2] TV buffer format %X, mode %u", arg(c, 3), arg(c, 2));
    render::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { render::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states
#include "../savestate.h"

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {1});  // full GPU wait, also with lazy DrawDone
    render_sync(kSyncSaveState);
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    w.u32(kNumRegs);
    w.bytes(g_regs, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : g_contexts) {
        keys.push_back(k);
        if (g_shadow == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)g_contexts[k].size());
        w.bytes(g_contexts[k].data(), g_contexts[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    r.bytes(g_regs, sizeof g_regs);
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    memset(g_written_blocks, 0xFF, sizeof g_written_blocks);  // (the loaded files: every block may differ)
    bump_all_categories();
    g_shader_state_gen++;
    g_shader_regs_gen++;
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    render::ss_reset();  // the renderer forgets surface contents and shader memos (Vulkan)
}

// (this fork, issue #67) Lockstep frame hashes: the run's state between the debug-server thread, the game
// thread and the render thread (lockstep.h).
#include "lockstep.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#include "guest_addr.h"
#include "platform/host.h"
#include "runtime.h"

namespace lockstep {
namespace {
std::atomic<bool> g_armed{false};
std::atomic<bool> g_seed{false};     // the next portable load is seeded
std::atomic<bool> g_arrived{false};  // its first gameplay frame was seen
std::atomic<bool> g_done{false};
std::atomic<uint64_t> g_arrivalStep{0};
std::atomic<uint32_t> g_n{0}, g_skip{0}, g_run{0};
std::mutex g_mu;
std::vector<Record> g_records;  // (g_mu)
// render thread: the tag of the next present
bool g_tagged = false;
Record g_tag;

// the memory diagnostic (memsnap): the game's static data word by word, everything else in 4 KiB block hashes
std::atomic<bool> g_memsnap{false};  // take one at this run's first hashed frame
constexpr uint32_t kStaticStart = 0x10000000, kStaticEnd = 0x10600000, kBlock = 0x1000;
struct Snap {
    bool valid = false;
    std::vector<uint32_t> words;                          // kStaticStart .. kStaticEnd
    std::vector<std::pair<uint32_t, uint64_t>> blocks;    // (address, hash) of touched, non-zero blocks
};
Snap g_snaps[2];  // previous, current (game thread writes; read after the run under g_mu)
std::string g_memdiff;
// the blocks that differed at the last snapshot, with their content then: compared word by word at the next
std::vector<std::pair<uint32_t, std::vector<uint32_t>>> g_watched;

void take_memsnap() {
    Snap& prev = g_snaps[0];
    Snap& cur = g_snaps[1];
    std::swap(prev, cur);
    cur.valid = true;
    cur.words.resize((kStaticEnd - kStaticStart) / 4);
    memcpy(cur.words.data(), mem::ptr(kStaticStart), kStaticEnd - kStaticStart);
    cur.blocks.clear();
    const std::pair<uint32_t, uint32_t> ranges[] = {{kStaticEnd, mem::kMem2End}, {mem::kFgBucket, mem::kFgBucket + mem::kFgBucketSize},
                                                    {mem::kMem1, mem::kMem1 + mem::kMem1Size}};
    for (const auto& r : ranges)
        for (uint32_t a = r.first; a < r.second; a += 0x10000) {
            uint8_t* p = mem::ptr(a);
            if (!host::memory_touched(p, 0x10000)) continue;
            for (uint32_t b = 0; b < 0x10000; b += kBlock) cur.blocks.push_back({a + b, hash_bytes(p + b, kBlock)});
        }
    std::string out;
    char line[160];
    if (!prev.valid) {
        out = "# memdiff: no earlier snapshot (the next run is compared with this one)\n";
    } else {
        int listed = 0, words = 0;
        for (size_t i = 0; i < cur.words.size(); i++) {
            if (cur.words[i] == prev.words[i]) continue;
            words++;
            if (listed++ < 600) {
                snprintf(line, sizeof line, "w %08X %08X %08X\n", uint32_t(kStaticStart + i * 4), __builtin_bswap32(prev.words[i]),
                         __builtin_bswap32(cur.words[i]));
                out += line;
            }
        }
        // blocks: both lists are in address order
        size_t i = 0, j = 0;
        int ranges_n = 0, diff_blocks = 0;
        uint32_t run_start = 0, run_end = 0;
        auto flush = [&] {
            if (run_end > run_start && ranges_n++ < 400) {
                snprintf(line, sizeof line, "b %08X %08X\n", run_start, run_end);
                out += line;
            }
            run_start = run_end = 0;
        };
        auto differ = [&](uint32_t a) {
            diff_blocks++;
            if (a != run_end) flush(), run_start = a;
            run_end = a + kBlock;
        };
        while (i < prev.blocks.size() || j < cur.blocks.size()) {
            if (j >= cur.blocks.size() || (i < prev.blocks.size() && prev.blocks[i].first < cur.blocks[j].first)) differ(prev.blocks[i++].first);
            else if (i >= prev.blocks.size() || cur.blocks[j].first < prev.blocks[i].first) differ(cur.blocks[j++].first);
            else {
                if (prev.blocks[i].second != cur.blocks[j].second) differ(cur.blocks[j].first);
                i++, j++;
            }
        }
        flush();
        snprintf(line, sizeof line, "# memdiff: %d static words, %d blocks of 4 KiB in %d ranges differ from the last run\n", words,
                 diff_blocks, ranges_n);
        out = line + out;
        // the blocks that differed last time, word by word: small numbers (an animation's frame, a counter) only
        auto small = [](uint32_t v) {
            float f;
            memcpy(&f, &v, 4);
            return v < 0x100 || (f >= 0.f && f < 256.f && (v >> 23) >= 0x70);
        };
        int wlisted = 0;
        for (const auto& wb : g_watched) {
            const uint32_t* now = reinterpret_cast<const uint32_t*>(mem::ptr(wb.first));
            for (uint32_t k = 0; k < kBlock / 4; k++) {
                const uint32_t o = __builtin_bswap32(wb.second[k]), c = __builtin_bswap32(now[k]);
                if (o == c || !small(o) || !small(c)) continue;
                if (wlisted++ < 80000) {
                    snprintf(line, sizeof line, "h %08X %08X %08X\n", wb.first + k * 4, o, c);
                    out += line;
                }
            }
        }
        // the blocks that differ now: watched at the next snapshot
        g_watched.clear();
        size_t pi = 0, cj = 0;
        while (cj < cur.blocks.size() && g_watched.size() < 4096) {
            while (pi < prev.blocks.size() && prev.blocks[pi].first < cur.blocks[cj].first) pi++;
            const bool same = pi < prev.blocks.size() && prev.blocks[pi].first == cur.blocks[cj].first &&
                              prev.blocks[pi].second == cur.blocks[cj].second;
            if (!same && cur.blocks[cj].first >= mem::kMem2Start && cur.blocks[cj].first < mem::kMem2End) {
                const uint32_t* p = reinterpret_cast<const uint32_t*>(mem::ptr(cur.blocks[cj].first));
                g_watched.emplace_back(cur.blocks[cj].first, std::vector<uint32_t>(p, p + kBlock / 4));
            }
            cj++;
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_memdiff = out;
}
}  // namespace

bool arm(uint32_t n, uint32_t skip) {
    if (g_armed.load()) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_records.clear();
        g_records.reserve(n);
    }
    g_n = n, g_skip = skip;
    g_run++;
    g_arrived = false, g_done = false;
    g_seed = true;
    g_armed.store(true, std::memory_order_release);
    LOG("[lockstep] armed: the next portable load is seeded; logic steps %u to %u after its arrival are hashed", skip + 1,
        skip + n);
    return true;
}

void disarm() {
    if (!g_armed.exchange(false)) return;
    g_seed = false;
    std::lock_guard<std::mutex> lk(g_mu);
    LOG("[lockstep] run ended: %zu of %u frames hashed", g_records.size(), g_n.load());
}

bool armed() { return g_armed.load(std::memory_order_relaxed); }
bool seed_wanted() { return armed() && g_seed.load(std::memory_order_relaxed); }

namespace {
// The HUD's layouts (nw::lyt) stay loaded across stage loads, and their looping animations (a 50-frame loop of
// T_Information_00 lights the picture's lower middle) go on from wherever the boot and the earlier stages left them:
// two loads of the same state show them at other frames. At the seeded arrival every looping one is set back to its
// first frame. Found by structure, not address: an animation object holds its resource (a "pai1" block: frame count
// u16 at +8, loop flag u8 at +0xA) and its frame (f32) in the next word.
void reset_layout_loops() {
    const auto t0 = std::chrono::steady_clock::now();
    constexpr uint32_t kPai1 = 0x70616931;  // "pai1"
    struct Range { uint32_t a, b; };
    const Range ranges[] = {{mem::kMem2Start, mem::kMem2End}};
    auto for_words = [&](auto&& fn) {
        for (const Range& r : ranges)
            for (uint32_t a = r.a; a < r.b; a += 0x10000) {
                const uint32_t* p = reinterpret_cast<const uint32_t*>(mem::ptr(a));
                if (!host::memory_touched(const_cast<uint32_t*>(p), 0x10000)) continue;
                for (uint32_t k = 0; k < 0x10000 / 4; k++) fn(a + k * 4, __builtin_bswap32(p[k]));
            }
    };
    std::vector<uint32_t> blocks;  // looping pai1 blocks
    for_words([&](uint32_t a, uint32_t v) {
        if (v == kPai1 && (ld32(a + 8) & 0xFF00) == 0x0100 && (ld32(a + 8) >> 16) > 0) blocks.push_back(a);
    });
    std::sort(blocks.begin(), blocks.end());
    int reset = 0;
    if (!blocks.empty())
        for_words([&](uint32_t a, uint32_t v) {
            if (v < blocks.front() || v > blocks.back() || !std::binary_search(blocks.begin(), blocks.end(), v)) return;
            const uint32_t fa = a + 4, fbits = ld32(fa);
            float f;
            memcpy(&f, &fbits, 4);
            const float frames = float(ld32(v + 8) >> 16);
            if (!(f >= 0.f && f <= frames)) return;  // not an animation's frame
            if (fbits) st32(fa, 0);
            reset++;
        });
    LOG("[lockstep] layout animations: %zu looping ones, %d frames set to 0 (%.0f ms)", blocks.size(), reset,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}
}  // namespace

void seeded_arrival(uint64_t step) {
    if (!armed() || !g_seed.exchange(false)) return;
    reset_layout_loops();
    g_arrivalStep = step;
    g_arrived.store(true, std::memory_order_release);
    LOG("[lockstep] seeded arrival at logic step %llu", (unsigned long long)step);
}

bool arrived() { return g_arrived.load(std::memory_order_acquire); }
bool done() { return g_done.load(std::memory_order_acquire); }

bool game_swap(uint64_t steps, uint32_t tag[kTagWords]) {
    if (!armed() || !arrived() || done()) return false;
    const uint64_t offset = steps - g_arrivalStep.load(std::memory_order_relaxed);
    const uint32_t skip = g_skip.load(std::memory_order_relaxed), n = g_n.load(std::memory_order_relaxed);
    if (offset <= skip || offset > uint64_t(skip) + n) return false;
    const uint32_t rnd = GD(0x101FF9D4);  // cM_rnd's three seeds (fixed_seed.h)
    tag[0] = uint32_t(offset);
    tag[1] = ld32(GD(0x101FF560));  // g_Counter.mTimer
    tag[2] = ld32(rnd), tag[3] = ld32(rnd + 4), tag[4] = ld32(rnd + 8);
    tag[5] = g_run.load(std::memory_order_relaxed);
    if (offset == uint64_t(skip) + 1 && g_memsnap.exchange(false)) take_memsnap();
    return true;
}

void render_tag(const uint32_t* p, uint32_t n) {
    if (n < kTagWords) return;
    g_tag = Record{};
    g_tag.step = p[0];
    g_tag.timer = p[1];
    memcpy(g_tag.rnd, p + 2, sizeof g_tag.rnd);
    g_tag.run = p[5];
    g_tagged = true;
}

bool take_tag(Record& r) {
    if (!g_tagged) return false;
    g_tagged = false;
    r = g_tag;
    return true;
}

void record(const Record& r) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!armed() || r.run != g_run.load(std::memory_order_relaxed) || g_done.load(std::memory_order_relaxed)) return;
    g_records.push_back(r);
    if (r.step >= g_skip.load(std::memory_order_relaxed) + g_n.load(std::memory_order_relaxed))
        g_done.store(true, std::memory_order_release);
}

std::atomic<bool> g_dump{false};
void memsnap_next() { g_memsnap = true, g_dump = true; }
bool dump_wanted() { return g_dump.exchange(false); }
std::string memdiff() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_memdiff;
}

std::vector<Record> records() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_records;
}

uint64_t hash_bytes(const void* data, size_t size, uint64_t seed) {
    constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
    auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    auto round = [&](uint64_t acc, uint64_t v) { return rotl(acc + v * P2, 31) * P1; };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        a = round(a, v[0]), b = round(b, v[1]), c = round(c, v[2]), d = round(d, v[3]);
    }
    uint64_t h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18) + uint64_t(size);
    for (; i < size; i++) h = rotl(h ^ (uint64_t(p[i]) * P3), 11) * P1;
    h ^= h >> 33, h *= P2, h ^= h >> 29, h *= P3, h ^= h >> 32;
    return h;
}

}  // namespace lockstep

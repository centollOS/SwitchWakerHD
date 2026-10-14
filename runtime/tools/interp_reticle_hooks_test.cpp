// Exercise the actual hooks with guest-memory fixtures and a projection spy.
// The spy projects each supplied world point through a translating camera; the
// test checks hook scope, original call/CPU semantics, isolation and bypasses.
#include "runtime.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>
#include <vector>

extern "C" void hook_023D813C(Cpu*);
extern "C" void hook_020CBB70(Cpu*);
extern "C" void hook_025F1018(Cpu*);
namespace interp { void reticle_reset(); void reticle_keep_draw(uint32_t); }
namespace {
constexpr uint32_t base = 0x10000000, bytes = 0x20000;
constexpr uint32_t sight = base + 0x6000, boom = base + 0x8000;
constexpr uint32_t target = base + 0x10000, output = base + 0x18000;
bool active = true, at60 = false;
bool true60_enabled = false, use_projection = false, record_matrix = false;
constexpr uint32_t view = base + 0x1F000;
uint32_t drawn_projection[16]{};
uint64_t step = 1;
float fraction = 1;
std::array<float, 3> camera{};
int calls = 0;
}
namespace interp {
bool enabled() { return active; }
uint64_t logic_steps() { return step; }
uint64_t pass_count() { return step * 8; }
float pass_t() { return fraction; }
float reticle_t() { return fraction; }
bool reticle_projection(uint32_t& address, uint32_t* words) {
    if (!use_projection) return false;
    address = view;
    std::memcpy(words, drawn_projection, sizeof drawn_projection);
    return true;
}
}
namespace true60 {
bool runs_60(uint32_t) { return at60; }
bool enabled() { return true60_enabled; }
}
void log_msg(const char*, ...) {}
namespace true60_test { void reticle_targets(Cpu*, uint32_t) { assert(false); } }
extern "C" void f_025F1018_orig(Cpu* c) {
    ++calls;
    for (int i = 0; i < 3; ++i)
        stf32(c->r[4] + 4 * i, ldf32(c->r[3] + 4 * i) -
              (use_projection && i == 0 ? float(ldf32(view)) : camera[i]));
    // The hook must leave original callee effects in place, rather than
    // restoring the entry CPU to recover argument registers.
    c->r[3] = 0x11223344;
    c->f[1].ps0 = 19.25;
    c->cr[7] = 1;
}
extern "C" void f_023D813C_orig(Cpu* c) {
    uint32_t packet = c->r[3];
    c->r[3] += 8;
    c->r[4] = output;
    hook_025F1018(c);
    if (record_matrix)
        for (int i = 0; i < 12; ++i) st32(packet + 0x14 + 4 * i, 0x40000000u + i);
}
extern "C" void f_020CBB70_orig(Cpu* c) {
    uint32_t packet = c->r[3], slot = c->r[5];
    c->r[3] = c->r[4];
    c->r[4] = output;
    hook_025F1018(c);
    if (record_matrix) {
        for (int i = 0; i < 12; ++i) st32(packet + 4 + 48 * slot + 4 * i, 0x40000000u + slot * 12 + i);
        st32(packet + 0x100, ld32(packet + 0x100) | (1u << slot));
    }
}

static void draw(bool boomerang, int slot, uint32_t source,
                 std::array<float, 3> computed, std::array<float, 3> expected,
                 bool scoped = true) {
    for (int i = 0; i < 3; ++i) stf32(source + 4 * i, computed[i]);
    auto* memory = static_cast<unsigned char*>(ppc_ptr(base));
    std::vector<unsigned char> before(memory, memory + bytes);
    Cpu c{};
    c.r[3] = scoped ? (boomerang ? boom : sight) : source;
    c.r[4] = scoped ? source : output;
    c.r[5] = slot;
    int count = calls;
    if (!scoped) hook_025F1018(&c);
    else if (boomerang) hook_020CBB70(&c);
    else hook_023D813C(&c);
    assert(calls == count + 1);
    assert(c.r[3] == 0x11223344 && c.f[1].ps0 == 19.25 && c.cr[7] == 1);
    for (int i = 0; i < 3; ++i) {
        assert(ldf32(output + 4 * i) == expected[i]);
        // Exclude only the projection result; source positions and all other
        // guest bytes (standing in for the remaining HUD) must be unchanged.
        std::memcpy(before.data() + output - base + 4 * i,
                    ppc_ptr(output + 4 * i), 4);
    }
    if (record_matrix && scoped) {
        uint32_t address = boomerang ? boom + 4 + 48 * slot : sight + 0x14;
        std::memcpy(before.data() + address - base, ppc_ptr(address), 48);
        if (boomerang) std::memcpy(before.data() + boom + 0x100 - base, ppc_ptr(boom + 0x100), 4);
    }
    assert(std::memcmp(before.data(), memory, bytes) == 0);
}

int main() {
    setenv("WWHD_RETICLE_FIX", "1", 1);
    unsetenv("WWHD_RETICLE_TRACE");
    void* mapped = mmap(ppc_ptr(base), bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    assert(mapped == ppc_ptr(base));
    std::memset(mapped, 0xA5, bytes);
    for (uint32_t p : {sight + 8, target + 0x37C, target + 0x13FC})
        for (int i = 0; i < 3; ++i) stf32(p + 4 * i, 100.f + i);

    for (int frames : {2, 4, 8}) {
        interp::reticle_reset();
        step = 1; fraction = 1; camera = {};
        draw(false, 0, sight + 8, {640, 360, 0}, {640, 360, 0});
        ++step;
        for (int pass = 1; pass <= frames; ++pass) {
            fraction = float(pass) / frames;
            camera[0] = fraction * 8;
            draw(false, 0, sight + 8, {624, 360, 0},
                 {640 - fraction * 24, 360, 0});
            // The scoped marker must not affect a later HUD projection.
            draw(false, 0, target, {100, 200, 0}, {100 - camera[0], 200, 0}, false);
        }
    }
    interp::reticle_reset();
    step = 10; fraction = 1; camera = {};
    for (int slot = 0; slot < 5; ++slot)
        draw(true, slot, target + 0x37C, {float(slot * 100), 300, 0}, {float(slot * 100), 300, 0});
    ++step; fraction = .5f;
    for (int slot = 0; slot < 5; ++slot)
        draw(true, slot, target + 0x37C, {float(slot * 100 + 20), 300, 0}, {float(slot * 100 + 10), 300, 0});
    // A different actor in a reused lock slot must not blend from the old target.
    draw(true, 0, target + 0x13FC, {900, 100, 0}, {900, 100, 0});
    // Reallocation at the same address still changes the fpc process ID.
    st32(target + 4, ld32(target + 4) + 1);
    draw(true, 2, target + 0x37C, {900, 100, 0}, {900, 100, 0});
    at60 = true;
    draw(true, 1, target + 0x37C, {700, 100, 0}, {700, 100, 0});
    at60 = false; ++step;
    draw(true, 1, target + 0x37C, {800, 100, 0}, {800, 100, 0});
    // No interpolation: verify exact original floats, including signed zero.
    active = false;
    draw(false, 0, sight + 8, {-0.f, 311.375f, -0.125f}, {-0.f, 311.375f, -0.125f});
    assert(ld32(output) == 0x80000000u);
    active = true; interp::reticle_reset(); ++step;
    draw(false, 0, sight + 8, {500, 300, 0}, {500, 300, 0});

    // true60 can restore camera/actor memory after their draw. Project through
    // the captured drawn view, restore the stale view immediately, then retain
    // only packet render matrices after the simulated actor rollback.
    interp::reticle_reset(); ++step; fraction = 1; camera = {};
    true60_enabled = use_projection = record_matrix = true;
    stf32(view, 16);
    std::memcpy(drawn_projection, ppc_ptr(view), sizeof drawn_projection);
    stf32(view, 999);
    draw(false, 0, sight + 8, {100, 200, 0}, {84, 200, 0});
    assert(ldf32(view) == 999);
    std::memset(ppc_ptr(sight + 0x14), 0, 48);
    interp::reticle_keep_draw(sight - 0x58E8);
    for (int i = 0; i < 12; ++i) assert(ld32(sight + 0x14 + 4 * i) == 0x40000000u + i);
    st32(boom + 0x100, 0);
    for (int slot = 0; slot < 5; ++slot)
        draw(true, slot, target + 0x37C, {100, 200, 0}, {84, 200, 0});
    std::memset(ppc_ptr(boom + 4), 0, 5 * 48);
    st32(boom + 0x100, 0);
    constexpr uint32_t physics = boom - 0x3B0 + 0x314;
    st32(physics, 0x12345678);
    interp::reticle_keep_draw(boom - 0x3B0);
    assert(ld32(physics) == 0x12345678);
    assert(ld32(boom + 0x100) == 31);
    for (int slot = 0; slot < 5; ++slot)
        for (int i = 0; i < 12; ++i) assert(ld32(boom + 4 + 48 * slot + 4 * i) == 0x40000000u + slot * 12 + i);
    ++step;
    st32(boom + 0x100, 0);
    interp::reticle_keep_draw(boom - 0x3B0);
    assert(ld32(boom + 0x100) == 0); // previous-pass render state is never replayed

    // Differential 30 Hz check: compare each actual setter hook with its
    // original over the complete guest fixture and CPU, even with true60's
    // render-preservation machinery available. No marker state may be replayed.
    active = false;
    for (int slot = -1; slot < 5; ++slot) {
        interp::reticle_reset();
        std::vector<unsigned char> entry(static_cast<unsigned char*>(mapped),
                                         static_cast<unsigned char*>(mapped) + bytes);
        Cpu original{}, hooked{};
        original.r[3] = slot < 0 ? sight : boom;
        original.r[4] = target + 0x37C;
        original.r[5] = slot;
        hooked = original;
        if (slot < 0) f_023D813C_orig(&original);
        else f_020CBB70_orig(&original);
        std::vector<unsigned char> expected(static_cast<unsigned char*>(mapped),
                                            static_cast<unsigned char*>(mapped) + bytes);
        std::memcpy(mapped, entry.data(), bytes);
        if (slot < 0) hook_023D813C(&hooked);
        else hook_020CBB70(&hooked);
        assert(std::memcmp(&original, &hooked, sizeof(Cpu)) == 0);
        assert(std::memcmp(mapped, expected.data(), bytes) == 0);
        uint32_t matrix = slot < 0 ? sight + 0x14 : boom + 4 + 48 * slot;
        std::memset(ppc_ptr(matrix), 0, 48);
        interp::reticle_keep_draw(slot < 0 ? sight - 0x58E8 : boom - 0x3B0);
        for (int i = 0; i < 12; ++i) assert(ld32(matrix + 4 * i) == 0);
    }
    munmap(mapped, bytes);
    std::puts("interp_reticle_hooks: PASS");
}

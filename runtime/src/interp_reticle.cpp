#include "runtime.h"
#include "interp_reticle.h"
#include "true60.h"
#include <cstdlib>
#include <unordered_map>

// Issue #163: world-space aiming points follow the drawn camera's fraction.
// WWHD_RETICLE_TRACE=1 logs scoped projections; WWHD_RETICLE_FIX=0 disables the fix.

namespace interp {
float pass_t();
uint64_t logic_steps();
uint64_t pass_count();
bool enabled();
float reticle_t();
bool reticle_projection(uint32_t&, uint32_t*);
}
namespace true60_test { void reticle_targets(Cpu*, uint32_t link); }
extern "C" {
void f_023D813C_orig(Cpu*);
void f_020CBB70_orig(Cpu*);
void f_025F1018_orig(Cpu*);
}
namespace {
thread_local uint32_t packet;
thread_local int marker;
std::unordered_map<uint64_t, interp::reticle::History> histories;
struct DrawMatrix {
    uint32_t owner = 0, address = 0, flags_address = 0, flags = 0;
    uint32_t words[12]{};
    uint64_t pass = 0;
};
std::unordered_map<uint64_t, DrawMatrix> matrices;
bool candidate_on() {
    static const bool on = !getenv("WWHD_RETICLE_FIX") || atoi(getenv("WWHD_RETICLE_FIX")) != 0;
    return on;
}
bool tracing() {
    static const bool on = getenv("WWHD_RETICLE_TRACE") != nullptr;
    return on;
}
void scoped(Cpu* c, int slot, void (*original)(Cpu*)) {
    const auto old_packet = packet;
    const auto old_marker = marker;
    packet = c->r[3];
    marker = slot;
    original(c);
    if (candidate_on() && true60::enabled() && (slot == -1 || (slot >= 0 && slot < 5))) {
        DrawMatrix& m = matrices[uint64_t(packet) * 8 + slot + 1];
        m.owner = packet - (slot < 0 ? 0x58E8 : 0x3B0);
        m.address = packet + (slot < 0 ? 0x14 : 4 + 48 * slot);
        memcpy(m.words, ppc_ptr(m.address), sizeof m.words);
        m.flags_address = slot < 0 ? 0 : packet + 0x100;
        m.flags = m.flags_address ? ld32(m.flags_address) : 0;
        m.pass = interp::pass_count();
        if (m.flags_address) {
            for (auto& [key, other] : matrices)
                if (other.flags_address == m.flags_address && other.pass == m.pass) other.flags = m.flags;
        }
        if (matrices.size() > 128) {
            for (auto it = matrices.begin(); it != matrices.end();) {
                if (it->second.pass + 16 < m.pass) it = matrices.erase(it);
                else ++it;
            }
        }
    }
    packet = old_packet;
    marker = old_marker;
}
}
namespace interp {
void reticle_reset() { histories.clear(); matrices.clear(); }
// Keep only render output after true60 restores an actor's draw-side gameplay
// state. These packets are painted later, outside that actor's draw callback.
void reticle_keep_draw(uint32_t owner) {
    if (!candidate_on()) return;
    for (const auto& [key, m] : matrices) {
        if (m.owner != owner || m.pass != pass_count()) continue;
        memcpy(ppc_ptr(m.address), m.words, sizeof m.words);
        if (m.flags_address) st32(m.flags_address, m.flags);
    }
}
}
extern "C" void hook_023D813C(Cpu* c) {
    static const bool fixture = getenv("WWHD_RETICLE_TARGET_FIXTURE") != nullptr;
    if (fixture) true60_test::reticle_targets(c, c->r[3] - 0x58E8);
    if (!interp::enabled() && !tracing() && !fixture) return f_023D813C_orig(c);
    scoped(c, -1, f_023D813C_orig);
}
extern "C" void hook_020CBB70(Cpu* c) {
    if (!interp::enabled() && !tracing()) return f_020CBB70_orig(c);
    scoped(c, int(c->r[5]), f_020CBB70_orig);
}
extern "C" void hook_025F1018(Cpu* c) {
    if (!packet) {
        f_025F1018_orig(c);
        return;
    }
    const uint32_t src = c->r[3], dst = c->r[4];
    const float x = ldf32(src), y = ldf32(src + 4), z = ldf32(src + 8);
    const uint32_t exact[3] = {ld32(src), ld32(src + 4), ld32(src + 8)};
    const bool fix = candidate_on();
    interp::reticle::Point drawn{x, y, z};
    if (fix && packet && src != dst && interp::enabled()) {
        const uint64_t key = uint64_t(packet) * 8 + marker + 1;
        const uint32_t actor = marker < 0 ? packet - 0x58E8 : src - 0x37C;
        const bool at60 = true60::runs_60(actor);
        if (!at60) {
            const uint64_t identity = uint64_t(ld32(actor + 4)) << 32 | src; // fpc process ID + point address
            drawn = histories[key].sample({x, y, z}, interp::logic_steps(), identity, interp::reticle_t());
            for (int i = 0; i < 3; ++i) stf32(src + 4 * i, drawn[i]);
        } else {
            histories.erase(key);
        }
        if (histories.size() > 128) {
            for (auto it = histories.begin(); it != histories.end();)
                if (it->second.step + 2 < interp::logic_steps()) it = histories.erase(it);
                else ++it;
        }
    }
    uint32_t view = 0, saved_view[16], drawn_view[16];
    if (fix && interp::enabled() && true60::enabled() && interp::reticle_projection(view, drawn_view)) {
        memcpy(saved_view, ppc_ptr(view), sizeof saved_view);
        memcpy(ppc_ptr(view), drawn_view, sizeof drawn_view);
    }
    f_025F1018_orig(c);
    if (view) memcpy(ppc_ptr(view), saved_view, sizeof saved_view);
    if (fix && src != dst && interp::enabled())
        for (int i = 0; i < 3; ++i) st32(src + 4 * i, exact[i]);
    if (packet && tracing())
        LOG("[reticle] step %llu pass %llu t %.3f packet %08X slot %d world %.6f %.6f %.6f screen %.6f %.6f %.6f draw_world %.6f %.6f %.6f",
            (unsigned long long)interp::logic_steps(), (unsigned long long)interp::pass_count(),
            interp::pass_t(), packet, marker, x, y, z, ldf32(dst), ldf32(dst + 4), ldf32(dst + 8), drawn[0], drawn[1], drawn[2]);
}

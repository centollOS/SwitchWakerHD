// Grass-speck bisection switches (dk_bisect.h). Render thread only (code_load runs there too).
#include "dk_bisect.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "dk.h"
#include "runtime.h"

namespace gfxdk {

namespace {
int env_int(const char* name, int fallback) {
    const char* e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

Bisect read_switches() {
    Bisect b;
    b.shaderSched = env_int("WWHD_DK_SHADER_SCHED", b.shaderSched);
    if (b.shaderSched < 0 || b.shaderSched > 3) b.shaderSched = 0;
    b.earlyZ = env_int("WWHD_DK_EARLY_Z", b.earlyZ);
    if (const char* e = getenv("WWHD_DK_PS_KNOB"); e && *e) b.psKnobMesa = !strcmp(e, "mesa") || !strcmp(e, "1");
    if (const char* e = getenv("WWHD_DK_DEPTH_BIAS"); e && *e) {
        if (!strcmp(e, "latte")) b.depthBias = DepthBiasMode::Latte;
        else if (!strcmp(e, "off") || !strcmp(e, "0")) b.depthBias = DepthBiasMode::Off;
        else if (!strcmp(e, "units2")) b.depthBias = DepthBiasMode::Units2;
        else if (!strcmp(e, "noslope")) b.depthBias = DepthBiasMode::NoSlope;
        else b.depthBias = DepthBiasMode::Gl;
    }
    b.decalBarrier = env_int("WWHD_DK_DECAL_BARRIER", b.decalBarrier);
    b.shaderInvalidate = env_int("WWHD_DK_SHADER_INVALIDATE", b.shaderInvalidate) != 0;
    b.descWfi = env_int("WWHD_DK_DESC_WFI", b.descWfi) != 0;
    b.tiledOff = env_int("WWHD_DK_TILED_OFF", b.tiledOff) != 0;
    b.shadowBarrier = env_int("WWHD_DK_SHADOW_BARRIER", b.shadowBarrier) != 0;
    return b;
}

// uam's DKSH (third_party/uam/source/dksh.h): one program per module, the code section after the control one
struct DkshHeader {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
};
struct DkshProgram {
    uint32_t type, entrypoint, num_gprs, constbuf1_off, constbuf1_sz, per_warp_scratch_sz;
    // fragment programs' part of the union
    uint8_t has_table_3d1, early_fragment_tests, post_depth_coverage, per_sample_invocation;
    uint32_t table_3d1[4];
    uint32_t param_d8;
    uint16_t param_65b, param_489;
};
static_assert(offsetof(DkshProgram, param_d8) == 44, "DKSH program header layout");
constexpr uint32_t kDkshMagic = 0x48534B44;
constexpr uint32_t kSphSize = 0x50;  // the shader program header in front of a graphics program's code

bool g_codeLoaded = false;  // shader code was copied to code memory since the last invalidate
bool g_lastBiased = false;
struct Patched {
    uint64_t modules = 0, fragment = 0, words = 0, dual = 0, earlyZ = 0, lateZ = 0, knob = 0;
} g_patched;
}  // namespace

const Bisect& bisect() {
    static const Bisect b = read_switches();
    return b;
}

void bisect_patch_dksh(uint8_t* p, uint32_t size) {
    g_codeLoaded = true;
    const Bisect& b = bisect();
    if (size < sizeof(DkshHeader)) return;
    DkshHeader h;
    memcpy(&h, p, sizeof h);
    if (h.magic != kDkshMagic || h.num_programs != 1 || h.programs_off + sizeof(DkshProgram) > h.control_sz ||
        uint64_t(h.control_sz) + h.code_sz > size)
        return;
    DkshProgram prog;
    memcpy(&prog, p + h.programs_off, sizeof prog);
    if (prog.type == 5) return;  // compute: no SPH, never used by the renderer
    uint8_t* code = p + h.control_sz;
    const uint32_t start = prog.entrypoint + kSphSize;
    const uint32_t end = prog.constbuf1_sz ? prog.constbuf1_off : h.code_sz;
    if (start % 32 || end > h.code_sz || start >= end) return;
    g_patched.modules++;
    const bool fragment = prog.type == 1;

    // ---- scheduling words: one control qword per group of three instructions
    if (b.shaderSched > 0) {
        for (uint32_t at = start; at + 32 <= end; at += 32) {
            uint64_t ctl;
            memcpy(&ctl, code + at, 8);
            uint64_t out = ctl;
            for (int i = 0; i < 3; i++) {
                uint64_t f = (ctl >> (21 * i)) & 0x1FFFFF;
                if ((f & 15) == 0) {
                    f |= 1;  // stall 0 = dual issue with the next instruction: stall 1 instead
                    g_patched.dual++;
                }
                if (b.shaderSched >= 2) f |= uint64_t(0x3F) << 11;  // wait on every scoreboard
                if (b.shaderSched >= 3) f |= 15;
                out = (out & ~(uint64_t(0x1FFFFF) << (21 * i))) | (f << (21 * i));
            }
            if (out != ctl) {
                memcpy(code + at, &out, 8);
                g_patched.words++;
            }
        }
    }
    if (!fragment) return;
    g_patched.fragment++;

    // ---- early / late Z and the subtiling knob (the program header deko3d reads at bind time)
    uint32_t sph[20];
    memcpy(sph, code + prog.entrypoint, sizeof sph);
    const bool kills = (sph[0] >> 15) & 1, writesDepth = (sph[19] >> 1) & 1, sampleMask = sph[19] & 1;
    if (b.earlyZ == 1 && !kills && !writesDepth && !sampleMask && !prog.early_fragment_tests) {
        p[h.programs_off + offsetof(DkshProgram, early_fragment_tests)] = 1;
        g_patched.earlyZ++;
    } else if (b.earlyZ == 2 && !kills) {
        sph[0] |= 1u << 15;  // KillsPixels: the hardware tests depth after the shader
        memcpy(code + prog.entrypoint, sph, 4);
        p[h.programs_off + offsetof(DkshProgram, early_fragment_tests)] = 0;
        g_patched.lateZ++;
    }
    if (b.psKnobMesa && !writesDepth) {
        const uint32_t knob = 0x20164010;  // Mesa nvc0_shader_state.c (method 0x0360)
        memcpy(p + h.programs_off + offsetof(DkshProgram, param_d8), &knob, 4);
        g_patched.knob++;
    }
}

void bisect_before_draw(bool depthBiased) {
    const Bisect& b = bisect();
    if (g_codeLoaded) {
        g_codeLoaded = false;
        if (b.shaderInvalidate) dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Shader);
    }
    if (b.decalBarrier && (depthBiased || g_lastBiased)) {
        // before each depth-biased draw, and before the first draw after a run of them
        dkCmdBufBarrier(R.cmd, b.decalBarrier >= 2 ? DkBarrier_Full : DkBarrier_Fragments, DkInvalidateFlags_Image);
    }
    g_lastBiased = depthBiased;
}

void bisect_log() {
    const Bisect& b = bisect();
    static const char* const kSched[] = {"as compiled by uam", "no dual issue",
                                         "no dual issue + every instruction waits on all scoreboards",
                                         "no dual issue + wait on all scoreboards + stall 15 (slow)"};
    static const char* const kZ[] = {"as compiled", "API-mandated early Z on shaders that neither kill nor write depth",
                                     "late Z everywhere (SPH KillsPixels)"};
    static const char* const kBias[] = {"gl (no clamp, as Mesa)", "latte (clamp from PA_SU_POLY_OFFSET_CLAMP)", "off",
                                        "units x2", "slope 0"};
    static const char* const kBarrier[] = {"none", "Fragments", "Full"};
    LOG("[dk] grass bisect: WWHD_DK_SHADER_SCHED=%d (%s); %llu modules patched (%llu fragment), %llu control words, "
        "%llu dual issues removed", b.shaderSched, kSched[b.shaderSched], (unsigned long long)g_patched.modules,
        (unsigned long long)g_patched.fragment, (unsigned long long)g_patched.words,
        (unsigned long long)g_patched.dual);
    LOG("[dk] grass bisect: WWHD_DK_EARLY_Z=%d (%s; %llu early, %llu late so far); WWHD_DK_PS_KNOB=%s",
        b.earlyZ, kZ[b.earlyZ >= 0 && b.earlyZ <= 2 ? b.earlyZ : 0], (unsigned long long)g_patched.earlyZ,
        (unsigned long long)g_patched.lateZ, b.psKnobMesa ? "mesa (0x20164010)" : "0 (uam 0x087F6080)");
    LOG("[dk] grass bisect: WWHD_DK_DEPTH_BIAS=%s; WWHD_DK_DECAL_BARRIER=%d (%s before depth-biased draws)",
        kBias[int(b.depthBias)], b.decalBarrier, kBarrier[b.decalBarrier >= 0 && b.decalBarrier <= 2 ? b.decalBarrier : 0]);
    LOG("[dk] grass bisect: WWHD_DK_SHADER_INVALIDATE=%d (shader caches after mid-frame code loads); "
        "WWHD_DK_DESC_WFI=%d (wait for idle before descriptor cache invalidates); WWHD_DK_TILED_OFF=%d (tiled cache "
        "disabled explicitly each frame)", b.shaderInvalidate, b.descWfi, b.tiledOff);
    LOG("[dk] grass bisect: WWHD_DK_SHADOW_BARRIER=%d (a draw sampling a target written since the last barrier: %s)",
        b.shadowBarrier, b.shadowBarrier ? "Full barrier + image/shader/descriptor/L2 invalidate"
                                         : "Fragments barrier + image invalidate (P4)");
}

}  // namespace gfxdk

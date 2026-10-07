// Image and sampler descriptor slots and the sampler cache (dk_surfaces.h). The descriptor sets live in
// memory.cpp's descriptor block (8192 images, 1024 samplers) and are bound once per frame by begin_commands.
// Slots below kReservedImageIds / kReservedSamplerIds belong to the renderer's own passes; the rest are
// handed out here. Every write is recorded with dkCmdBufPushData (ordered with the commands: the CPU never
// writes a descriptor the GPU may be reading); a freed slot is reused once the GPU has finished the frame
// that freed it. Render thread only.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "dk_surfaces.h"
#include "runtime.h"
#include "surf_internal.h"

namespace gfxdk {

namespace {
struct Retired {
    uint64_t frame;  // the frame being recorded when the slot was freed
    uint32_t id;
};
std::vector<uint32_t> g_freeImages;  // slots that no frame in flight uses
uint32_t g_nextImage = kReservedImageIds;
std::deque<Retired> g_retiredImages;
uint32_t g_imagesUsed = 0;
bool g_dirty = false;  // descriptors written since the last commit_descriptors
DescriptorStats g_stats;

// ---- samplers: GX2 sampler words (+ compare, integer) -> slot
struct SamplerKey {
    uint32_t w0, w1, w2, flags;
    bool operator==(const SamplerKey& o) const { return !memcmp(this, &o, sizeof o); }
};
struct SamplerKeyHash {
    size_t operator()(const SamplerKey& k) const {
        uint64_t a = (uint64_t(k.w0) << 32 | k.w1) * 0x9E3779B97F4A7C15ull, b = (uint64_t(k.w2) << 32 | k.flags);
        return size_t(a ^ (b * 0xFF51AFD7ED558CCDull) ^ (a >> 29));
    }
};
struct SamplerSlot {
    SamplerKey key{};
    uint64_t lastFrame = 0;  // the last frame recorded with it
    bool used = false;
};
std::unordered_map<SamplerKey, uint32_t, SamplerKeyHash> g_samplers;
std::vector<SamplerSlot> g_samplerSlots(kSamplerDescriptors);
uint32_t g_nextSampler = kReservedSamplerIds;

// 16x anisotropic filtering (WWHD_ANISO=1, or the settings overlay's Effects; off by default as in gfx/vulkan;
// gfx/gl has none): set from the overlay's thread, taken by the render thread at a frame's start
std::atomic<bool> g_anisoWanted{[] {
    const char* e = getenv("WWHD_ANISO");
    return e && atoi(e) != 0;
}()};
bool g_anisoFrame = false;  // this frame's (render thread)
bool g_anisoLogged = false;

// the frame being recorded (begin_commands opened it)
uint64_t recording() { return R.frame + 1; }
// no frame in flight still reads what frame `f` used: it ended and the GPU finished it
bool frame_retired(uint64_t f) { return f <= R.frame && frame_done(f); }

// GX2 sampler words -> DkSampler (gfx/gl/draw.cpp sampler(), with gfx/vulkan's 16x anisotropy option)
DkSampler make_sampler(const uint32_t* words, bool compare, bool integer, bool aniso) {
    Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0 w;
    Latte::LATTE_SQ_TEX_SAMPLER_WORD1_0 w1;
    memcpy(static_cast<void*>(&w), words, 4);
    memcpy(static_cast<void*>(&w1), words + 1, 4);
    auto linear = [](uint32_t v) { return !(v == 0 || v == 4); };  // POINT and ANISO_POINT are nearest
    auto wrap = [](uint32_t v) {
        switch (v) {
        case 0: return DkWrapMode_Repeat;
        case 1: return DkWrapMode_MirroredRepeat;
        case 2: return DkWrapMode_ClampToEdge;
        case 3: case 5: case 7: return DkWrapMode_MirrorClampToEdge;
        default: return DkWrapMode_ClampToBorder;
        }
    };
    DkSampler s;
    dkSamplerDefaults(&s);
    const bool mag = !integer && linear(uint32_t(w.get_XY_MAG_FILTER()));
    const bool min = !integer && linear(uint32_t(w.get_XY_MIN_FILTER()));
    const uint32_t mip = integer ? 1 : uint32_t(w.get_MIP_FILTER());
    s.magFilter = mag ? DkFilter_Linear : DkFilter_Nearest;
    s.minFilter = min ? DkFilter_Linear : DkFilter_Nearest;
    s.mipFilter = mip == 0 ? DkMipFilter_None : mip == 2 ? DkMipFilter_Linear : DkMipFilter_Nearest;
    s.wrapMode[0] = wrap(uint32_t(w.get_CLAMP_X()));
    s.wrapMode[1] = wrap(uint32_t(w.get_CLAMP_Y()));
    s.wrapMode[2] = wrap(uint32_t(w.get_CLAMP_Z()));
    // the LOD as gfx/gl gives it to Mesa: bias, min LOD, max LOD (0 without mipmapping); Mesa's state
    // tracker (st_convert_sampler) swaps a min LOD above the max LOD, where deko3d would raise the max to
    // the min: a sampler without mipmapping and a min LOD above 0 clamps to 0..min in GL, not to min
    s.lodBias = float(w1.get_LOD_BIAS()) / 64.f;
    const float minLod = w1.get_MIN_LOD() / 64.f, maxLod = uint32_t(w.get_MIP_FILTER()) ? w1.get_MAX_LOD() / 64.f : 0.f;
    s.lodClampMin = std::min(minLod, maxLod);
    s.lodClampMax = std::max(minLod, maxLod);
    // 16x anisotropy (gfx/vulkan's condition): mipmapped, linear minification, no depth compare; GL applies
    // none, and neither does this renderer when the option is off (Latte's MAX_ANISO_RATIO is ignored, as GL)
    if (aniso && !integer && mip != 0 && min && !compare && uint32_t(w.get_DEPTH_COMPARE_FUNCTION()) == 0)
        s.maxAnisotropy = 16.0f;
    const uint32_t border = uint32_t(w.get_BORDER_COLOR_TYPE());
    static const float colors[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    for (int i = 0; i < 4; i++) s.borderColor[i].value_f = colors[border < 3 ? border : 0][i];
    if (compare) {
        s.compareEnable = true;
        s.compareOp = DkCompareOp(DkCompareOp_Never + uint32_t(w.get_DEPTH_COMPARE_FUNCTION()));
    }
    return s;
}
}  // namespace

uint32_t image_descriptor_alloc() {
    uint32_t id;
    if (!g_freeImages.empty()) {
        id = g_freeImages.back();
        g_freeImages.pop_back();
    } else if (g_nextImage < kImageDescriptors) {
        id = g_nextImage++;
    } else {
        fatal("[dk] image descriptors: all %u slots are in use (%u reserved, %zu waiting for the GPU); frame %llu",
              kImageDescriptors, kReservedImageIds, g_retiredImages.size(), (unsigned long long)recording());
    }
    g_imagesUsed++;
    return id;
}

void image_descriptor_write(uint32_t id, const DkImageView& view) {
    if (id >= kImageDescriptors) fatal("[dk] image descriptor %u written: out of range (%u slots)", id, kImageDescriptors);
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &view, false, false);
    dkCmdBufPushData(R.cmd, image_descriptors() + DkGpuAddr(id) * sizeof(DkImageDescriptor), &d, sizeof d);
    g_dirty = true;
    g_stats.imageWrites++;
    R.perf.imageDescriptorWrites++;
}

void image_descriptor_free_later(uint32_t id) {
    if (id < kReservedImageIds || id >= kImageDescriptors) return;  // 0: none; the renderer's own slots
    g_retiredImages.push_back({recording(), id});
    g_imagesUsed--;
}

uint32_t sampler_id(const uint32_t* samplerWords, bool compare, bool integer) {
    const SamplerKey key{samplerWords[0], samplerWords[1], samplerWords[2],
                         uint32_t(compare) | uint32_t(integer) << 1 | uint32_t(g_anisoFrame) << 2};
    if (auto it = g_samplers.find(key); it != g_samplers.end()) {
        g_samplerSlots[it->second].lastFrame = recording();
        g_stats.samplerHits++;
        return it->second;
    }
    uint32_t id;
    if (g_nextSampler < kSamplerDescriptors) {
        id = g_nextSampler++;
    } else {
        // the least recently used slot that no frame in flight uses (a rewrite while the GPU may read it
        // would show the new sampler on that frame's draws)
        id = 0;
        uint64_t oldest = ~0ull;
        for (uint32_t i = kReservedSamplerIds; i < kSamplerDescriptors; i++)
            if (g_samplerSlots[i].lastFrame < oldest && frame_retired(g_samplerSlots[i].lastFrame)) {
                oldest = g_samplerSlots[i].lastFrame;
                id = i;
            }
        if (!id) {
            static int logged = 0;
            if (logged++ < 10)
                LOG("[dk] sampler descriptors: all %u slots were used by frames still in flight; rewriting slot %u "
                    "(frame %llu may sample with the wrong sampler)", kSamplerDescriptors - kReservedSamplerIds,
                    kReservedSamplerIds, (unsigned long long)recording());
            id = kReservedSamplerIds;
        }
        g_samplers.erase(g_samplerSlots[id].key);
        g_stats.samplerEvictions++;
        // the draw path's texture caches may still hold the evicted slot for these words: looked up again
        R.surfaceEpoch++;
    }
    SamplerSlot& slot = g_samplerSlots[id];
    slot.key = key;
    slot.lastFrame = recording();
    slot.used = true;
    g_samplers.emplace(key, id);
    const DkSampler s = make_sampler(samplerWords, compare, integer, g_anisoFrame);
    DkSamplerDescriptor d;
    dkSamplerDescriptorInitialize(&d, &s);
    dkCmdBufPushData(R.cmd, sampler_descriptors() + DkGpuAddr(id) * sizeof(DkSamplerDescriptor), &d, sizeof d);
    g_dirty = true;
    g_stats.samplerWrites++;
    R.perf.samplerDescriptorWrites++;
    return id;
}

void sampler_used(uint32_t id) {
    if (id >= kReservedSamplerIds && id < kSamplerDescriptors) g_samplerSlots[id].lastFrame = recording();
}

void commit_descriptors() {
    if (!g_dirty) return;
    dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
    g_dirty = false;
}

bool aniso_enabled() { return g_anisoWanted.load(std::memory_order_relaxed); }
void set_aniso(bool on) {
    if (g_anisoWanted.exchange(on) != on) LOG("[dk] 16x anisotropic filtering %s (from the next frame)", on ? "on" : "off");
}

void descriptors_frame_start() {
    // the anisotropy option: samplers are looked up again under the new setting (their cache keys include it)
    const bool aniso = g_anisoWanted.load(std::memory_order_relaxed);
    if (!g_anisoLogged || aniso != g_anisoFrame) {
        LOG("[dk] anisotropic filtering: %s (WWHD_ANISO=1 or the overlay's Effects); sampler LOD as gfx/gl",
            aniso ? "16x on mipmapped linear samplers" : "off (as gfx/gl)");
        g_anisoLogged = true;
        g_anisoFrame = aniso;
        R.surfaceEpoch++;  // the draw path's texture caches hold sampler slots of the old setting
    }
    while (!g_retiredImages.empty() && frame_retired(g_retiredImages.front().frame)) {
        g_freeImages.push_back(g_retiredImages.front().id);
        g_retiredImages.pop_front();
    }
    // begin_commands' barrier made last frame's writes visible
    g_dirty = false;
}

void descriptor_usage(uint32_t& images, uint32_t& samplers) {
    images = g_imagesUsed;
    samplers = uint32_t(g_samplers.size());
}

DescriptorStats descriptor_stats_take() {
    DescriptorStats s = g_stats;
    s.imagesUsed = g_imagesUsed;
    s.samplersUsed = uint32_t(g_samplers.size());
    g_stats = {};
    return s;
}

}  // namespace gfxdk

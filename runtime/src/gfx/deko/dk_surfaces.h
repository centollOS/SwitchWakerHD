// The deko3d renderer's surfaces (P2, surface lane; docs/deko3d-plan.md "P2 lanes"): guest surfaces as
// DkImages in the image heap, their views and descriptors, uploads from guest memory, render targets,
// clears, copies, the TV scan buffer, invalidation and the sampler cache. A structural port of
// gfx/gl/surfaces.cpp (detiling, sparse hash, GuestRanges, scan copies, internal resolution), with
// images, views and uploads modelled on gfx/vulkan/surfaces.cpp and formats on gfx/vulkan/formats.cpp.
//
// Files: surfaces.cpp (this header's functions), formats.cpp (format_info, convert_row) and
// descriptors.cpp (descriptor slots, the sampler cache). Render thread only, like everything that records.
//
// Contracts with the other lanes:
// - Everything here that records commands assumes begin_commands() (dk.h) opened the frame.
// - Commands that bind render targets or set viewports, scissors or 3D state (clears) call forget_state()
//   (dk_draw.h) afterwards; the 2D-engine copies (dkCmdBufCopyImage / BlitImage / CopyBufferToImage) do not.
// - upload_surface and the copies leave their results visible to the commands that follow them (their own
//   barriers); the draw path adds none for them.
// - Descriptor writes are recorded with dkCmdBufPushData (ordered with the commands, never written by the CPU
//   into a descriptor the GPU may be reading); commit_descriptors() before the draw that uses them makes
//   them visible (a DkInvalidateFlags_Descriptors barrier when anything was written since the last one).
#pragma once
#include <deko3d.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dk.h"

namespace gfxdk {

// ---- formats (formats.cpp): GX2 surface formats -> DkImageFormat, CPU conversions as gfx/vulkan's
enum class Convert : uint8_t {
    NONE,       // copy texels as stored
    RGB565,     // Latte 5_6_5 -> RGBA8
    RGBA5551,   // Latte 1_5_5_5 (R in low bits) -> RGBA8
    ABGR1555,   // Latte 5_5_5_1 -> RGBA8
    RGBA4,      // Latte 4_4_4_4 -> RGBA8
    RG4,        // Latte 4_4 -> RG8
    D24S8,      // 24-bit depth + 8-bit stencil -> ZF32_X24S8 (kept at first, plan section 3)
    D24_R32F,   // 24-bit depth sampled as a color texture -> R32F
    X24_8_32F,  // 32F depth + 8 stencil in 64 bits -> ZF32_X24S8
};
struct FormatInfo {
    DkImageFormat image = DkImageFormat_None;  // the DkImage's format (render targets, uploads)
    uint32_t bytesPerBlock = 0;                // guest bytes per texel or 4x4 block
    uint32_t hostBytesPerBlock = 0;            // bytes per texel or block in the DkImage
    bool compressed = false, depth = false, stencil = false;
    bool srgb = false;                         // an sRGB format (the TV picture's encoding, present)
    Convert convert = Convert::NONE;
    enum Kind : uint8_t { FLOAT, UINT, SINT } kind = FLOAT;  // shader-visible data type
};
// isDepth: the surface is used as a depth buffer (selects depth formats). image None: unsupported (logged once)
FormatInfo format_info(uint32_t gx2Format, bool isDepth);
void convert_row(Convert c, const uint8_t* src, uint8_t* dst, uint32_t count);

// ---- descriptors (descriptors.cpp). The image and sampler descriptor sets of memory.cpp are bound once
// per frame by begin_commands. Slot ranges: images 0..kReservedImageIds-1 and samplers
// 0..kReservedSamplerIds-1 belong to the renderer's own passes (overlay_dk.cpp: images 1-63 and sampler 0;
// the present pass: image 0 for the game's picture and samplers 1-2: linear and nearest, clamped). The
// rest belong to this lane, which allocates and recycles them; nobody else writes descriptors there.
constexpr uint32_t kReservedImageIds = 64, kReservedSamplerIds = 16;
constexpr uint32_t kPresentImageId = 0, kPresentLinearSamplerId = 1, kPresentNearestSamplerId = 2;
// a free image descriptor slot; fatal() naming the count when the 8192 slots are used up
uint32_t image_descriptor_alloc();
// records the descriptor of view into slot id (dkCmdBufPushData); visible after commit_descriptors()
void image_descriptor_write(uint32_t id, const DkImageView& view);
// the slot is reused once the GPU has finished the frame being recorded
void image_descriptor_free_later(uint32_t id);
// the sampler descriptor slot for GX2 sampler words (SQ_TEX_SAMPLER_WORD0..2): cached by its words and
// the two flags; compare: a depth-compare sampler; integer: an integer texture (nearest filtering only).
// When the cache is full the least recently used slot that no in-flight frame uses is rewritten.
uint32_t sampler_id(const uint32_t* samplerWords, bool compare, bool integer);
// a sampler slot from an earlier sampler_id is used by this frame too (the draw path's texture cache): the
// cache's eviction then leaves it alone until the GPU is done with this frame. (An eviction advances
// R.surfaceEpoch: cached lookups of the evicted slot are redone.)
void sampler_used(uint32_t id);
// 16x anisotropic filtering (WWHD_ANISO, the overlay's Effects): any thread; samplers follow from the next frame
bool aniso_enabled();
void set_aniso(bool on);
// the barrier that makes this frame's descriptor writes visible (nothing when none since the last one)
void commit_descriptors();
// the image descriptor of a 1x1 transparent black texture (shaders sampling a unit that has no surface)
uint32_t null_image_id();
struct DescriptorStats {
    uint32_t imagesUsed = 0, samplersUsed = 0;  // slots in use now
    uint64_t imageWrites = 0, samplerWrites = 0, samplerHits = 0, samplerEvictions = 0;
};
DescriptorStats descriptor_stats_take();

// ---- surfaces (surfaces.cpp)
struct GuestLayout;  // address-library geometry of each mip level (surfaces.cpp)
struct SurfaceDesc {
    uint32_t addr = 0, mipAddr = 0, width = 0, height = 0, slices = 1, pitch = 0, mips = 1, format = 0, dim = 1,
             tileMode = 0, swizzle = 0;
    bool isDepth = false;
};
// a DkImage in the image heap with the descriptor of each view that draws sampled
struct SurfaceImage {
    DkImage image{};
    DkImageLayout layout{};
    ImageAlloc mem;               // memory.cpp: freed with image_free_later
    bool valid = false;           // image initialized
    DkImageType type = DkImageType_2D;
    uint32_t pw = 0, ph = 0;      // pixels (the guest size times the surface's scale)
    uint32_t layers = 1;          // array layers (1 for 3D: depth is `slices`)
    uint32_t flags = 0;           // its DkImageFlags and format (an image kept for a rescale is reused only with
    DkImageFormat format = DkImageFormat_None;  // the same)
    uint32_t imageId = 0;         // descriptor of the default view (all mips and layers, identity swizzle); 0: none
    // sampled views by (view type, format, swizzle, first mip, mip count) key: their descriptor slots
    std::unordered_map<uint64_t, uint32_t> views;
};
struct Surface {
    SurfaceImage img;
    // the guest's description (as gfx/gl's)
    uint32_t addr = 0, mipAddr = 0, width = 0, height = 0, slices = 1, pitch = 0, mips = 1, format = 0, dim = 1,
             tileMode = 0, swizzle = 0;
    bool isDepth = false, gpuWritten = false, dirty = true;
    uint64_t writeSeq = 0, contentHash = 0, lastCheckedFrame = ~0ull, sparseHash = 0;
    uint64_t changedFrame = 0;   // the last frame its guest data was uploaded (upload_surface)
    uint64_t drawFrame = ~0ull;  // the last frame a draw rendered to it, and how many draws did
    uint32_t frameDraws = 0;
    // the GamePad picture (gamepad_only, dk_draw.h), as gfx/gl's
    uint64_t drcScanFrame = ~0ull, tvScanFrame = ~0ull, readFrame = ~0ull;
    bool gamepadSource = false;
    uint64_t gamepadSourceSince = ~0ull;
    bool tvShared = false;
    bool skipLogged = false;
    Surface* derivedFrom = nullptr;
    uint32_t dataSize = 0;
    // internal resolution: the image has img.pw x img.ph pixels for the guest's width x height. Only
    // screen-shaped render targets get a scale other than 1 (P3); lookups and guest memory keep the guest size.
    float scale = 1.0f;
    bool renderTarget = false;  // created or used as a render target
    bool scalable = false;      // screen-shaped: takes res_scale() as a render target
    bool hudFull = false;       // the TV picture's buffer at full resolution for the HUD (draw.cpp)
    SurfaceImage twin;          // the TV picture's other image (scaled scene / full-resolution HUD)
    float twinScale = 0;
    FormatInfo fmt;
    std::shared_ptr<GuestLayout> guest;
};

// every surface, by address and as a list (surfaces.cpp); the TV scan buffer state (copy_to_scan)
struct SurfaceSet {
    std::unordered_multimap<uint32_t, std::unique_ptr<Surface>> byAddr;
    std::vector<Surface*> list;
    std::unique_ptr<Surface> tvScan;
    Surface* tvSource = nullptr;  // the buffer last copied to the TV scan buffer
    Surface* scanSrc = nullptr;   // ... while its copy is not made yet (present reads it directly)
};
extern SurfaceSet S;

uint64_t next_write_seq();
inline void mark_gpu_written(Surface* s) {
    if (!s->gpuWritten) {
        s->gpuWritten = true;
        R.surfaceEpoch++;
    }
    s->writeSeq = next_write_seq();
}
// s is read by something other than the GamePad picture (gamepad_only)
inline void note_read(Surface* s) {
    s->readFrame = R.frame;
    if (s->gamepadSource && R.frame > s->gamepadSourceSince) s->tvShared = true;
    if (s->derivedFrom) s->derivedFrom->readFrame = R.frame;
}
// the TV picture: GX2CopyColorBufferToScanBuffer only notes its buffer; presenting reads that buffer unless
// something writes to it first: scan_flush makes the copy then
void scan_flush();
inline void before_write(const Surface* s) {
    if (s && s == S.scanSrc) scan_flush();
}

Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering);
// the render targets the registers describe (CB_COLORn_*, DB_*), created when new; *slice: the layer
Surface* color_target(const uint32_t* regs, int i, uint32_t* slice);
Surface* depth_target(const uint32_t* regs, uint32_t* slice);
Surface* surface_from_color_buffer(uint32_t addr, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
Surface* surface_from_depth_buffer(uint32_t addr, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
// the surface a texture unit's 7 SQ_TEX_RESOURCE words describe (created and uploaded when new); unique:
// whether it is the only surface at its address (a cached lookup then depends only on the words)
Surface* sampled_texture(const uint32_t* texWords, bool isDepthSampler, bool* unique = nullptr);
// guest memory -> image when the guest data changed (checked once per frame; dirty, sparse hash): stream
// slice + dkCmdBufCopyBufferToImage per level and layer, then the barrier that orders it before later use
void upload_surface(Surface* s);
// the image descriptor slot of the view the texture words select (dimension, format, swizzle, mips)
uint32_t sampled_view_id(Surface* s, const uint32_t* texWords);
// a view of one level/layer of the surface's image for dkCmdBufBindRenderTargets (draw.cpp, clears)
void target_view(Surface* s, uint32_t level, uint32_t layer, DkImageView* out);
void create_surface_image(Surface* s);   // the image at the surface's scale (and its default descriptor)
void destroy_surface_image(Surface* s);  // image memory and descriptors freed once the GPU is done
// a snapshot of a surface the draw also renders to (sampling a bound target is undefined): a copy made
// when the surface was written since the last one
Surface* feedback_copy(Surface* s);
// copy (scaled) between two surfaces' levels/layers; depth always through dkCmdBufBlitImage (plan section 3)
void blit(Surface* src, uint32_t srcLevel, uint32_t srcLayer, uint32_t sw, uint32_t sh, Surface* dst,
          uint32_t dstLevel, uint32_t dstLayer, uint32_t dw, uint32_t dh);

// ---- GX2 operations (render::Backend entries)
void clear_color(const uint32_t* regs, uint32_t colorBuffer, const float rgba[4]);
void clear_depth_stencil(const uint32_t* regs, uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice);
void copy_to_scan(uint32_t colorBuffer, uint32_t target);  // target 1: TV, else the GamePad
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
void ss_reset_surfaces();  // a save state was loaded: every surface re-read from guest memory

// ---- presentation (backend.cpp's present pass samples the game's picture)
struct PresentSource {
    Surface* surface = nullptr;  // null: no picture yet (the present pass shows the background)
    uint32_t imageId = kPresentImageId;  // its default view's descriptor, written into the reserved slot
    uint32_t pw = 0, ph = 0;             // the image's pixels
    uint32_t width = 0, height = 0;      // the guest's size
    bool srgb = false;                   // an sRGB format: present encodes as R.tvSrgb says
};
// the TV picture to present: the scan buffer, or the noted buffer itself when nothing wrote to it since the
// game copied it (gfx/gl present); writes its view into kPresentImageId (commit_descriptors() follows)
PresentSource present_source();

// ---- internal resolution (gfx/gl's, surfaces.cpp): screen-shaped render targets get res_scale() x their guest
// size (WWHD_RES_SCALE, 0.5..2; dynamic resolution, backend.cpp, lowers it while the GPU is the limit)
inline uint32_t scaled_size(uint32_t v, float scale) {
    return scale == 1.0f ? v : std::max<uint32_t>(1, uint32_t(v * scale + 0.99f));
}
float res_scale();                // the factor render targets get this frame (render thread)
float requested_res_scale();      // the factor asked for (any thread)
float res_scale_shown();          // the factor in use, for the settings overlay (any thread)
void set_res_scale(float scale);  // from the next frame on (any thread)
void latch_res_scale();           // present: apply a requested change; the frame's allocation budget
void rescale_surface(Surface* s, float scale, bool keepContents, bool keepOld = false);
// a render target about to be written: the scale it should have unless that needs a new image and this
// frame's allocations are used up (false: still at its old scale); force: in any case
bool fit_scale(Surface* s, bool keepContents, bool force = false);
size_t rescale_pool_bytes();      // images a rescale let go, kept for the next one (stats)

// once per frame from begin_commands: retire freed descriptor slots and images the GPU is done with
void surfaces_frame_start();
// image heap bytes of all surfaces (stats): total, render targets, surface count
void surface_memory(size_t& total, size_t& targets, size_t& count);

}  // namespace gfxdk

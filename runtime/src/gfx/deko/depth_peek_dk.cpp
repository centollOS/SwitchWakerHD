// (this fork, issue #94) Depth peeks on deko3d: the sun's corona and lens flare (peekz.cpp). The game asks for
// the depth at a few points around the sun once a frame, after the 3D scene (OP_PEEK_Z), and judges the answers
// a frame later; gfx/vulkan and Metal copy those pixels of the main depth buffer and write the answers when the
// GPU is done. Here the same: the copy engine copies each point's pixel of the newest 1280x720 depth buffer into
// a slot of CPU-visible memory, and the answers are written (depth_peek::publish) at a later peek, once the
// frame of that copy is done (frame_done: no wait for the GPU). Method from SwitchWaker #7 (its dk_encode.cpp
// depth peek: copy engine, slots in flight, the barrier before the CPU reads). WWHD_PEEKZ=0: the game's own
// function (no corona, no flare).
#include <deko3d.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "dk.h"
#include "dk_surfaces.h"
#include "dk_sync.h"
#include "gfx/depth_peek.h"
#include "runtime.h"

namespace gfxdk {
namespace {
constexpr uint32_t kSlots = 3;         // one answered, up to two in flight (the GPU runs a frame or two behind)
constexpr uint32_t kPoints = 64;       // dDlst_peekZ_c's cells
constexpr uint32_t kStride = 256;      // bytes per point in a slot (one copy each, as capture.cpp's aligned rows)
constexpr uint32_t kSlotBytes = kPoints * kStride;

struct Slot {
    bool inFlight = false;
    uint64_t frame = 0;                // the frame whose commands hold the copies
    uint64_t ticket = 0;
    DkImageFormat format = DkImageFormat_None;
    std::vector<uint32_t> destinations;  // guest addresses of the answers
};
DkMemBlock g_mem = nullptr;
Slot g_slots[kSlots];
uint64_t g_copies = 0, g_skipped = 0;

bool create_memory() {
    if (g_mem) return true;
    LOG("[dk] depth peek: creating its memory block (%u KiB, CPU-uncached)", kSlots * kSlotBytes >> 10);
    log_flush();  // deko3d aborts when a creation fails
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, kSlots * kSlotBytes);
    m.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached;
    g_mem = dkMemBlockCreate(&m);
    return g_mem != nullptr;
}

// the depth of one copied pixel as the game reads it (24-bit, 0xFFFFFF: nothing drawn there)
uint32_t depth_value(DkImageFormat f, const uint8_t* p) {
    if (f == DkImageFormat_Z16) {
        uint16_t v;
        memcpy(&v, p, 2);
        return v == 0xFFFF ? 0xFFFFFFu : uint32_t(v) << 8;
    }
    if (f == DkImageFormat_Z24S8) {  // (the renderer's own buffers; guest D24 surfaces are ZF32_X24S8)
        uint32_t v;
        memcpy(&v, p, 4);
        return v >> 8;
    }
    float z;  // ZF32, ZF32_X24S8 (the float first)
    memcpy(&z, p, 4);
    return ::gfx::depth_peek::from_float(z);
}

// the answers of every slot whose frame the GPU has finished
void answer_done() {
    for (uint32_t i = 0; i < kSlots; i++) {
        Slot& s = g_slots[i];
        if (!s.inFlight || !frame_done(s.frame)) continue;
        const auto* raw = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(g_mem)) + size_t(i) * kSlotBytes;
        std::vector<uint32_t> depths(s.destinations.size());
        for (size_t k = 0; k < depths.size(); k++) depths[k] = depth_value(s.format, raw + k * kStride);
        ::gfx::depth_peek::publish(s.ticket, s.destinations, depths);
        s.inFlight = false;
    }
}

// the main depth buffer: the 1280x720 depth surface written last (the scene the game just drew)
Surface* main_depth() {
    Surface* best = nullptr;
    for (Surface* s : S.list) {
        if (s->isDepth && s->width == 1280 && s->height == 720 && s->gpuWritten && s->img.valid &&
            (!best || s->writeSeq > best->writeSeq))
            best = s;
    }
    return best;
}

bool supported(DkImageFormat f) {
    return f == DkImageFormat_Z16 || f == DkImageFormat_ZF32 || f == DkImageFormat_Z24S8 || f == DkImageFormat_ZF32_X24S8;
}
}  // namespace

// cells: (x, y) in the game's 640x480 space and the guest address of the answer, n words (triples)
void peek_z(const uint32_t* cells, uint32_t n) {
    begin_commands();  // (as every recording entry of the renderer table)
    if (!create_memory()) return;
    answer_done();
    const uint32_t points = std::min(n / 3, kPoints);
    Surface* depth = main_depth();
    if (!points || !depth || !supported(depth->img.format)) return;
    Slot* slot = nullptr;
    for (uint32_t i = 0; i < kSlots && !slot; i++)
        if (!g_slots[i].inFlight) slot = &g_slots[i];
    if (!slot) {  // two copies still in flight: the game asks again next frame
        g_skipped++;
        return;
    }
    const uint32_t index = uint32_t(slot - g_slots);
    const DkGpuAddr base = dkMemBlockGetGpuAddr(g_mem) + DkGpuAddr(index) * kSlotBytes;
    sync_transfer_begin(true);  // after the draws that wrote the depth buffer
    DkImageView v;
    dkImageViewDefaults(&v, &depth->img.image);
    slot->destinations.resize(points);
    for (uint32_t i = 0; i < points; i++) {
        // the image's own pixels: the internal resolution scales the whole buffer (img.pw x img.ph)
        const uint32_t x = uint32_t(::gfx::depth_peek::pixel(cells[i * 3], depth->img.pw, 640));
        const uint32_t y = uint32_t(::gfx::depth_peek::pixel(cells[i * 3 + 1], depth->img.ph, 480));
        const DkImageRect rect = {x, y, 0, 1, 1, 1};
        const DkCopyBuf dst = {base + DkGpuAddr(i) * kStride, 0, 0};
        dkCmdBufCopyImageToBuffer(R.cmd, &v, &rect, &dst, 0);
        slot->destinations[i] = cells[i * 3 + 2];
    }
    // the copies out of the GPU's L2 into memory before the CPU reads them, and done before later draws write
    // the depth buffer (as SwitchWaker's depth peek and capture.cpp's readback)
    sync_barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache, SyncWhy::TransferAfter);
    slot->inFlight = true;
    slot->frame = R.frame + 1;  // the frame being recorded (begin_commands opened it)
    slot->format = depth->img.format;
    slot->ticket = ::gfx::depth_peek::next_ticket();
    if (g_copies++ == 0)
        LOG("[dk] depth peek: first copy, %u points of the depth buffer %08X (%ux%u pixels, format %d)", points,
            depth->addr, depth->img.pw, depth->img.ph, int(depth->img.format));
}
}  // namespace gfxdk

// deko3d memory (dk.h; docs/deko3d-plan.md section 3): the per-frame stream and command memory rings
// with a fence per frame, the image heap (64 MB chunks, first-fit suballocation, frees deferred until
// the GPU is past the frame), the shader code block and the descriptor sets. Render thread only, except
// memory_init (main thread, before the render thread starts).
#include "dk.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dk_bisect.h"
#include "runtime.h"

namespace gfxdk {
namespace {

// deko3d aborts the process when a creation fails (release library: diagAbortWithResult, 2359-xxxx; the debug
// one: debug_message, then a trap) and never returns null: the line before names what was being created
DkMemBlock make_block(uint32_t size, uint32_t flags, const char* what) {
    LOG("[dk] creating a memory block: %s, %u KiB (flags 0x%X)", what, size >> 10, flags);
    log_flush();
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, size);
    m.flags = flags;
    DkMemBlock b = dkMemBlockCreate(&m);
    LOG("[dk] memory block: %s at GPU address 0x%llX", what, (unsigned long long)dkMemBlockGetGpuAddr(b));
    return b;
}

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// ---- per-frame slots: a fence, a stream slice and a command memory slice each
struct Slot {
    DkFence fence{};              // after the commands of the frame that used the slot last
    bool fenced = false;          // the fence was signalled once (an unused DkFence is not waited for)
    uint32_t streamUsed = 0;
    uint32_t cmdUsed = 0;         // command memory fed from the slice
    // blocks created when the slice ran out; the slot keeps them and feeds them again the next time it
    // runs out (only after its fence, like the slice)
    std::vector<std::pair<DkMemBlock, uint32_t>> cmdOverflow;  // block, size
    size_t cmdOverflowUsed = 0;
    std::vector<ImageAlloc> retired;      // image memory freed while the slot's frame was recorded
};
Slot g_slots[kFrames];
uint32_t g_slot = 0;
bool g_inFrame = false;
DkMemBlock g_stream = nullptr, g_cmdMem = nullptr, g_code = nullptr, g_descriptors = nullptr, g_queries = nullptr;
uint8_t* g_streamCpu = nullptr;
DkGpuAddr g_streamGpu = 0;
uint32_t g_codeUsed = 0;
uint64_t g_cmdFedThisFrame = 0;
MemoryStats g_stats;

// the command buffer asks for memory during a frame (frame_begin feeds the first 64 KB): the next 64 KB of
// this frame's slice, then the slot's overflow blocks
void add_cmd_memory(void*, DkCmdBuf cmd, size_t minReqSize) {
    Slot& s = g_slots[g_slot];
    const uint32_t want = align_up(uint32_t(std::max<size_t>(minReqSize, kCmdChunk)), DK_MEMBLOCK_ALIGNMENT);
    if (s.cmdUsed + want <= kCmdSliceSize) {
        dkCmdBufAddMemory(cmd, g_cmdMem, g_slot * kCmdSliceSize + s.cmdUsed, want);
        s.cmdUsed += want;
        g_cmdFedThisFrame += want;
        return;
    }
    if (s.cmdOverflowUsed == 0) g_stats.cmdOverflows++;  // the frame's first overflow
    while (s.cmdOverflowUsed < s.cmdOverflow.size()) {
        auto [block, size] = s.cmdOverflow[s.cmdOverflowUsed++];
        if (size < want) continue;
        dkCmdBufAddMemory(cmd, block, 0, size);
        g_cmdFedThisFrame += size;
        return;
    }
    const uint32_t size = std::max<uint32_t>(want, 1u << 20);
    LOG("[dk] command memory: frame %llu needs more than its %u MiB slice; creating a %u KiB block for slot %u",
        (unsigned long long)R.frame + 1, kCmdSliceSize >> 20, size >> 10, g_slot);
    log_flush();  // a failed creation aborts (make_block)
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, size);
    m.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock b = dkMemBlockCreate(&m);
    s.cmdOverflow.push_back({b, size});
    s.cmdOverflowUsed = s.cmdOverflow.size();
    dkCmdBufAddMemory(cmd, b, 0, size);
    g_cmdFedThisFrame += size;
}

// ---- image heap
struct Chunk {
    DkMemBlock block;
    uint32_t size;
    std::vector<std::pair<uint32_t, uint32_t>> free;  // offset, size; sorted by offset
};
std::vector<Chunk> g_chunks;

void add_chunk(uint32_t size) {
    char what[64];
    snprintf(what, sizeof what, "image heap chunk %zu", g_chunks.size());
    g_chunks.push_back({make_block(size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, what), size, {{0, size}}});
    g_stats.imageChunks = g_chunks.size();
}

void chunk_free(Chunk& c, uint32_t offset, uint32_t size) {
    auto it = std::lower_bound(c.free.begin(), c.free.end(), std::make_pair(offset, 0u));
    it = c.free.insert(it, {offset, size});
    // merge with the neighbours
    if (it + 1 != c.free.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        c.free.erase(it + 1);
    }
    if (it != c.free.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        c.free.erase(it);
    }
}

}  // namespace

void memory_init() {
    // command buffer memory: fed as the command buffer asks (usage measured in 64 KB steps)
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, R.device);
    cm.cbAddMem = add_cmd_memory;
    LOG("[dk] creating the command buffer");
    log_flush();
    R.cmd = dkCmdBufCreate(&cm);
    g_cmdMem = make_block(kFrames * kCmdSliceSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                          "command memory ring (4 frames)");
    g_stream = make_block(kFrames * kStreamSliceSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                          "stream ring (4 frames: vertices, indices, uniforms, uploads)");
    g_streamCpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_stream));
    g_streamGpu = dkMemBlockGetGpuAddr(g_stream);
    g_code = make_block(kCodeSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                        "shader code");
    g_descriptors = make_block(align_up((kImageDescriptors + kSamplerDescriptors) * 32, DK_MEMBLOCK_ALIGNMENT),
                               DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                               "descriptors (8192 images, 1024 samplers)");
    g_queries = make_block(kQuerySize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "queries");
    add_chunk(kImageChunkSize);  // the first image heap chunk now: its size shows in the start-up memory figures
}

void frame_begin(uint64_t frame) {
    g_slot = uint32_t(frame % kFrames);
    Slot& s = g_slots[g_slot];
    if (s.fenced) {
        Stage stage("deko3d: waiting for the GPU (frame fence)");
        for (int waited = 0;; waited++) {
            const DkResult r = dkFenceWait(&s.fence, 2'000'000'000ll);
            if (r == DkResult_Success) break;
            LOG("[dk] frame %llu: the GPU has not finished frame %llu after %d s (result %d)%s",
                (unsigned long long)frame, (unsigned long long)(frame - kFrames), (waited + 1) * 2, int(r),
                dkQueueIsInErrorState(R.queue) ? "; the queue is in an error state" : "");
            if (waited >= 4) {
                LOG("[dk] giving up on the fence: continuing (the slot's memory may still be in use)");
                break;
            }
        }
    }
    for (const ImageAlloc& a : s.retired)
        if (a.chunk >= 0) chunk_free(g_chunks[size_t(a.chunk)], a.offset, a.size);
    s.retired.clear();
    s.streamUsed = 0;
    // dkCmdBufClear rewinds to the start of the memory fed last (deko3d 0.5.0 CmdBuf::clear keeps it), which
    // belongs to the previous frame: feed this slot's first chunk explicitly (as deko_examples' CCmdMemRing);
    // the command buffer then no longer points into another slot's memory or an overflow block
    dkCmdBufClear(R.cmd);
    dkCmdBufAddMemory(R.cmd, g_cmdMem, g_slot * kCmdSliceSize, kCmdChunk);
    s.cmdUsed = kCmdChunk;
    s.cmdOverflowUsed = 0;
    g_cmdFedThisFrame = kCmdChunk;
    g_inFrame = true;
}

void frame_end() {
    Slot& s = g_slots[g_slot];
    dkCmdBufSignalFence(R.cmd, &s.fence, true);
    s.fenced = true;
    g_inFrame = false;
    g_stats.frames++;
    g_stats.cmdBytesSum += g_cmdFedThisFrame;
    g_stats.cmdBytesMax = std::max(g_stats.cmdBytesMax, g_cmdFedThisFrame);
    g_stats.streamBytesSum += s.streamUsed;
}

bool frame_done(uint64_t frame) {
    Slot& s = g_slots[frame % kFrames];
    return frame == 0 || !s.fenced || dkFenceWait(&s.fence, 0) == DkResult_Success;
}

bool frame_open() { return g_inFrame; }

StreamAlloc stream_alloc(uint32_t size, uint32_t alignment) {
    Slot& s = g_slots[g_slot];
    const uint32_t offset = align_up(s.streamUsed, std::max<uint32_t>(alignment, 4));
    if (!g_inFrame || offset + size > kStreamSliceSize) {
        static int logged = 0;
        if (logged++ < 20)
            LOG("[dk] stream: %u bytes do not fit in frame %llu's %u MiB slice (%u used)%s", size,
                (unsigned long long)R.frame + 1, kStreamSliceSize >> 20, s.streamUsed, g_inFrame ? "" : " (outside a frame)");
        g_stats.streamFull++;
        return {};
    }
    s.streamUsed = offset + size;
    const uint32_t at = g_slot * kStreamSliceSize + offset;
    return {g_streamCpu + at, g_streamGpu + at};
}

StreamSlice stream_upload(const void* data, uint32_t size, uint32_t alignment, uint32_t zeroTail) {
    const StreamAlloc a = stream_alloc(size + zeroTail, alignment);
    if (!a) return {};
    {
        SampledTime copy{R.perf.copyNs, R.timedDraw};
        if (size) memcpy(a.cpu, data, size);
        if (zeroTail) memset(static_cast<uint8_t*>(a.cpu) + size, 0, zeroTail);
    }
    R.perf.streamBytes += size + zeroTail;
    return {a.gpu, size + zeroTail};
}

namespace {
struct UploadEntry {
    uint64_t key = 0, stamp = ~0ull, gen = 0;
    uint32_t size = 0;
    StreamSlice slice;
};
FrameTable<UploadEntry> g_uploads;
}  // namespace

// Guest vertex and uniform data may not change between GX2Invalidate calls (or GX2DrawDone) while a
// frame's draws can still read it, so an address uploaded once this frame is reused (gfx/gl)
StreamSlice stream_guest(uint32_t addr, uint32_t size, uint32_t alignment, uint32_t zeroTail) {
    static const bool enabled = !getenv("WWHD_DK_NO_DEDUP");
    const uint64_t key = uint64_t(addr) | uint64_t(alignment) << 32 | uint64_t(zeroTail) << 48;
    const uint64_t stamp = R.frame + 1;  // the frame being recorded
    const UploadEntry* e = g_uploads.find(key, stamp);
    if (enabled && e->stamp == stamp && e->gen == R.streamGen && e->size >= size) {
        R.perf.reusedBytes += size;
        return e->slice;
    }
    const StreamSlice slice = stream_upload(mem::ptr(addr), size, alignment, zeroTail);
    if (slice) g_uploads.put({key, stamp, R.streamGen, size, slice});
    return slice;
}

void image_tile_size_fix(DkImageLayoutMaker& m, uint32_t rows) {
    if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) return;
    if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D ||
        m.type == DkImageType_Buffer)
        return;  // (3D images: deko3d's depth tile never shrinks at level 0)
    if (m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) {
        // the multisampled height (deko3d: m_samplesY)
        if (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x) rows *= 2;
    }
    // deko3d's choice (dkImageLayoutInitialize: pickTileSize of 1.5 x the height in GOBs) ...
    const uint32_t gobs = (rows + rows / 2 + 7) / 8;
    uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
    // ... and its level-0 shrink (calcLevelOffset: adjustTileSize(m_tileH, 8, height))
    uint32_t shrunk = tile;
    while (shrunk && (8u << (shrunk - 1)) >= rows) shrunk--;
    if (shrunk == tile) return;  // deko3d's own layout is consistent
    m.flags |= DkImageFlags_CustomTileSize;
    m.tileSize = DkTileSize(shrunk);
}

ImageAlloc image_alloc(uint32_t size, uint32_t alignment) {
    size = align_up(size, 256);
    alignment = std::max<uint32_t>(alignment, 256);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t ci = 0; ci < g_chunks.size(); ci++) {
            Chunk& c = g_chunks[ci];
            for (size_t i = 0; i < c.free.size(); i++) {
                auto [off, len] = c.free[i];
                const uint32_t at = align_up(off, alignment);
                if (at - off + size > len) continue;
                // split: the part before the aligned start and the rest stay free
                c.free.erase(c.free.begin() + long(i));
                if (at > off) chunk_free(c, off, at - off);
                if (at + size < off + len) chunk_free(c, at + size, off + len - at - size);
                g_stats.imageBytes += size;
                return {c.block, at, size, int(ci)};
            }
        }
        // no room: a new chunk (bigger than 64 MB only for a bigger image)
        add_chunk(std::max(kImageChunkSize, align_up(size + alignment, DK_MEMBLOCK_ALIGNMENT)));
    }
    fatal("[dk] image heap: cannot place %u bytes", size);
}

void image_free_later(const ImageAlloc& a) {
    if (a.chunk < 0) return;
    g_stats.imageBytes -= a.size;
    g_slots[g_slot].retired.push_back(a);
}

bool code_load(DkShader& shader, const void* dksh, uint32_t size, const char* name) {
    const uint32_t at = align_up(g_codeUsed, DK_SHADER_CODE_ALIGNMENT);
    if (at + size > kCodeSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
        LOG("[dk] shader code memory full: %s (%u bytes) not loaded", name, size);
        return false;
    }
    uint8_t* const dst = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_code)) + at;
    memcpy(dst, dksh, size);
    bisect_patch_dksh(dst, size);  // grass bisection switches (dk_bisect.h)
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, g_code, at);
    dkShaderInitialize(&shader, &m);
    if (!dkShaderIsValid(&shader)) {
        LOG("[dk] shader %s (%u bytes of DKSH) is not valid", name, size);
        return false;
    }
    g_codeUsed = at + size;
    g_stats.codeBytes = g_codeUsed;
    return true;
}

QueryMemory query_memory() {
    return {static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_queries)), dkMemBlockGetGpuAddr(g_queries)};
}

DkGpuAddr image_descriptors() { return dkMemBlockGetGpuAddr(g_descriptors); }
DkGpuAddr sampler_descriptors() { return dkMemBlockGetGpuAddr(g_descriptors) + kImageDescriptors * 32; }

MemoryStats memory_stats_take() {
    MemoryStats s = g_stats;
    g_stats.cmdBytesMax = g_stats.cmdBytesSum = g_stats.cmdOverflows = 0;
    g_stats.streamBytesSum = g_stats.streamFull = g_stats.frames = 0;
    return s;
}

}  // namespace gfxdk

// deko3d memory (dk.h; docs/deko3d-plan.md section 3): the per-frame stream and command memory rings
// with a fence per frame, the image heap (64 MB chunks, first-fit suballocation, frees deferred until
// the GPU is past the frame), the shader code block and the descriptor sets. Render thread only, except
// memory_init (main thread, before the render thread starts).
#include "dk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "runtime.h"

namespace gfxdk {
namespace {

DkMemBlock make_block(uint32_t size, uint32_t flags, const char* what) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, size);
    m.flags = flags;
    DkMemBlock b = dkMemBlockCreate(&m);
    if (!b) fatal("[dk] cannot create the %s memory block (%u MiB)", what, size >> 20);
    LOG("[dk] memory block: %s, %u KiB (flags 0x%X), GPU address 0x%llX", what, size >> 10, flags,
        (unsigned long long)dkMemBlockGetGpuAddr(b));
    return b;
}

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// ---- per-frame slots: a fence, a stream slice and a command memory slice each
struct Slot {
    DkFence fence{};              // after the commands of the frame that used the slot last
    bool fenced = false;          // the fence was signalled once (an unused DkFence is not waited for)
    uint32_t streamUsed = 0;
    uint32_t cmdUsed = 0;         // command memory fed from the slice
    std::vector<DkMemBlock> cmdOverflow;  // blocks created when the slice ran out (freed with the slot)
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

// the command buffer asks for memory: the next 64 KB of this frame's slice, then blocks of its own
void add_cmd_memory(void*, DkCmdBuf cmd, size_t minReqSize) {
    Slot& s = g_slots[g_slot];
    const uint32_t want = align_up(uint32_t(std::max<size_t>(minReqSize, kCmdChunk)), DK_MEMBLOCK_ALIGNMENT);
    if (s.cmdUsed + want <= kCmdSliceSize) {
        dkCmdBufAddMemory(cmd, g_cmdMem, g_slot * kCmdSliceSize + s.cmdUsed, want);
        s.cmdUsed += want;
        g_cmdFedThisFrame += want;
        return;
    }
    const uint32_t size = std::max<uint32_t>(want, 1u << 20);
    static int logged = 0;
    if (logged++ < 20)
        LOG("[dk] command memory: frame %llu needs more than its %u MiB slice; a %u KiB block is added",
            (unsigned long long)R.frame + 1, kCmdSliceSize >> 20, size >> 10);
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, size);
    m.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock b = dkMemBlockCreate(&m);
    if (!b) fatal("[dk] out of memory for command memory (%u KiB)", size >> 10);
    s.cmdOverflow.push_back(b);
    dkCmdBufAddMemory(cmd, b, 0, size);
    g_cmdFedThisFrame += size;
    g_stats.cmdOverflows++;
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
    R.cmd = dkCmdBufCreate(&cm);
    if (!R.cmd) fatal("[dk] cannot create a command buffer");
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
    for (DkMemBlock b : s.cmdOverflow) dkMemBlockDestroy(b);
    s.cmdOverflow.clear();
    for (const ImageAlloc& a : s.retired)
        if (a.chunk >= 0) chunk_free(g_chunks[size_t(a.chunk)], a.offset, a.size);
    s.retired.clear();
    s.streamUsed = 0;
    s.cmdUsed = 0;
    g_cmdFedThisFrame = 0;
    // a cleared command buffer has no memory: the first command asks for some (add_cmd_memory)
    dkCmdBufClear(R.cmd);
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
    memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_code)) + at, dksh, size);
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

DkGpuAddr image_descriptors() { return dkMemBlockGetGpuAddr(g_descriptors); }
DkGpuAddr sampler_descriptors() { return dkMemBlockGetGpuAddr(g_descriptors) + kImageDescriptors * 32; }

MemoryStats memory_stats_take() {
    MemoryStats s = g_stats;
    g_stats.cmdBytesMax = g_stats.cmdBytesSum = g_stats.cmdOverflows = 0;
    g_stats.streamBytesSum = g_stats.streamFull = g_stats.frames = 0;
    return s;
}

}  // namespace gfxdk

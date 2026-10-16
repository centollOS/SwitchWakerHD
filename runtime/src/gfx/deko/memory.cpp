// deko3d memory (dk.h; docs/deko3d-plan.md section 3): the per-frame stream and command memory rings
// with a fence per frame, the image heap (64 MB chunks, first-fit suballocation, frees deferred until
// the GPU is past the frame), the shader code block and the descriptor sets. Render thread only, except
// memory_init (main thread, before the render thread starts).
#include "dk.h"
#include "render_prof.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "runtime.h"

namespace gfxdk {
namespace {

// deko3d aborts the process when a creation fails (release library: diagAbortWithResult, 2359-xxxx; the debug
// one: debug_message, then a trap) and never returns null: the line before names what was being created
DkMemBlock make_block(uint32_t size, uint32_t flags, const char* what, void* storage = nullptr) {
    LOG("[dk] creating a memory block: %s, %u KiB (flags 0x%X)", what, size >> 10, flags);
    log_flush();
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, size);
    m.flags = flags;
    m.storage = storage;
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

// (this fork, memory round 2) WWHD_DK_IMAGE_TRACE: the heap's events for image_heap_sim.py. A free names the
// placement it ends by its number (offsets differ under another policy). Little-endian records of 16 bytes.
struct TraceEvent {
    enum Kind : uint8_t { Alloc = 1, Free = 2, Pressure = 3, NewChunk = 4 };
    uint8_t kind;
    uint8_t alignLog2;  // Alloc: log2 of the alignment
    uint16_t pad = 0;
    uint32_t frame;
    uint32_t id;    // Alloc/Free: the placement's number; NewChunk: the chunk's index
    uint32_t size;  // bytes (Alloc, Free, NewChunk), Pressure: the request that found no room
};
static_assert(sizeof(TraceEvent) == 16);
struct Trace {
    bool on = false;
    std::vector<TraceEvent> events;
    std::unordered_map<uint64_t, uint32_t> live;  // chunk << 32 | offset -> placement number
    uint32_t next = 0;
} g_trace;
const char* const kTracePath = "image_trace.bin";  // sdmc:/switch/wwhd (the working directory)

void trace(TraceEvent::Kind kind, uint32_t id, uint32_t size, uint32_t alignment = 1) {
    g_trace.events.push_back({kind, uint8_t(__builtin_ctz(alignment)), 0, uint32_t(R.frame), id, size});
}
void trace_placed(int chunk, uint32_t offset, uint32_t size, uint32_t alignment) {
    if (!g_trace.on) return;
    const uint32_t id = g_trace.next++;
    g_trace.live[uint64_t(chunk) << 32 | offset] = id;
    trace(TraceEvent::Alloc, id, size, alignment);
}
void trace_freed(int chunk, uint32_t offset, uint32_t size) {
    if (!g_trace.on) return;
    auto it = g_trace.live.find(uint64_t(chunk) << 32 | offset);
    if (it == g_trace.live.end()) return;
    trace(TraceEvent::Free, it->second, size);
    g_trace.live.erase(it);
}

// (this fork) docs/first-start-memory-plan.md step 3: the chunk's memory is taken here first (deko3d's own allocation
// aborts the game when the heap has no such free range: after a first start's shader preparation, a Picto Box
// picture did); false when there is none. WWHD_DK_IMAGE_CHUNKS_MAX=n ([dev], a test): no chunk after the n-th.
bool add_chunk(uint32_t size) {
    static const size_t maxChunks = [] {
        const char* e = getenv("WWHD_DK_IMAGE_CHUNKS_MAX");
        return e && *e ? size_t(strtoul(e, nullptr, 10)) : SIZE_MAX;
    }();
    void* storage = g_chunks.size() < maxChunks ? aligned_alloc(DK_MEMBLOCK_ALIGNMENT, size) : nullptr;
    if (!storage) {
        static int logged = 0;
        if (logged++ < 20)
            LOG("[dk] image heap: no memory for chunk %zu (%u KiB)%s", g_chunks.size(), size >> 10,
                g_chunks.size() >= maxChunks ? " (WWHD_DK_IMAGE_CHUNKS_MAX)" : "");
        return false;
    }
    char what[64];
    snprintf(what, sizeof what, "image heap chunk %zu", g_chunks.size());
    g_chunks.push_back({make_block(size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, what, storage), size, {{0, size}}});
    g_stats.imageChunks = g_chunks.size();
    if (g_trace.on) trace(TraceEvent::NewChunk, uint32_t(g_chunks.size() - 1), size);
    return true;
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
    if (const char* e = getenv("WWHD_DK_IMAGE_TRACE"); e && *e == '1') {
        g_trace.on = true;
        if (FILE* f = fopen(kTracePath, "wb")) {  // a new trace each session: the magic and the record size
            fwrite("WIT1", 1, 4, f);
            const uint32_t recordSize = sizeof(TraceEvent);
            fwrite(&recordSize, 4, 1, f);
            fclose(f);
        }
        LOG("[dk] image heap trace on (WWHD_DK_IMAGE_TRACE): %s, written every 5 s", kTracePath);
    }
    g_queries = make_block(kQuerySize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "queries");
    if (!add_chunk(kImageChunkSize))  // the first image heap chunk now: its size shows in the start-up memory figures
        fatal("[dk] image heap: no memory for its first chunk");
}

void frame_begin(uint64_t frame) {
    g_slot = uint32_t(frame % kFrames);
    Slot& s = g_slots[g_slot];
    if (s.fenced) {
        Stage stage("deko3d: waiting for the GPU (frame fence)");
        const uint64_t waitStart = rprof::enabled() ? rprof::now_ns() : 0;
        struct WaitLap {  // the render thread blocked on the GPU (render_prof.h)
            uint64_t t;
            ~WaitLap() { if (t) rprof::add_wait(rprof::kWaitGpu, rprof::now_ns() - t); }
        } lap{waitStart};
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
        if (a.chunk >= 0) {
            chunk_free(g_chunks[size_t(a.chunk)], a.offset, a.size);
            trace_freed(a.chunk, a.offset, a.size);
        }
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
    if (rprof::enabled()) rprof::add_upload(size + zeroTail);  // in the caller's rprof::UploadKind
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
    const uint64_t key = uint64_t(addr) | uint64_t(alignment) << 32 | uint64_t(zeroTail) << 48;
    const uint64_t stamp = R.frame + 1;  // the frame being recorded
    const UploadEntry* e = g_uploads.find(key, stamp);
    if (e->stamp == stamp && e->gen == R.streamGen && e->size >= size) {
        R.perf.reusedBytes += size;
        return e->slice;
    }
    rprof::guest_read(rprof::Upload(rprof::g_upload_kind), addr, size);  // unique guest bytes (sampled frames)
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
    bool chunkTried = false;
    for (;;) {
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
                trace_placed(int(ci), at, size, alignment);
                return {c.block, at, size, int(ci)};
            }
        }
        // no room: textures nobody draws with any more go first (surfaces.cpp), while that frees something;
        // then a new chunk (bigger than 64 MB only for a bigger image); (this fork) without memory for one, every
        // texture the frames in flight do not use, and at last the image is not made: the GX2 command that needed
        // it is skipped (render::guarded logs it) instead of ending the game
        if (g_trace.on) trace(TraceEvent::Pressure, 0, size, alignment);
        if (image_heap_pressure(size + alignment)) continue;
        if (!chunkTried) {
            chunkTried = true;
            if (add_chunk(std::max(kImageChunkSize, align_up(size + alignment, DK_MEMBLOCK_ALIGNMENT)))) continue;
        }
        if (image_heap_pressure(size + alignment, kFrames + 1)) continue;
        break;
    }
    static int logged = 0;
    if (logged++ < 20) {
        const ImageHeapFigures f = image_heap_figures();
        LOG("[dk] image heap: no room for %u KiB (%zu chunks, %.1f MiB free, largest %.1f MiB)", size >> 10, size_t(f.chunks),
            double(f.free) / 1048576.0, double(f.largestFree) / 1048576.0);
    }
    throw std::runtime_error("image heap full");
}

void image_free_later(const ImageAlloc& a) {
    if (a.chunk < 0) return;
    g_stats.imageBytes -= a.size;
    g_slots[g_slot].retired.push_back(a);
}

void image_free_now(const ImageAlloc& a) {
    if (a.chunk < 0) return;
    g_stats.imageBytes -= a.size;
    chunk_free(g_chunks[size_t(a.chunk)], a.offset, a.size);
    trace_freed(a.chunk, a.offset, a.size);
}

ImageHeapFigures image_heap_figures() {
    ImageHeapFigures f;
    f.chunks = g_chunks.size();
    for (const Chunk& c : g_chunks) {
        f.total += c.size;
        for (const auto& [off, len] : c.free) {
            f.free += len;
            f.largestFree = std::max<uint64_t>(f.largestFree, len);
        }
    }
    return f;
}

void image_trace_flush() {
    if (!g_trace.on || g_trace.events.empty()) return;
    if (FILE* f = fopen(kTracePath, "ab")) {
        fwrite(g_trace.events.data(), sizeof(TraceEvent), g_trace.events.size(), f);
        fclose(f);
    }
    g_trace.events.clear();
}

bool g_shaderCodeLoaded = false;

bool code_load(DkShader& shader, const void* dksh, uint32_t size, const char* name) {
    const uint32_t at = align_up(g_codeUsed, DK_SHADER_CODE_ALIGNMENT);
    if (at + size > kCodeSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
        LOG("[dk] shader code memory full: %s (%u bytes) not loaded", name, size);
        return false;
    }
    uint8_t* const dst = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_code)) + at;
    memcpy(dst, dksh, size);
    g_shaderCodeLoaded = true;
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

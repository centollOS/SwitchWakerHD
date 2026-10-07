// GPU ordering for the deko3d renderer (dk_sync.h): hazard-tracked barriers, batched upload barriers,
// zcull kept across binds of the same depth buffer, the tiled cache. Render thread only.
#include "dk_sync.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dk_surfaces.h"
#include "runtime.h"

namespace gfxdk {

namespace {
bool env_on(const char* name, bool fallback) {
    const char* e = getenv(name);
    return e && *e ? *e != '0' : fallback;
}

uint64_t g_epoch = 1;           // barriers that ordered fragments and invalidated the texture cache, + 1
// the epoch the last Full barrier started (or, without an image invalidate, the one it ran in): 3D work marked
// with an older epoch finished before that barrier, which the copy engine waits for too. Fragments barriers
// order the 3D engine only, so an upload (copy engine) is ordered against them by this, not by g_epoch.
uint64_t g_fullEpoch = 1;
bool g_pendingUploads = false;  // uploads recorded whose barrier after them is deferred (WWHD_DK_UPLOAD_BATCH)

// zcull: the depth buffer (and layer) whose data the hardware holds, as far as the draws know
const Surface* g_zcullDepth = nullptr;
uint32_t g_zcullSlice = 0;
uint64_t g_zcullEpoch = 0;  // R.zcullEpoch when that data was last valid (0: none)

// the draw being prepared: what it samples
Surface* g_sampled[64];
uint32_t g_sampledCount = 0;

struct Counters {
    uint64_t barriers[size_t(SyncWhy::Count)] = {};
    uint64_t tiledFlushes = 0;
    uint64_t zcullDrops = 0, zcullKept = 0;  // binds of a depth buffer: zcull dropped / kept
    uint64_t boundDepthSamples = 0;          // draws that sampled their bound depth buffer without a copy
    uint64_t uploadsDeferred = 0;            // uploads whose barrier after them was left to the next user
    uint64_t uploadsBeforeOlder = 0;         // barriers before uploads whose image was used before the last
                                             // Fragments barrier but after the last Full one (3D vs copy engine)
    uint64_t passChanges = 0;                // target changes (draw.cpp), with or without a barrier
} g_count;

uint32_t g_tileW = 128, g_tileH = 128;  // deko3d's default (gpu_3d_base.cpp TiledCacheTileSize 0x80 x 0x80)

bool hazard_write(const Surface* s) { return s && s->syncRead == g_epoch; }    // written now, read since
bool hazard_read(const Surface* s) { return s && s->syncWrite == g_epoch; }    // read now, written since
}  // namespace

bool sync_lazy_barriers() {
    static const bool on = env_on("WWHD_DK_LAZY_BARRIERS", true);
    return on;
}
bool sync_zcull_keep() {
    static const bool on = env_on("WWHD_DK_ZCULL_KEEP", true);
    return on;
}
bool sync_depth_sample_bound() {
    static const bool on = env_on("WWHD_DK_DEPTH_SAMPLE_BOUND", true);
    return on;
}
bool sync_upload_batch() {
    static const bool on = env_on("WWHD_DK_UPLOAD_BATCH", true);
    return on;
}
bool sync_tiled_cache() {
    static const bool on = [] {
        const bool v = env_on("WWHD_DK_TILED_CACHE", false);
        if (const char* e = getenv("WWHD_DK_TILE_SIZE"); e && *e) {
            // WxH or N (square), powers of two from 16 to 16384 (dkCmdBufSetTileSize)
            unsigned w = 0, h = 0;
            const int n = sscanf(e, "%ux%u", &w, &h);
            if (n == 1) h = w;
            auto pow2 = [](unsigned x) { return x >= 16 && x <= 16384 && !(x & (x - 1)); };
            if (n >= 1 && pow2(w) && pow2(h)) {
                g_tileW = w;
                g_tileH = h;
            } else
                LOG("[dk] GPU sync: WWHD_DK_TILE_SIZE=%s ignored (WxH, powers of two from 16 to 16384)", e);
        }
        return v;
    }();
    return on;
}

void sync_log_switches() {
    LOG("[dk] GPU sync: barriers %s (WWHD_DK_LAZY_BARRIERS=0: a Fragments barrier at every change of render "
        "targets and after every clear, as before)",
        sync_lazy_barriers() ? "where a hazard needs one: a draw sampling what was rendered or cleared since the last "
                               "barrier, or writing what was sampled since it"
                             : "at every change of render targets and after every clear");
    LOG("[dk] GPU sync: zcull %s (WWHD_DK_ZCULL_KEEP=0: dropped at every bind of a depth buffer, as before)",
        sync_zcull_keep() ? "kept while the same depth buffer and layer stay bound (dropped when another one was "
                            "bound, or after copies, uploads or new images of it)"
                          : "dropped at every bind of a depth buffer");
    LOG("[dk] GPU sync: a draw that samples its bound depth buffer without writing depth or stencil %s "
        "(WWHD_DK_DEPTH_SAMPLE_BOUND=0: a feedback copy, as before)",
        sync_depth_sample_bound() ? "reads it directly after a barrier" : "reads a feedback copy");
    LOG("[dk] GPU sync: uploads %s (WWHD_DK_UPLOAD_BATCH=0: full barriers before and after each, as before)",
        sync_upload_batch() ? "in a row share one full barrier, placed before the next draw, clear, copy or present; "
                              "the one before an upload only when the 3D engine used its image since the last full barrier"
                            : "between full barriers each");
    if (sync_tiled_cache())
        LOG("[dk] GPU sync: tiled cache ON (WWHD_DK_TILED_CACHE=1, TEST: not verified on hardware), tiles %ux%u "
            "(WWHD_DK_TILE_SIZE), flushed before every barrier",
            g_tileW, g_tileH);
    else
        LOG("[dk] GPU sync: tiled cache off (deko3d's default; WWHD_DK_TILED_CACHE=1 turns it on, a test)");
}

uint64_t sync_epoch() { return g_epoch; }

void sync_barrier(DkBarrier mode, uint32_t invalidate, SyncWhy why) {
    if (mode != DkBarrier_None && sync_tiled_cache()) {
        // binned primitives finish before the barrier orders them (deko3d flushes the tiled cache itself only
        // at fences and reports)
        dkCmdBufTiledCacheOp(R.cmd, DkTiledCacheOp_Flush);
        g_count.tiledFlushes++;
    }
    dkCmdBufBarrier(R.cmd, mode, invalidate);
    g_count.barriers[size_t(why)]++;
    if (mode >= DkBarrier_Fragments && (invalidate & DkInvalidateFlags_Image)) g_epoch++;
    if (mode == DkBarrier_Full) g_fullEpoch = g_epoch;
    // a Full barrier waits for the copy engine too: it covers the uploads waiting for theirs
    if (mode == DkBarrier_Full && (invalidate & DkInvalidateFlags_Image)) g_pendingUploads = false;
}

void sync_frame_start() {
    // what the previous frame's present pass sampled (the TV picture) is read before this frame's draws write
    // it; the texture, uniform and descriptor caches forget what the CPU rewrote in this slot's stream slice
    // (dkQueueFlush already invalidates them at the start of the next submission; kept as it was)
    sync_barrier(DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors,
                 SyncWhy::FrameStart);
    if (sync_tiled_cache()) {
        static bool sized = false;
        if (!sized && (g_tileW != 128 || g_tileH != 128)) dkCmdBufSetTileSize(R.cmd, g_tileW, g_tileH);
        sized = true;
        dkCmdBufTiledCacheOp(R.cmd, DkTiledCacheOp_Enable);
    }
}

void sync_present() {
    sync_flush_pending();
    // what the frame's passes rendered is complete before the picture is sampled
    sync_barrier(DkBarrier_Fragments, DkInvalidateFlags_Image, SyncWhy::Present);
    // the present pass binds the swapchain's depth buffer: zcull holds that one's now
    g_zcullDepth = nullptr;
    g_zcullEpoch = 0;
}

void sync_flush_pending() {
    if (!g_pendingUploads) return;
    sync_barrier(DkBarrier_Full, DkInvalidateFlags_Image, SyncWhy::UploadsAfter);
}

// ---- draws
void sync_draw_begin() { g_sampledCount = 0; }

void sync_draw_sample(Surface* s) {
    if (s && g_sampledCount < sizeof g_sampled / sizeof g_sampled[0]) g_sampled[g_sampledCount++] = s;
}

void sync_draw_check(const std::array<Surface*, 8>& colors, Surface* depth, bool depthWrites, bool boundDepthSampled) {
    sync_flush_pending();
    const bool lazy = sync_lazy_barriers();
    if (!lazy && !boundDepthSampled) return;  // the old path: barriers at target changes and clears only
    bool raw = false, war = false;
    if (lazy) {
        for (uint32_t i = 0; i < g_sampledCount && !raw; i++) raw = hazard_read(g_sampled[i]);
        for (auto* c : colors) war |= hazard_write(c);
        if (depthWrites) war |= hazard_write(depth);
    } else  // (a bound depth buffer sampled directly: rendered into by this pass's earlier draws)
        raw = hazard_read(depth);
    if (raw) sync_barrier(DkBarrier_Fragments, DkInvalidateFlags_Image, SyncWhy::ReadAfterWrite);
    else if (war) sync_barrier(DkBarrier_Fragments, DkInvalidateFlags_Image, SyncWhy::WriteAfterRead);
}

void sync_draw_mark(const std::array<Surface*, 8>& colors, Surface* depth, bool depthWrites) {
    for (uint32_t i = 0; i < g_sampledCount; i++) g_sampled[i]->syncRead = g_epoch;
    for (auto* c : colors)
        if (c) c->syncWrite = g_epoch;
    if (depth && depthWrites) depth->syncWrite = g_epoch;
    g_sampledCount = 0;
}

void sync_note_bound_depth_sample() { g_count.boundDepthSamples++; }

bool sync_zcull_bind(const Surface* depth, uint32_t slice) {
    g_count.passChanges++;
    if (!depth) return false;  // (zcull keeps the last depth buffer's data; deko3d drops it at an address change)
    const bool same = depth == g_zcullDepth && slice == g_zcullSlice && g_zcullEpoch == R.zcullEpoch;
    g_zcullDepth = depth;
    g_zcullSlice = slice;
    g_zcullEpoch = R.zcullEpoch;
    if (sync_zcull_keep() && same) {
        g_count.zcullKept++;
        return false;
    }
    return true;
}
void sync_zcull_dropped() { g_count.zcullDrops++; }
void sync_zcull_reset(const Surface* depth, uint32_t slice) {
    g_zcullDepth = depth;
    g_zcullSlice = slice;
    g_zcullEpoch = R.zcullEpoch;
    g_count.zcullDrops++;
}

// ---- clears
void sync_clear_begin(Surface* s) {
    sync_flush_pending();
    if (sync_lazy_barriers() && hazard_write(s))
        sync_barrier(DkBarrier_Fragments, DkInvalidateFlags_Image, SyncWhy::WriteAfterRead);
}

void sync_clear_end(Surface* s, bool depthCleared, uint32_t lastSlice) {
    // the old path: what a clear wrote is visible to the commands after it (sampling, copies). Hazard
    // tracking leaves that to the draw that samples it; copies have their own barrier before them.
    if (!sync_lazy_barriers()) sync_barrier(DkBarrier_Fragments, DkInvalidateFlags_Image, SyncWhy::ClearAfter);
    s->syncWrite = g_epoch;
    if (s->fmt.depth) {
        // the clear bound s as the depth target (deko3d dropped zcull if another address was bound) and a
        // depth clear sets its zcull data; a stencil-only clear leaves it unknown
        g_zcullDepth = depthCleared ? s : nullptr;
        g_zcullSlice = lastSlice;
        g_zcullEpoch = depthCleared ? R.zcullEpoch : 0;
    }
}

// ---- transfers
void sync_transfer_begin(bool workSince) {
    if (g_pendingUploads) {  // ordered after the uploads, and after everything before them
        sync_flush_pending();
        return;
    }
    if (!workSince) return;
    // the old path's barrier had no invalidate; with it the barrier also starts an epoch (hazard tracking)
    sync_barrier(DkBarrier_Full, sync_lazy_barriers() || sync_upload_batch() ? DkInvalidateFlags_Image : 0u,
                 SyncWhy::TransferBefore);
}

void sync_transfer_end() { sync_barrier(DkBarrier_Full, DkInvalidateFlags_Image, SyncWhy::TransferAfter); }

void sync_upload_begin(Surface* s, bool workSince) {
    if (!sync_upload_batch()) {
        sync_transfer_begin(workSince);
        return;
    }
    // the copy engine overwrites the image: 3D work that used it since the last Full barrier must finish first
    // (Fragments barriers do not hold the copy engine back: only a Full one makes the host wait for both).
    // Uploads write images only from staging memory, so uploads in a row need nothing between them.
    const uint64_t used = s->syncRead > s->syncWrite ? s->syncRead : s->syncWrite;
    if (used >= g_fullEpoch) {
        if (used != g_epoch) g_count.uploadsBeforeOlder++;
        sync_barrier(DkBarrier_Full, DkInvalidateFlags_Image, SyncWhy::TransferBefore);
    }
}

void sync_upload_end() {
    if (!sync_upload_batch()) {
        sync_transfer_end();
        return;
    }
    if (g_pendingUploads) g_count.uploadsDeferred++;  // (one barrier fewer than before)
    g_pendingUploads = true;
}

std::string sync_report(uint64_t frames) {
    if (!frames) return "";
    const auto& b = g_count.barriers;
    auto pf = [&](uint64_t n) { return double(n) / double(frames); };
    auto at = [&](SyncWhy w) { return pf(b[size_t(w)]); };
    const double fragments = at(SyncWhy::FrameStart) + at(SyncWhy::Present) + at(SyncWhy::PassChange) +
                             at(SyncWhy::ReadAfterWrite) + at(SyncWhy::WriteAfterRead) + at(SyncWhy::ClearAfter);
    const double full = at(SyncWhy::TransferBefore) + at(SyncWhy::TransferAfter) + at(SyncWhy::UploadsAfter);
    char out[800];
    snprintf(out, sizeof out,
             "per frame: %.1f Fragments barriers (pass changes %.1f of %.1f, read after write %.1f, write after read "
             "%.1f, after clears %.1f, frame start %.1f, present %.1f), %.1f Full barriers (before transfers %.1f, after "
             "transfers %.1f, after upload batches %.1f; %.1f uploads joined a batch; %.1f before uploads of images used "
             "before the last Fragments barrier, ordered for the copy engine); zcull dropped %.1f, kept %.1f "
             "binds; bound depth sampled without a copy %.1f; tiled cache flushes %.1f",
             fragments, at(SyncWhy::PassChange), pf(g_count.passChanges), at(SyncWhy::ReadAfterWrite),
             at(SyncWhy::WriteAfterRead), at(SyncWhy::ClearAfter), at(SyncWhy::FrameStart), at(SyncWhy::Present), full,
             at(SyncWhy::TransferBefore), at(SyncWhy::TransferAfter), at(SyncWhy::UploadsAfter),
             pf(g_count.uploadsDeferred), pf(g_count.uploadsBeforeOlder), pf(g_count.zcullDrops), pf(g_count.zcullKept),
             pf(g_count.boundDepthSamples), pf(g_count.tiledFlushes));
    g_count = Counters{};
    return out;
}

}  // namespace gfxdk

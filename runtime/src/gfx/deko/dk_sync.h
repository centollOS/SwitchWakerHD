// GPU ordering for the deko3d renderer (P4, GPU side; gpu_sync.cpp): the barriers between passes, clears,
// uploads and copies, zcull invalidation, sampling a bound depth buffer, and deko3d's tiled cache.
//
// What deko3d 0.5.0 records for each (source/maxwell/gpu_base.cpp, gpu_3d_state.cpp):
//   DkBarrier_Fragments  FragmentBarrier (+ a texture-cache invalidate with WFI): every earlier fragment is
//                        done before later ones start; the pipeline drains between the two passes
//   DkBarrier_Full       a gpfifo SetReference plus a new gpfifo entry with NoPrefetch: the host waits until
//                        every engine (3D, 2D, copy) is idle and stops prefetching commands
//   DkInvalidateFlags_Zcull  InvalidateZcull: the bound depth buffer's zcull data is thrown away (it is rebuilt
//                        only by rendering after it, or a clear)
//   dkQueueFlush         also invalidates the image, shader, descriptor and L2 caches at the start of the next
//                        submission (Queue::postSubmitFlush), so the frame start needs no invalidation of its own
//
// Hazard tracking (WWHD_DK_LAZY_BARRIERS, on unless =0): instead of a Fragments barrier at every change of
// render targets and after every clear, a barrier goes in only where the hardware needs one:
//   read after write   a draw samples a surface that a draw or clear wrote since the last barrier
//   write after read   a draw or clear writes a surface that a draw sampled since the last barrier
// Writes after writes need none (the ROP keeps API order per pixel whatever the bound targets are), nor do
// reads after reads. Each Surface carries the barrier epoch of its last GPU write and read (syncWrite,
// syncRead); every barrier that orders fragments and invalidates the texture cache starts a new epoch.
// Transfers (copy and 2D engines) keep their own barriers (surfaces.cpp transfer_begin / transfer_end).
#pragma once
#include <deko3d.h>

#include <array>
#include <cstdint>
#include <string>

#include "dk.h"

namespace gfxdk {
struct Surface;

// ---- switches (read at the first use, each announced by a '[dk] GPU sync' line at startup)
bool sync_lazy_barriers();        // WWHD_DK_LAZY_BARRIERS: hazard-tracked barriers (=0: one per pass and clear)
bool sync_zcull_keep();           // WWHD_DK_ZCULL_KEEP: zcull kept while the same depth buffer stays bound
bool sync_depth_sample_bound();   // WWHD_DK_DEPTH_SAMPLE_BOUND: a draw that does not write depth samples the
                                  // bound depth buffer directly (=0: through a feedback copy)
bool sync_upload_batch();         // WWHD_DK_UPLOAD_BATCH: uploads in a row share one barrier after them
bool sync_tiled_cache();          // WWHD_DK_TILED_CACHE=1: deko3d's tiled cache (off by default: untested)
void sync_log_switches();         // the startup lines (draw_frame_start)

// a barrier: counted by reason; one with Fragments or stronger and the image invalidate starts an epoch
enum class SyncWhy : uint8_t {
    FrameStart, Present, PassChange, ReadAfterWrite, WriteAfterRead, ClearAfter, TransferBefore, TransferAfter,
    UploadsAfter, Count
};
void sync_barrier(DkBarrier mode, uint32_t invalidate, SyncWhy why);
uint64_t sync_epoch();

// ---- frame (begin_commands): the frame-start barrier and the tiled cache's state
void sync_frame_start();
// present: what is pending (uploads) and the barrier before the present pass samples the frame's picture;
// zcull's depth becomes the swapchain's
void sync_present();

// ---- draws (draw.cpp). Per draw: sync_draw_sample for each sampled surface (after any feedback copy), then
// sync_draw_check before the render targets are bound (the barrier the hazards need, and pending uploads'),
// then sync_draw_mark after them (the reads and writes of this draw, in the epoch they run in)
void sync_draw_begin();
void sync_draw_sample(Surface* s);
void sync_draw_check(const std::array<Surface*, 8>& colors, Surface* depth, bool depthWrites, bool boundDepthSampled);
void sync_draw_mark(const std::array<Surface*, 8>& colors, Surface* depth, bool depthWrites);
// a draw sampled its bound depth buffer directly (no feedback copy): counted
void sync_note_bound_depth_sample();

// zcull at a bind of render targets with this depth buffer (draw.cpp): true when its data must be dropped
// (another depth buffer or layer was bound last, or its contents changed outside the 3D engine: R.zcullEpoch)
bool sync_zcull_bind(const Surface* depth, uint32_t slice);
void sync_zcull_dropped();  // the caller dropped it (counted)
// the caller dropped the bound depth buffer's zcull data without a change of targets (R.zcullEpoch moved)
void sync_zcull_reset(const Surface* depth, uint32_t slice);

// ---- clears (surfaces.cpp): before (a write after a read needs a barrier) and after (the old path's barrier)
void sync_clear_begin(Surface* s);
void sync_clear_end(Surface* s, bool depthCleared, uint32_t lastSlice);

// ---- transfers (surfaces.cpp). workSince: draws or clears were recorded since the last transfer
void sync_transfer_begin(bool workSince);
void sync_transfer_end();
void sync_upload_begin(Surface* s, bool workSince);
void sync_upload_end();
void sync_flush_pending();  // the barrier deferred uploads still need, now

// the 5 s line: barriers and zcull per frame
std::string sync_report(uint64_t frames);

}  // namespace gfxdk

// Captures of the deko3d renderer (P3, capture.cpp): a frame's pictures as PNG files, as gfx/gl's capture
// tooling (gfx/gl/backend.cpp frame_dumps, gfx/gl/dump.cpp), for pictures that go wrong on the console.
//
// What a captured frame writes to sdmc:/switch/wwhd/captures/<frame>/ (the working directory's captures/):
//   frame_<n>.png              the TV picture the present pass sampled, as the display shows it (sRGB-encoded
//                              when the TV format is sRGB, as gfx/gl's frame_<n>.png)
//   frame_<n>_window.png       the swapchain image presented (bars, FPS counter and overlay included)
//   frame_<n>_tv_source.png    the buffer the game copied to the TV scan buffer, when the present pass read
//                              its copy instead
//   target_<addr>_<w>x<h>_f<gx2 format>[_s<layer>][_depth].png   every render target of the frame's draws
//   tex_<addr>_<w>x<h>_f<gx2 format>[_<dim>].png                a texture the frame's draws sampled, as
//                              the GPU holds it (level 0, layer 0). Textures of CPU data are compared byte for
//                              byte with their upload data after the readback and written (with their
//                              _upload.png) only when they differ; WWHD_DK_CAPTURE_ALL_TEXTURES=1 writes all.
//                              GPU-written textures are always written.
//   tex_..._upload.png         the same texture's guest data as the CPU decodes it for its upload (detiled
//                              and converted, before the GPU): a wrong picture in both is a wrong format or
//                              decode; right here and wrong in the GPU's is a wrong upload, view or sampling
// Each file gets one '[dk] capture' line: GX2 and deko3d formats, size, mips/layers, tile mode, and the draws
// (numbered as the frame's '[trace]   draw #n' lines) that used it, with each use's unit, view swizzle,
// texture-word format and sampler. GPU images are copied (dkCmdBufCopyImageToBuffer: the copy engine
// de-swizzles the block-linear layout) into a CPU-cached memory block after the frame is presented, in
// batches of up to 16 MiB; a worker thread converts to RGBA8 and writes the PNGs (zlib level 1). The render
// thread never waits for the writer: past 128 MiB queued a file is dropped with a log line. The end:
// '[dk] capture of frame N done in X s: ...'.
//
// Triggers: both sticks clicked (request_capture: everything, plus the frame's passes and draws in the log),
// WWHD_DUMP_FRAMES=n,... (the pictures), WWHD_DUMP_TARGETS=n,... (the render targets), WWHD_DUMP_TEXTURES=n,...
// (the sampled textures and their upload data). Render thread only, except the worker.
#pragma once
#include <deko3d.h>

#include <array>
#include <cstdint>
#include <vector>

#include "dk.h"

namespace gfxdk {
struct Surface;
struct PresentSource;

enum CaptureWhat : uint32_t {
    kCapturePictures = 1,  // frame_<n>.png, frame_<n>_window.png, frame_<n>_tv_source.png
    kCaptureTargets = 2,   // target_*.png
    kCaptureTextures = 4,  // tex_*.png and tex_*_upload.png
    kCaptureAll = 7,
};
// what the frame being recorded captures (0: nothing): set by capture_arm, read by the draw path's notes
extern uint32_t g_capture;

// backend.cpp swap(): the next frame (R.frame + 1) captures `what` (0: nothing); why: for the log
void capture_arm(uint32_t what, const char* why);
// draw.cpp, while g_capture: a draw's textures are noted as they are resolved (before feedback copies;
// feedback: the draw samples a snapshot of it), then given the draw's number when the draw is executed
void capture_draw_begin();  // a new draw is being prepared: textures noted for a skipped one are dropped
void capture_note_texture(Surface* s, bool vertex, uint32_t unit, const uint32_t* texWords,
                          const uint32_t* samplerWords, bool compare, bool feedback);
void capture_note_draw(uint32_t index, const std::array<Surface*, 8>& colors, const uint32_t* slices, Surface* depth,
                       uint32_t depthSlice);
// backend.cpp present(), after dkQueuePresentImage of a captured frame: waits for the GPU, copies the
// images back and queues the PNGs (window: the swapchain image presented, ww x wh RGBA8)
void capture_present(const DkImage& window, uint32_t ww, uint32_t wh, const PresentSource& src);

// surfaces.cpp: level 0, layer 0 of s's guest data as upload_surface stages it (detiled and converted to
// s->fmt.image's layout, rows of blocks without padding); changed: the guest data no longer hashes as it
// did at the last upload (the GPU holds older data). False for a GPU-written surface or one without image.
bool capture_upload_data(Surface* s, std::vector<uint8_t>& out, bool& changed);

}  // namespace gfxdk

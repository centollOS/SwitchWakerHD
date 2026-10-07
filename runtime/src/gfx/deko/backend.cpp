// The deko3d renderer's device, presentation and host loop, and its entry in the renderer table (dk.h).
// Every GX2 swap presents the game's TV picture (present_source, dk_surfaces.h) with the picture adjustments,
// the FPS counter and the settings overlay (hold Minus); black until the game has copied a picture to the TV
// scan buffer. WWHD_DK_TEST_PATTERN=1 shows P1's test pattern of the device's conventions (origin, y direction,
// depth range, depth test) instead of the picture. Also here: submits, the statistics, hitch log and trace.
#include <malloc.h>
#include <unistd.h>
extern "C" char* fake_heap_end;  // libnx: the end of the heap malloc grows into (sbrk)
#include <switch.h>

#include "dk.h"
#include "dk_capture.h"
#include "dk_draw.h"
#include "dk_shaders.h"
#include "dk_surfaces.h"
#include "dk_sync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio_out.h"
#include "gfx/renderer.h"
#include "gfx/switch_renderer.h"
#include "gx2/gx2.h"
#include "imgui.h"
#include "input.h"
#include "overlay/hostui.h"
#include "overlay/overlay.h"
#include "platform/host.h"
#include "platform/input_switch.h"
#include "platform/settings_switch.h"
#include "runtime.h"

// the renderer's own shaders, compiled by uam at build time (CMakeLists.txt, gfx/deko/shaders)
#include "imgui_fsh_dksh.h"
#include "imgui_vsh_dksh.h"
#include "pattern_fsh_dksh.h"
#include "pattern_vsh_dksh.h"
#include "present_fsh_dksh.h"
#include "present_vsh_dksh.h"
#include "text_fsh_dksh.h"
#include "text_vsh_dksh.h"

namespace gfxdk {
Renderer R;

namespace {
constexpr uint32_t kSwapImages = 3;
// the window (swapchain images): 1280x720 handheld, 1920x1080 docked (window_wanted below); the game's picture is
// scaled into it by the present pass. Render thread only once the device exists.
uint32_t g_winW = 1280, g_winH = 720;
uint64_t g_windowResizes = 0;  // swapchains recreated for a new window size (the stats line)
// the console's operation mode as the host loop last read it (run_main_loop: appletGetOperationMode, which libnx
// updates on AppletMessage_OperationModeChanged): 1 docked (TV), 0 handheld, -1 not read yet
std::atomic<int> g_opMode{-1};

// WWHD_DK_DOCKED_1080 (on unless =0): docked, the window is 1920x1080 (the TV's output); handheld 1280x720 (the
// screen). =0: 1280x720 always (the old path; the system scales it to the TV). WWHD_DK_WINDOW=WxH (at most
// 1920x1080) forces that size in both modes, for A/B tests in handheld. The game still renders its 1280x720
// picture (internal and dynamic resolution as before): the present pass scales it into the window.
struct WindowConfig {
    int mode = 1;  // 0 always 1280x720, 1 by the operation mode, 2 forced
    uint32_t w = 0, h = 0;
};
const WindowConfig& window_config() {
    static const WindowConfig c = [] {
        WindowConfig c;
        if (const char* e = getenv("WWHD_DK_WINDOW"); e && *e) {
            unsigned w = 0, h = 0;
            if (sscanf(e, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 1920 && h <= 1080) {
                c.mode = 2;
                c.w = w;
                c.h = h;
            } else
                LOG("[dk] WWHD_DK_WINDOW=%s ignored: WxH from 320x180 to 1920x1080", e);
        }
        if (c.mode != 2)
            if (const char* e = getenv("WWHD_DK_DOCKED_1080"); e && *e == '0') c.mode = 0;
        if (c.mode == 2)
            LOG("[dk] window: %ux%u in both modes (WWHD_DK_WINDOW); the game's 1280x720 picture is scaled into it", c.w, c.h);
        else if (c.mode == 1)
            LOG("[dk] window: docked 1920x1080, handheld 1280x720, the swapchain recreated when the mode changes; the game's "
                "1280x720 picture is scaled into it (WWHD_DK_DOCKED_1080=0: 1280x720 always, the old path)");
        else
            LOG("[dk] window: 1280x720 in both modes (WWHD_DK_DOCKED_1080=0, the old path; docked the system scales it)");
        return c;
    }();
    return c;
}
void window_wanted(uint32_t& w, uint32_t& h) {
    const WindowConfig& c = window_config();
    if (c.mode == 2) {
        w = c.w;
        h = c.h;
        return;
    }
    const bool big = c.mode == 1 && g_opMode.load(std::memory_order_relaxed) == 1;
    w = big ? 1920 : 1280;
    h = big ? 1080 : 720;
}
// the host loop: the operation mode, logged when it changes (appletMainLoop handled the applet message)
void poll_operation_mode() {
    const int docked = appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0;
    const int before = g_opMode.exchange(docked, std::memory_order_relaxed);
    if (before == docked) return;
    uint32_t w, h;
    window_wanted(w, h);
    if (before < 0)
        LOG("[dk] operation mode: %s; window %ux%u", docked ? "docked (TV)" : "handheld", w, h);
    else
        LOG("[dk] operation mode changed (AppletMessage_OperationModeChanged): now %s; window %ux%u from the next present",
            docked ? "docked (TV)" : "handheld", w, h);
}
#ifdef WWHD_DEKO3D_DEBUG_LIB
constexpr bool kDebugLib = true;  // linked against libdeko3dd (CMakeLists.txt)
#else
constexpr bool kDebugLib = false;
#endif

DkSwapchain g_swapchain = nullptr;
DkImage g_swapImages[kSwapImages];
DkImage g_depth;
DkShader g_shaders[kShaderCount];
bool g_shaderOk[kShaderCount] = {};
DkShader g_presentShaders[2];  // the present pass of the game's picture (vertex, fragment)
bool g_presentOk = false;
std::atomic<int> g_debugMessages{0};

// deko3d's messages. Only the debug library (libdeko3dd, WWHD_DEKO3D_DEBUG_LIB) calls this: its checks of
// every call, warnings (result DkResult_Success) and errors, after which it traps. The release library calls
// nothing on an error: it aborts with result 2359-xxxx (diagAbortWithResult), the log writer thread's last
// lines lost; hence the log_flush() before each creation and the queue checks in present().
void debug_message(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success)  // a trap follows: the error ends the process here, readably
        fatal("[dk] deko3d error in %s: result %d: %s (frame %llu)", context ? context : "?", int(result),
              message ? message : "", (unsigned long long)R.frame + 1);
    const int n = g_debugMessages.fetch_add(1);
    if (n < 200) LOG("[dk] deko3d %s: %s", context ? context : "?", message ? message : "");
    else if (n == 200) LOG("[dk] deko3d: more messages, not logged");
}

// deko3d aborts on any call that touches a queue in an error state (a GPU fault: submit, acquire, present,
// flush): checked before each, the error ends the process with the frame and the counts in the log
void check_queue(const char* before, uint64_t frame) {
    if (!dkQueueIsInErrorState(R.queue)) return;
    const Renderer::Counts& c = R.counts;
    fatal("[dk] the deko3d queue is in an error state (a GPU fault) before %s of frame %llu; deko3d messages %d; "
          "GX2 so far: %llu draws (%llu executed), %llu clears, %llu surface copies, %llu scan copies",
          before, (unsigned long long)frame, g_debugMessages.load(), (unsigned long long)c.draws,
          (unsigned long long)R.drawCount, (unsigned long long)c.clears, (unsigned long long)c.copies,
          (unsigned long long)c.scans);
}

void load_shaders() {
    static const struct {
        const uint8_t* data;
        size_t size;
        const char* name;
    } kEmbedded[kShaderCount] = {
        {pattern_vsh_dksh, pattern_vsh_dksh_size, "pattern_vsh"}, {pattern_fsh_dksh, pattern_fsh_dksh_size, "pattern_fsh"},
        {text_vsh_dksh, text_vsh_dksh_size, "text_vsh"},          {text_fsh_dksh, text_fsh_dksh_size, "text_fsh"},
        {imgui_vsh_dksh, imgui_vsh_dksh_size, "imgui_vsh"},       {imgui_fsh_dksh, imgui_fsh_dksh_size, "imgui_fsh"},
    };
    size_t bytes = 0;
    int ok = 0;
    for (int i = 0; i < kShaderCount; i++) {
        g_shaderOk[i] = code_load(g_shaders[i], kEmbedded[i].data, uint32_t(kEmbedded[i].size), kEmbedded[i].name);
        ok += g_shaderOk[i];
        bytes += kEmbedded[i].size;
    }
    g_presentOk = code_load(g_presentShaders[0], present_vsh_dksh, uint32_t(present_vsh_dksh_size), "present_vsh") &&
                  code_load(g_presentShaders[1], present_fsh_dksh, uint32_t(present_fsh_dksh_size), "present_fsh");
    bytes += present_vsh_dksh_size + present_fsh_dksh_size;
    LOG("[dk] embedded shaders: %d of %d loaded, present pass %s (%zu bytes of DKSH)", ok, int(kShaderCount),
        g_presentOk ? "loaded" : "NOT loaded: the game's picture cannot be shown", bytes);
}

ImageAlloc g_swapMem[kSwapImages], g_depthMem;

void init_image(DkImage& image, ImageAlloc& mem, DkImageFormat format, uint32_t flags, const char* what) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.flags = flags;
    m.format = format;
    m.dimensions[0] = g_winW;
    m.dimensions[1] = g_winH;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    mem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&image, &layout, mem.block, mem.offset);
    LOG("[dk] %s: %ux%u, %u KiB at image heap offset 0x%X", what, g_winW, g_winH, mem.size >> 10, mem.offset);
}

void init_swapchain() {
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        char what[48];
        snprintf(what, sizeof what, "swapchain image %u (RGBA8)", i);
        init_image(g_swapImages[i], g_swapMem[i], DkImageFormat_RGBA8_Unorm,
                   DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression, what);
        images[i] = &g_swapImages[i];
    }
    init_image(g_depth, g_depthMem, DkImageFormat_Z24S8, DkImageFlags_UsageRender | DkImageFlags_HwCompression,
               "test pattern depth buffer (Z24S8)");
    DkSwapchainMaker m;
    dkSwapchainMakerDefaults(&m, R.device, nwindowGetDefault(), images, kSwapImages);
    LOG("[dk] creating the swapchain on the default window");
    log_flush();  // a failed creation aborts (debug_message)
    g_swapchain = dkSwapchainCreate(&m);
    dkSwapchainSetSwapInterval(g_swapchain, 1);
    u32 nw = 0, nh = 0;
    nwindowGetDimensions(nwindowGetDefault(), &nw, &nh);
    LOG("[dk] swapchain: %u RGBA8 images of %ux%u on the default window (%ux%u), swap interval 1", kSwapImages, g_winW,
        g_winH, nw, nh);
}

// a new window size (the operation mode changed): the GPU finishes what was submitted (the previous present
// passes into the old images), the old swapchain releases the window's buffers (nwindowReleaseBuffers), and new
// images and a new swapchain of the new size take their place (deko3d sets the window's dimensions from them,
// nwindowSetDimensions). The old images' memory is freed once the GPU is done with this frame. Same swap
// interval (1); relaxed vsync is gx2_core's own pacing (steady clock, 59.94 Hz), the same in both modes.
void resize_window(uint32_t w, uint32_t h, uint64_t frame) {
    const uint64_t t0 = now_ns();
    const uint32_t ow = g_winW, oh = g_winH;
    check_queue("recreating the swapchain", frame);
    dkQueueWaitIdle(R.queue);
    const uint64_t t1 = now_ns();
    dkSwapchainDestroy(g_swapchain);
    g_swapchain = nullptr;
    for (uint32_t i = 0; i < kSwapImages; i++) image_free_later(g_swapMem[i]);
    image_free_later(g_depthMem);
    g_winW = w;
    g_winH = h;
    init_swapchain();
    g_windowResizes++;
    LOG("[dk] frame %llu: window %ux%u -> %ux%u: swapchain recreated in %.1f ms (GPU idle wait %.1f ms), swap interval 1",
        (unsigned long long)frame, ow, oh, w, h, double(now_ns() - t0) / 1e6, double(t1 - t0) / 1e6);
}

// ---- state for the renderer's own passes
void bind_pass_state(bool blend, bool depthTest) {
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(R.cmd, &rs);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, blend);
    dkCmdBufBindColorState(R.cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkBlendStateSetFactors(&bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One,
                           DkBlendFactor_InvSrcAlpha);
    dkCmdBufBindBlendStates(R.cmd, 0, &bs, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = depthTest;
    ds.depthWriteEnable = depthTest;
    ds.depthCompareOp = DkCompareOp_Less;
    dkCmdBufBindDepthStencilState(R.cmd, &ds);
}

void set_view(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const DkViewport vp = {float(x), float(y), float(w), float(h), 0.0f, 1.0f};
    const DkScissor sc = {x, y, w, h};
    dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
    dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
}

bool bind_shaders(ShaderId vs, ShaderId fs) {
    if (!g_shaderOk[vs] || !g_shaderOk[fs]) return false;
    const DkShader* sh[] = {&g_shaders[vs], &g_shaders[fs]};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_GraphicsMask, sh, 2);
    return true;
}

// ---- text: the FPS counter's 3x5 pixel font (as the OpenGL renderer's), one draw per text box
uint32_t glyph_bits(char c) {
    static const struct { char c; const char* rows; } kFont[] = {
        {'0', "111101101101111"}, {'1', "010110010010111"}, {'2', "111001111100111"}, {'3', "111001111001111"},
        {'4', "101101111001001"}, {'5', "111100111001111"}, {'6', "111100111101111"}, {'7', "111001001001001"},
        {'8', "111101111101111"}, {'9', "111101111001111"}, {'A', "010101111101101"}, {'B', "110101110101110"},
        {'C', "011100100100011"}, {'D', "110101101101110"}, {'E', "111100110100111"}, {'F', "111100110100100"},
        {'G', "011100101101011"}, {'H', "101101111101101"}, {'I', "111010010010111"}, {'J', "001001001101010"},
        {'K', "101101110101101"}, {'L', "100100100100111"}, {'M', "101111111101101"}, {'N', "110101101101101"},
        {'O', "010101101101010"}, {'P', "110101110100100"}, {'Q', "010101101110011"}, {'R', "110101110101101"},
        {'S', "011100010001110"}, {'T', "111010010010010"}, {'U', "101101101101111"}, {'V', "101101101101010"},
        {'W', "101101111111101"}, {'X', "101101010101101"}, {'Y', "101101010010010"}, {'Z', "111001010100111"},
        {'%', "101001010100101"}, {':', "000010000010000"}, {'/', "001001010100100"}, {'-', "000000111000000"},
        {'.', "000000000000010"}, {',', "000000000010100"}, {'=', "000111000111000"}, {'(', "010100100100010"},
        {')', "010001001001010"}, {'>', "100010001010100"}, {'<', "001010100010001"}, {'+', "000010111010000"},
    };
    for (auto& g : kFont)
        if (g.c == c) {
            uint32_t bits = 0;
            for (int i = 0; i < 15; i++) bits = bits << 1 | uint32_t(g.rows[i] == '1');
            return bits;
        }
    return 0;  // space and anything else
}

struct TextUbo {  // text_fsh.glsl, std140
    float box[4];
    int32_t grid[4];
    float fg[4], bg[4];
    uint32_t glyphs[768];  // uvec4 glyphs[192]: character i in component i & 3 of entry i >> 2
};
static_assert(offsetof(TextUbo, box) == 0 && offsetof(TextUbo, grid) == 16 && offsetof(TextUbo, fg) == 32 &&
                  offsetof(TextUbo, bg) == 48 && offsetof(TextUbo, glyphs) == 64 && sizeof(TextUbo) == 64 + 768 * 4,
              "TextUbo must match text_fsh.glsl's std140 block");

// start-up self-test of the FPS counter's data: the glyph bits of the characters it shows, the TextUbo
// offsets next to the std140 offsets text_fsh.glsl reads, and the shader's lookup replayed on the CPU for
// a sample line (the characters read back from the packed words, as text_fsh selects them)
void text_self_test() {
    std::string bits;
    for (const char* c = "0123456789FPS."; *c; c++) {
        char b[16];
        snprintf(b, sizeof b, " %c=%04X", *c, glyph_bits(*c));
        bits += b;
    }
    LOG("[dk] FPS counter self-test: glyph bits (3x5, row 0 in bits 14-12)%s", bits.c_str());
    const size_t off[5] = {offsetof(TextUbo, box), offsetof(TextUbo, grid), offsetof(TextUbo, fg), offsetof(TextUbo, bg),
                           offsetof(TextUbo, glyphs)};
    static const size_t kStd140[5] = {0, 16, 32, 48, 64};
    const bool layoutOk = !memcmp(off, kStd140, sizeof off);
    LOG("[dk] FPS counter self-test: TextUbo offsets box %zu grid %zu fg %zu bg %zu glyphs %zu, size %zu (text_fsh.glsl "
        "std140: 0 16 32 48 64, size 3136): %s", off[0], off[1], off[2], off[3], off[4], sizeof(TextUbo),
        layoutOk ? "match" : "MISMATCH");
    // the shader's lookup: entry i >> 2, component i & 3, bit (4 - row) * 3 + (2 - column)
    const std::string sample = "30.0 FPS";
    TextUbo t{};
    for (size_t i = 0; i < sample.size(); i++) t.glyphs[i] = glyph_bits(sample[i]);
    std::string back;
    for (int i = 0; i < int(sample.size()); i++) {
        const uint32_t* q = &t.glyphs[(i >> 2) * 4];
        const uint32_t g = q[i & 3];
        char found = '?';
        for (const char* c = " 0123456789FPS."; *c; c++)
            if (glyph_bits(*c) == g) {
                found = *c;
                break;
            }
        back += found;
    }
    LOG("[dk] FPS counter self-test: \"%s\" packed and read back as \"%s\": %s", sample.c_str(), back.c_str(),
        back == sample ? "ok" : "MISMATCH");
}

// lines at (left, top) in window pixels, `scale` window pixels per font pixel
void draw_text(int left, int top, int scale, const std::vector<std::string>& lines, const float fg[4], const float bg[4]) {
    if (lines.empty() || !bind_shaders(kTextVs, kTextFs)) return;
    int columns = 0;
    for (auto& l : lines) columns = std::max(columns, int(l.size()));
    const int rows = std::min(int(lines.size()), 768 / std::max(columns, 1));
    constexpr uint32_t kUboSize = (sizeof(TextUbo) + 255) & ~255u;
    StreamAlloc u = stream_alloc(kUboSize, DK_UNIFORM_BUF_ALIGNMENT);
    if (!u) return;
    TextUbo t{};
    t.box[0] = float(left);
    t.box[1] = float(top);
    t.box[2] = float(scale);
    t.grid[0] = columns;
    t.grid[1] = rows;
    memcpy(t.fg, fg, sizeof t.fg);
    memcpy(t.bg, bg, sizeof t.bg);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < int(lines[size_t(r)].size()); c++) t.glyphs[r * columns + c] = glyph_bits(lines[size_t(r)][size_t(c)]);
    memcpy(u.cpu, &t, sizeof t);
    const int w = (columns * 4 + 1) * scale, h = (rows * 6 + 1) * scale;
    bind_pass_state(true, false);
    set_view(uint32_t(left), uint32_t(top), uint32_t(std::min<int>(w, int(g_winW) - left)),
             uint32_t(std::min<int>(h, int(g_winH) - top)));
    const DkBufExtents ubo = {u.gpu, kUboSize};
    dkCmdBufBindUniformBuffers(R.cmd, DkStage_Fragment, 0, &ubo, 1);
    dkCmdBufBindVtxAttribState(R.cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(R.cmd, nullptr, 0);
    dkCmdBufDraw(R.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

// ---- the orientation / depth test pattern. Positions in normalized device coordinates with y down (y = -1
// is the top of the screen; pattern_vsh negates y, dk.h) and clip-space z from 0 to 1.
struct PatternVertex {
    float x, y, z;
    float r, g, b, a;
};
void quad(std::vector<PatternVertex>& v, float x0, float y0, float x1, float y1, float z, const float top[4],
          const float bottom[4], const float* right = nullptr) {
    // corners: top = y0 side, bottom = y1 side; `right`: a horizontal gradient from top (left) to right
    const float* tl = top;
    const float* tr = right ? right : top;
    const float* bl = right ? top : bottom;
    const float* br = right ? right : bottom;
    auto p = [&](float x, float y, const float* c) { v.push_back({x, y, z, c[0], c[1], c[2], c[3]}); };
    p(x0, y0, tl), p(x1, y0, tr), p(x0, y1, bl);
    p(x1, y0, tr), p(x1, y1, br), p(x0, y1, bl);
}
void triangle(std::vector<PatternVertex>& v, float x0, float y0, float x1, float y1, float x2, float y2, float z,
              const float c[4]) {
    v.push_back({x0, y0, z, c[0], c[1], c[2], c[3]});
    v.push_back({x1, y1, z, c[0], c[1], c[2], c[3]});
    v.push_back({x2, y2, z, c[0], c[1], c[2], c[3]});
}

void draw_pattern(uint64_t frame) {
    static const float kSkyTop[4] = {0.10f, 0.22f, 0.50f, 1}, kSkyBottom[4] = {0.02f, 0.02f, 0.04f, 1};
    static const float kBlack[4] = {0, 0, 0, 1}, kWhite[4] = {1, 1, 1, 1}, kRed[4] = {0.9f, 0.1f, 0.1f, 1},
                       kGreen[4] = {0.1f, 0.8f, 0.2f, 1}, kCyan[4] = {0.1f, 0.85f, 0.9f, 1},
                       kMagenta[4] = {0.85f, 0.15f, 0.8f, 1}, kOrange[4] = {1.0f, 0.55f, 0.0f, 1},
                       kGray[4] = {0.3f, 0.3f, 0.3f, 1};
    std::vector<PatternVertex> v;
    v.reserve(64);
    quad(v, -1, -1, 1, 1, 0.99f, kSkyTop, kSkyBottom);           // background: blue at the top, dark at the bottom
    quad(v, -0.6f, 0.30f, 0.6f, 0.42f, 0.5f, kBlack, kBlack, kWhite);  // black (left) to white (right)
    quad(v, -0.97f, -0.80f, -0.72f, -0.45f, 0.5f, kRed, kRed);    // top left
    quad(v, 0.72f, 0.55f, 0.97f, 0.90f, 0.5f, kGreen, kGreen);    // bottom right
    // depth test: the near cyan triangle first, then the far magenta one over it
    triangle(v, -0.40f, -0.25f, 0.00f, -0.25f, -0.20f, 0.20f, 0.25f, kCyan);
    triangle(v, -0.25f, -0.10f, 0.15f, -0.10f, -0.05f, 0.25f, 0.75f, kMagenta);
    // depth range: the orange triangle has z = -0.5, outside 0..1 (clipped) but inside -1..1; drawn into
    // a gray square that stays empty when the depth range is 0..1
    quad(v, 0.30f, -0.30f, 0.70f, 0.20f, 0.9f, kGray, kGray);
    triangle(v, 0.35f, -0.25f, 0.65f, -0.25f, 0.50f, 0.15f, -0.5f, kOrange);
    if (!bind_shaders(kPatternVs, kPatternFs)) return;
    const uint32_t bytes = uint32_t(v.size() * sizeof(PatternVertex));
    StreamAlloc s = stream_alloc(bytes, 16);
    if (!s) return;
    memcpy(s.cpu, v.data(), bytes);
    bind_pass_state(false, true);
    set_view(0, 0, g_winW, g_winH);
    static const DkVtxAttribState attribs[] = {
        DkVtxAttribState{0, 0, offsetof(PatternVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(PatternVertex, r), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState buffers[] = {DkVtxBufferState{sizeof(PatternVertex), 0}};
    dkCmdBufBindVtxAttribState(R.cmd, attribs, 2);
    dkCmdBufBindVtxBufferState(R.cmd, buffers, 1);
    const DkBufExtents vb = {s.gpu, bytes};
    dkCmdBufBindVtxBuffers(R.cmd, 0, &vb, 1);
    dkCmdBufDraw(R.cmd, DkPrimitive_Triangles, uint32_t(v.size()), 1, 0, 0);
    // the legend (window coordinates from the top left)
    char status[64];
    snprintf(status, sizeof status, "FRAME %llu, WWHD_DK_TEST_PATTERN=1", (unsigned long long)frame);
    static const float fg[4] = {1, 1, 1, 1}, bg[4] = {0, 0, 0, 0.55f};
    // laid out for 1280x720, scaled with the window
    const float k = float(g_winH) / 720.0f;
    draw_text(int(330 * float(g_winW) / 1280.0f), int(36 * k), std::max(1, int(3 * k)),
              {"DEKO3D TEST PATTERN", status, "", "THIS TEXT UPRIGHT: WINDOW ORIGIN TOP LEFT",
               "RED BOX TOP LEFT, GREEN BOTTOM RIGHT: Y NEGATED", "CYAN IN FRONT OF MAGENTA: DEPTH TEST",
               "GRAY SQUARE EMPTY (NO ORANGE): DEPTH 0 TO 1", "BAR: BLACK LEFT TO WHITE RIGHT",
               "BACKGROUND: BLUE TOP, DARK BOTTOM", "HOLD MINUS: SETTINGS"},
              fg, bg);
}

// ---- performance overlay in the top-left corner (WWHD_FPS, the OpenGL renderer's counter)
std::atomic<int> g_fpsMode{-1};  // -1: not read yet
int overlay_mode() {
    int mode = g_fpsMode.load(std::memory_order_relaxed);
    if (mode < 0) {
        const char* e = getenv("WWHD_FPS");
        mode = e && *e ? atoi(e) : 1;
        int expected = -1;
        if (!g_fpsMode.compare_exchange_strong(expected, mode)) mode = expected;
    }
    return mode;
}

struct OverlayStats {
    double renderBusy = -1, gpuBusyPct = -1, draws = 0;
} overlayStats;

void draw_fps() {
    const int mode = overlay_mode();
    if (mode <= 0) return;
    static uint64_t windowStart = now_ns(), windowFrame = R.frame;
    static double fps = 0;
    const uint64_t now = now_ns();
    if (now - windowStart >= 500'000'000) {
        fps = double(R.frame - windowFrame) * 1e9 / double(now - windowStart);
        windowStart = now;
        windowFrame = R.frame;
    }
    std::vector<std::string> lines;
    char b[32];
    snprintf(b, sizeof b, "%.1f FPS", fps);
    lines.push_back(b);
    if (mode >= 2) {
        auto& s = overlayStats;
        if (s.renderBusy >= 0) {
            snprintf(b, sizeof b, "RT %.0f%% DR %.0f", s.renderBusy / 10, s.draws);
            lines.push_back(b);
        }
        if (s.gpuBusyPct >= 0) {
            snprintf(b, sizeof b, "GPU BUSY %.0f%%", s.gpuBusyPct);
            lines.push_back(b);
        }
    }
    static const float fg[4] = {1.0f, 0.95f, 0.35f, 1.0f}, bg[4] = {0, 0, 0, 0.6f};
    const int scale = std::max(2, int(g_winH) / 180), margin = std::max(4, int(g_winH) / 90);  // 4 and 8 at 720, 6 and 12 at 1080
    draw_text(margin, margin, scale, lines, fg, bg);
}

// ---- picture adjustments (gfx/switch_renderer.h): kept for the present pass of the game's picture
// (P2); the test pattern is drawn as it is
std::mutex g_gradeMu;
gfxsw::PictureGrade g_gradeSet;
bool g_gradeChanged = false;
gfxsw::PictureGrade picture_grade() {
    {
        std::lock_guard<std::mutex> lk(g_gradeMu);
        if (g_gradeChanged) return g_gradeSet;
    }
    static const gfxsw::PictureGrade g = [] {
        gfxsw::PictureGrade g;
        auto read = [](const char* name, float& v, float lo, float hi) {
            const char* e = getenv(name);
            if (!e || !*e) return;
            v = std::clamp(strtof(e, nullptr), lo, hi);
            LOG("[dk] %s=%.2f", name, v);
        };
        read("WWHD_EXPOSURE", g.exposure, 0.25f, 4.0f);
        read("WWHD_CONTRAST", g.contrast, 0.0f, 2.0f);
        read("WWHD_SATURATION", g.saturation, 0.0f, 3.0f);
        read("WWHD_GAMMA", g.gamma, 0.5f, 2.0f);
        return g;
    }();
    return g;
}

// ---- the game's TV picture into the window: fit 16:9 (bars around other shapes), row 0 at the top, with
// the picture adjustments and, for an sRGB TV format, sRGB encoding (as gfx/gl present)
struct PresentUbo {  // present_fsh.glsl, std140
    float grade[4];
    int32_t encode[4];  // x: encode as sRGB (sRGB TV format); y: the image is sRGB (sampling decoded it)
};
bool draw_picture(const PresentSource& src, bool capture) {
    if (!g_presentOk || !src.surface || !src.width || !src.height) return false;
    commit_descriptors();  // present_source wrote the picture's view into kPresentImageId
    const float a = float(src.width) / float(src.height);
    int w = int(g_winW), h = int(float(g_winW) / a);
    if (h > int(g_winH)) {
        h = int(g_winH);
        w = int(float(g_winH) * a);
    }
    const int x = (int(g_winW) - w) / 2, y = (int(g_winH) - h) / 2;
    constexpr uint32_t kUboSize = (sizeof(PresentUbo) + 255) & ~255u;
    StreamAlloc u = stream_alloc(kUboSize, DK_UNIFORM_BUF_ALIGNMENT);
    if (!u) return false;
    const gfxsw::PictureGrade g = picture_grade();
    const bool encode = R.tvSrgb.load(std::memory_order_relaxed);
    PresentUbo p{{g.exposure, g.contrast, g.saturation, g.gamma}, {encode ? 1 : 0, src.srgb ? 1 : 0, 0, 0}};
    memcpy(u.cpu, &p, sizeof p);
    static int encoded = -1;
    if (int(encode) * 2 + int(src.srgb) != encoded) {
        encoded = int(encode) * 2 + int(src.srgb);
        LOG("[dk] presenting the game's picture (%ux%u, GX2 format %03X%s) with %s", src.width, src.height,
            src.surface->format, src.srgb ? ", sRGB" : "", encode ? "sRGB encoding (sRGB TV format)" : "no encoding");
    }
    // a picture of the window's size is copied pixel for pixel; others are filtered
    const bool exact = src.pw == uint32_t(w) && src.ph == uint32_t(h);
    const bool integer = src.surface->fmt.kind != FormatInfo::FLOAT;
    const DkResHandle tex =
        dkMakeTextureHandle(src.imageId, exact || integer ? kPresentNearestSamplerId : kPresentLinearSamplerId);
    bind_pass_state(false, false);
    set_view(uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h));
    const DkShader* sh[] = {&g_presentShaders[0], &g_presentShaders[1]};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_GraphicsMask, sh, 2);
    const DkBufExtents ubo = {u.gpu, kUboSize};
    dkCmdBufBindUniformBuffers(R.cmd, DkStage_Fragment, 0, &ubo, 1);
    dkCmdBufBindTextures(R.cmd, DkStage_Fragment, 0, &tex, 1);
    dkCmdBufBindVtxAttribState(R.cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(R.cmd, nullptr, 0);
    dkCmdBufDraw(R.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    if (capture)
        LOG("[dk] capture frame %llu: presented %s (image %ux%u, descriptor %u) at %d,%d %dx%d", (unsigned long long)R.frame + 1,
            trace_name(src.surface).c_str(), src.pw, src.ph, src.imageId, x, y, w, h);
    return true;
}

// The clocks the console runs at (CPU, GPU, memory), as the OpenGL renderer reports them: clkrst
// (8.0.0+) or pcv; nothing if the game may use neither.
std::string clock_report() {
    static int state = 0;  // 0: not tried, 1: clkrst, 2: pcv, -1: unavailable
    static ClkrstSession sessions[3];
    static const PcvModuleId ids[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
    static const PcvModule modules[3] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
    if (state == 0) {
        state = -1;
        if (hosversionAtLeast(8, 0, 0)) {
            if (R_SUCCEEDED(clkrstInitialize())) {
                bool ok = true;
                for (int i = 0; i < 3 && ok; i++) ok = R_SUCCEEDED(clkrstOpenSession(&sessions[i], ids[i], 3));
                if (ok) state = 1;
            }
        } else if (R_SUCCEEDED(pcvInitialize()))
            state = 2;
        if (state < 0) LOG("[dk] clocks: the clock services (clkrst, pcv) cannot be used: not logged");
    }
    if (state < 0) return "";
    u32 hz[3] = {};
    for (int i = 0; i < 3; i++) {
        if (state == 1) clkrstGetClockRate(&sessions[i], &hz[i]);
        else pcvGetClockRate(modules[i], &hz[i]);
    }
    char b[96];
    snprintf(b, sizeof b, "CPU %u MHz, GPU %u MHz, memory %u MHz", hz[0] / 1000000, hz[1] / 1000000, hz[2] / 1000000);
    return b;
}

// ---- captures (both sticks clicked): the next frame's passes, draws and present pass in the log, and its
// pictures, render targets and textures as PNG files (dk_capture.h, capture.cpp)
std::atomic<bool> g_captureRequested{false};
uint64_t g_captureFrame = ~0ull;
bool capturing() { return R.frame + 1 == g_captureFrame; }

// WWHD_DYNAMIC_RES (on unless =0; gfx/gl's semantics): dynamic resolution needs the GPU passes' idle time
bool dynamic_res_requested() {
    static const bool on = [] {
        const char* e = getenv("WWHD_DYNAMIC_RES");
        return !(e && atof(e) == 0.0 && *e == '0');
    }();
    return on;
}

// ---- dynamic resolution (as gfx/gl's DynamicRes; WWHD_DYNAMIC_RES, on unless =0; =0.x sets the lowest factor,
// default 0.75): when the GPU is the limit (frames below 30 per second with the GPU still on the previous frame
// at present), screen-shaped render targets drop to a lower internal resolution, in steps of 0.05, by what the
// frame rate says is missing; with GPU time to spare at the start of frames (GpuPasses, below) it steps back up.
// A step up that the GPU cannot hold is undone, and the next try waits twice as long (up to a minute). The HUD
// stays at full resolution (draw.cpp). Starts at the requested factor (WWHD_RES_SCALE, at most 1 by default).
struct DynamicRes {
    bool on = false, ready = false;
    float lo = 0.75f, hi = 1.0f, scale = 1.0f;
    uint64_t windowStart = 0, frames = 0, behind = 0;
    uint64_t lastDown = 0, lastUp = 0, backoff = 4'000'000'000ull, calmSince = 0;
    uint32_t idleSamples = 0;  // sampled frames in a row with enough GPU time to spare
    bool settle = false;
    uint64_t idleSeen = 0;     // GpuPasses frames already looked at
    void setup() {
        ready = true;
        hi = res_scale();
        const char* e = getenv("WWHD_DYNAMIC_RES");
        on = dynamic_res_requested();
        if (e && atof(e) > 0) lo = std::clamp(float(atof(e)), 0.5f, 1.0f);
        lo = std::min(lo, hi);
        scale = hi;
        if (on) LOG("[dk] dynamic resolution: %.2f to %.2f when the GPU is the limit (WWHD_DYNAMIC_RES)", lo, hi);
        else LOG("[dk] dynamic resolution: off (WWHD_DYNAMIC_RES=0); internal resolution stays %.2f", hi);
    }
    static float step_down(float s) { return std::floor(s * 20.0f - 0.01f) / 20.0f; }
    void apply(float s, const char* why, double fps, double behindPct) {
        s = std::clamp(s, lo, hi);
        if (s == scale) return;
        LOG("[dk] dynamic resolution %.2f -> %.2f (%s: %.1f fps, GPU still busy at %.0f%% of presents)", scale, s, why, fps,
            behindPct);
        scale = s;
        set_res_scale(s);
        settle = true;
    }
    // once a frame at present
    void frame(uint64_t now, bool gpuBehind, double idleMs, double busyMs, uint64_t passFrames) {
        if (!ready) setup();
        if (!on) return;
        if (!windowStart) windowStart = now;
        frames++;
        behind += gpuBehind;
        // GPU time to spare: the idle start of a sampled frame, enough for one step up with a margin
        if (passFrames != idleSeen && idleMs >= 0 && busyMs > 0) {
            // as if all of the GPU's work grew with the pixel count, plus 3 ms to spare
            idleSeen = passFrames;
            const float next = std::min(hi, scale + 0.05f);
            const double grow = double(next * next) / double(scale * scale) - 1.0;
            idleSamples = idleMs >= 3.0 + busyMs * grow ? idleSamples + 1 : 0;
        }
        if (now - windowStart < 500'000'000ull) return;
        const double secs = double(now - windowStart) / 1e9, fps = double(frames) / secs;
        const double behindPct = 100.0 * double(behind) / double(frames);
        windowStart = now;
        frames = behind = 0;
        if (settle) {  // the window after a change (render targets resampled, the GPU's queue draining)
            settle = false;
            return;
        }
        if (fps < 29.0 && behindPct >= 50) {
            // the share of GPU time to cut (to 31.5 ms frames), most of it scales with the pixel count;
            // at most three steps at a time (each step costs the render targets a resample)
            const double cut = 1.0 - 31.5 / (1000.0 / fps), pixels = std::max(0.5, 1.0 - cut / 0.85);
            float s = std::min(step_down(scale), std::floor(float(scale * std::sqrt(pixels)) * 20.0f) / 20.0f);
            s = std::max(s, std::floor(scale * 20.0f - 0.01f) / 20.0f - 0.10f);
            if (lastUp && now - lastUp < 4'000'000'000ull) backoff = std::min<uint64_t>(backoff * 2, 64'000'000'000ull);
            lastDown = calmSince = now;
            lastUp = 0;
            idleSamples = 0;
            apply(s, "GPU-bound", fps, behindPct);
            return;
        }
        if (behindPct > 10) {
            calmSince = now;
            idleSamples = 0;
        }
        if (now - calmSince > 30'000'000'000ull) backoff = 4'000'000'000ull;
        if (scale < hi && idleSamples >= 3 && fps >= 29.7 && now - lastDown >= backoff && now - calmSince >= 2'000'000'000ull) {
            lastUp = now;
            idleSamples = 0;
            apply(std::min(hi, scale + 0.05f), "GPU time to spare", fps, behindPct);
        }
    }
} dynamicRes;

// ---- GPU time per render pass (as gfx/gl's GpuPasses; WWHD_DK_GPU_PASSES, or WWHD_GL_GPU_PASSES, on unless =0).
// Every 30th frame, a timestamp (dkCmdBufReportCounter DkCounter_Timestamp: written when the work before it
// has passed the ROP, so a pass's pixel work counts in that pass) goes at the frame's start, where its render
// targets change, at clears, copies and present; the next sampled frame reads them (30 frames later: frame_begin
// waited for that frame's fence long before) and the 5 s report lists the passes that took the most GPU time.

// The GPU's timestamps do not count real nanoseconds on the Switch: the Tegra X1's GPU timer runs at 19.2 MHz
// and is read as if it ran at 31.25 MHz (deko3d's dkTimestampToNs: x625/384, the starting factor). The factor
// is measured as gfx/gl GpuClock does: the sampled frames' first timestamps against the CPU clock when they were
// submitted, over the whole session.
struct GpuClock {
    bool have = false;
    uint64_t cpu0 = 0, gpu0 = 0;
    double factor = 31.25 / 19.2;
    bool logged = false;
    void sample(uint64_t cpuNs, uint64_t gpuTs) {
        if (!have) {
            have = true;
            cpu0 = cpuNs;
            gpu0 = gpuTs;
            return;
        }
        if (gpuTs <= gpu0 || cpuNs - cpu0 < 4'000'000'000ull) return;
        factor = double(cpuNs - cpu0) / double(gpuTs - gpu0);
        if (!logged && cpuNs - cpu0 > 30'000'000'000ull) {
            logged = true;
            LOG("[dk] GPU timer: %.4f real ns per GPU ns (measured over %.0f s); GPU times in this log are real time",
                factor, double(cpuNs - cpu0) / 1e9);
        }
    }
} gpuClock;

bool g_gpuPassSampling = false;  // the frame being recorded is sampled

struct GpuPasses {
    static constexpr uint32_t kSlots = kQuerySize / 16;  // 16 bytes per report: counter value, timestamp
    struct Mark {
        uint32_t slot;
        std::string label;
        uint64_t draws;
        uint64_t cpuNs;  // when it was recorded
    };
    // the GPU idle at a sampled frame's start, against the CPU time from the frame's start to the first pass
    // being recorded: equal when the GPU waits for the game to send the frame (not for its own work)
    double idleSumMs = 0, queueSumMs = 0;
    bool on = false, ready = false, fullLogged = false;
    uint64_t startCpu = 0;                    // when the sampled frame's start was submitted (GpuClock)
    double lastIdleMs = -1, lastBusyMs = -1;  // the last sampled frame: GPU idle at its start, and the rest
    uint64_t collected = 0;                   // sampled frames read
    uint64_t incomplete = 0;                  // sampled frames whose timestamps were not all written (skipped)
    std::vector<Mark> marks;                  // of the sampled frame whose results are pending
    struct Total {
        double ms = 0;
        uint64_t draws = 0;
    };
    std::unordered_map<std::string, Total> totals;
    uint64_t frames = 0;
    void setup() {
        ready = true;
        const char* e = getenv("WWHD_DK_GPU_PASSES");
        if (!e) e = getenv("WWHD_GL_GPU_PASSES");
        on = !(e && *e == '0') || dynamic_res_requested();  // dynamic resolution steps up by the sampled idle time
        LOG("[dk] GPU passes: %s", on ? "timestamps every 30th frame ('[dk] GPU passes' every 5 s; WWHD_DK_GPU_PASSES=0 "
                                        "turns them off unless dynamic resolution is on)"
                                      : "off (WWHD_DK_GPU_PASSES=0)");
    }
    uint64_t timestamp(uint32_t slot) const {
        uint64_t t;
        memcpy(&t, query_memory().cpu + size_t(slot) * 16 + 8, 8);
        return t;
    }
    void collect() {  // the previous sampled frame's results
        if (marks.size() < 2) {
            marks.clear();
            return;
        }
        std::vector<uint64_t> t(marks.size());
        for (size_t i = 0; i < marks.size(); i++) {
            t[i] = timestamp(marks[i].slot);
            // (cleared when recorded: a zero, or time going backwards, is a report the GPU did not write)
            if (!t[i] || (i && t[i] < t[i - 1])) {
                incomplete++;
                marks.clear();
                return;
            }
        }
        gpuClock.sample(startCpu, t[0]);
        const double k = gpuClock.factor / 1e6;
        for (size_t i = 0; i + 1 < marks.size(); i++) {
            Total& x = totals[marks[i].label];
            x.ms += double(t[i + 1] - t[i]) * k;
            x.draws += marks[i + 1].draws - marks[i].draws;
        }
        lastIdleMs = double(t[1] - t[0]) * k;
        idleSumMs += lastIdleMs;
        queueSumMs += double(marks[1].cpuNs - marks[0].cpuNs) / 1e6;
        lastBusyMs = double(t.back() - t[1]) * k;
        collected++;
        frames++;
        marks.clear();
    }
    // begin_commands of `frame`, before anything of the frame is recorded
    void frame_start(uint64_t frame) {
        if (!ready) setup();
        g_gpuPassSampling = false;
        if (!on || frame % 30) return;
        collect();
        g_gpuPassSampling = true;
        startCpu = now_ns();
        mark("frame start", nullptr, nullptr);
        // sent now: the GPU writes it when it is done with the previous frame's work, not when the first pass
        // is submitted (what lies between is the GPU idle at the frame's start)
        check_queue("the GPU pass sampling's frame start", frame);
        dkQueueSubmitCommands(R.queue, dkCmdBufFinishList(R.cmd));
        dkQueueFlush(R.queue);
    }
    void mark(const char* kind, const Surface* color, const Surface* depth) {
        if (marks.size() >= kSlots) {
            if (!fullLogged) {
                fullLogged = true;
                LOG("[dk] GPU passes: more than %u marks in frame %llu; the rest of it is not timed", kSlots,
                    (unsigned long long)R.frame + 1);
            }
            return;
        }
        char label[96];
        if (color || depth) {
            const Surface* s = color ? color : depth;
            snprintf(label, sizeof label, "%s %ux%u%s%s", kind, s->width, s->height,
                     color ? (" color " + std::to_string(color->format)).c_str() : "",
                     depth ? (" depth " + std::to_string(depth->format)).c_str() : "");
        } else
            snprintf(label, sizeof label, "%s", kind);
        const uint32_t slot = uint32_t(marks.size());
        memset(query_memory().cpu + size_t(slot) * 16, 0, 16);
        dkCmdBufReportCounter(R.cmd, DkCounter_Timestamp, query_memory().gpu + DkGpuAddr(slot) * 16);
        marks.push_back({slot, label, R.drawCount, now_ns()});
    }
    void frame_end() {  // present, before the frame's fence
        if (!g_gpuPassSampling) return;
        mark("frame end", nullptr, nullptr);
        g_gpuPassSampling = false;
    }
    std::string report() {
        if (!frames) return "";
        std::vector<std::pair<double, std::string>> v;
        double sum = 0;
        for (auto& [label, t] : totals) {
            v.push_back({t.ms / double(frames), label});
            sum += t.ms;
        }
        std::sort(v.rbegin(), v.rend());
        // the GPU's own work: everything but the idle time at the frame's start (P4: the figure to compare
        // between builds and switches, which the CPU's pace does not change)
        double startMs = 0;
        if (auto it = totals.find("frame start"); it != totals.end()) startMs = it->second.ms;
        char head[192];
        snprintf(head, sizeof head, "%.1f ms a frame in %zu kinds of passes (GPU busy %.1f ms without the frame start; "
                 "%llu sampled frames%s):", sum / double(frames), totals.size(), (sum - startMs) / double(frames),
                 (unsigned long long)frames,
                 incomplete ? (", " + std::to_string(incomplete) + " incomplete skipped").c_str() : "");
        std::string out = head;
        for (size_t i = 0; i < v.size() && i < 10; i++) {
            char item[160];
            snprintf(item, sizeof item, " %s%s %.1f ms (%.0f draws)", i ? ";" : "", v[i].second.c_str(), v[i].first,
                     double(totals[v[i].second].draws) / double(frames));
            out += item;
        }
        char tail[160];
        snprintf(tail, sizeof tail, "; frame start: GPU idle %.1f ms, first pass recorded %.1f ms after the frame's start (CPU)",
                 idleSumMs / double(frames), queueSumMs / double(frames));
        out += tail;
        idleSumMs = queueSumMs = 0;
        totals.clear();
        frames = 0;
        incomplete = 0;
        return out;
    }
} gpuPasses;

// ---- statistics every 5 s: frames, the GX2 commands counted, where the render thread spent its time,
// command and stream memory per frame, the heap, the clocks
struct FrameTimes {
    uint64_t presentNs = 0, acquireNs = 0, fenceNs = 0, submitNs = 0;
    uint64_t behind = 0, presents = 0;  // the GPU had not finished the previous frame at present
} g_times;

uint64_t thread_ticks() {
    u64 t = 0;
    if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)))
        svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX);
    return t;
}

uint64_t g_hitches = 0;      // frames over 55 ms (check_hitch)
Renderer::Perf g_hitchBase;  // R.perf after the previous present

void frame_stats() {
    static uint64_t lastFrame = 0, lastWait = 0, lastTicks = 0, lastUnderrun = 0, lastFsCalls = 0, lastFsNs = 0;
    static Renderer::Counts last;
    static auto lastAt = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastAt < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - lastAt).count();
    lastAt = now;
    const uint64_t frames = R.frame - lastFrame;
    auto perFrame = [&](uint64_t n) { return frames ? double(n) / double(frames) : 0.0; };
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / secs; };
    const Renderer::Counts c = R.counts;
    const uint64_t wait = gx2::render_thread_wait_ns();
    const double busy = 1000.0 - ms(wait - lastWait);
    const uint64_t ticks = thread_ticks();
    const double cpuMs = lastTicks ? double(ticks - lastTicks) * 1000.0 / double(armGetSystemTickFreq()) / secs : -1;
    const double behindPct = g_times.presents ? 100.0 * double(g_times.behind) / double(g_times.presents) : -1;
    const MemoryStats m = memory_stats_take();
    uint64_t fsCalls, fsNs, underrun, dropped;
    fs_stats(fsCalls, fsNs);
    audio::stats(underrun, dropped);
    LOG("[dk] %.1f fps; GX2 per frame: %.0f draws, %.0f clears, %.0f surface copies, %.0f scan "
        "copies, %.0f invalidates, %.0f flushes, %.0f waits; render thread busy %.0f ms/s (CPU %.0f ms/s), present %.0f "
        "ms/s (frame fence %.0f, swapchain image %.0f, submit %.0f; window %ux%u, %llu resizes); GPU behind at %.0f%% of presents; command memory "
        "per frame %.0f KiB (max %llu KiB, %llu frames over the %u MiB slice); stream %.1f KiB/frame (%llu full); "
        "heap never used %zu MiB; image heap %llu KiB in %llu chunks, shader code %llu KiB; files %llu (%.0f ms), audio "
        "gaps %.0f ms; deko3d messages %d",
        double(frames) / secs, perFrame(c.draws - last.draws), perFrame(c.clears - last.clears),
        perFrame(c.copies - last.copies), perFrame(c.scans - last.scans), perFrame(c.invalidates - last.invalidates),
        perFrame(c.flushes - last.flushes), perFrame(c.waits - last.waits), busy, cpuMs, ms(g_times.presentNs),
        ms(g_times.fenceNs), ms(g_times.acquireNs), ms(g_times.submitNs), g_winW, g_winH,
        (unsigned long long)g_windowResizes, behindPct,
        m.frames ? double(m.cmdBytesSum) / 1024.0 / double(m.frames) : 0.0, (unsigned long long)(m.cmdBytesMax >> 10),
        (unsigned long long)m.cmdOverflows, kCmdSliceSize >> 20,
        m.frames ? double(m.streamBytesSum) / 1024.0 / double(m.frames) : 0.0, (unsigned long long)m.streamFull,
        heap_never_used_mib(), (unsigned long long)(m.imageBytes >> 10), (unsigned long long)m.imageChunks,
        (unsigned long long)(m.codeBytes >> 10), (unsigned long long)(fsCalls - lastFsCalls), double(fsNs - lastFsNs) / 1e6,
        double(underrun - lastUnderrun) * 1000.0 / audio::kRate, g_debugMessages.load());
    // the draw path: what was executed or skipped and why, time per executed draw by stage (sampled), the
    // caches, bytes streamed and submits
    {
        static uint64_t lastDrawCount = 0;
        const Renderer::Perf& p = R.perf;
        const DrawSkips k = draw_skips_take();
        const uint64_t executed = R.drawCount - lastDrawCount;
        lastDrawCount = R.drawCount;
        auto us = [&](uint64_t ns) { return executed ? double(ns) / 1e3 / double(executed) : 0.0; };
        auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
        auto kib = [&](uint64_t bytes) { return perFrame(bytes) / 1024.0; };
        const uint64_t lookups = executed + k.noFetchShader + k.shaderPending + k.shaderFailed;
        LOG("[dk] draws per frame: %.0f executed; skipped %.1f shader pending, %.1f shader failed, %.1f no fetch shader, "
            "%.1f no target, %.1f empty scissor, %.1f stream full, %.1f unsupported, %.1f GamePad; us per draw: total %.1f "
            "= lookup %.1f + indices %.1f + resources %.1f + state %.1f + submit %.1f; memo %.0f%% combos %.0f%% texture "
            "cache %.0f%% of %.0f lookups/frame; KiB/frame: vertices %.0f, indices %.0f, uniforms %.0f, stream %.0f "
            "(%.0f reused, copy %.1f ms/s); submits %.1f/frame (%.1f for draws, %.1f ms/s); GamePad draws %.1f/frame",
            perFrame(executed), perFrame(k.shaderPending), perFrame(k.shaderFailed), perFrame(k.noFetchShader),
            perFrame(k.noTarget), perFrame(k.scissorEmpty), perFrame(k.streamFull), perFrame(k.unsupported),
            perFrame(p.gamepadSkipped), us(p.drawNs), us(p.lookupNs), us(p.indexNs), us(p.resourceNs), us(p.stateNs),
            us(p.submitNs), pct(p.memoHits, lookups), pct(p.comboHits, lookups), pct(p.textureCacheHits, p.textureLookups),
            perFrame(p.textureLookups), kib(p.vertexBytes), kib(p.indexBytes), kib(p.uboBytes), kib(p.streamBytes),
            kib(p.reusedBytes), ms(p.copyNs), perFrame(p.flushes), perFrame(p.midFrameSubmits), ms(p.flushNs),
            perFrame(p.gamepadDraws));
        log_resource_stats(executed, frames);
        log_lookup_stats(frames);
        const ShaderStats sh = shader_stats_take();
        LOG("[dk] shaders: %llu translated, %llu DKSH in RAM, %llu from cache files, %llu queued, %llu compiled (%.0f ms in "
            "uam), %llu failed, %llu pending now, %llu draws skipped for them; loads %.1f ms, render thread %.1f ms/s "
            "(%llu loads), code %llu KiB",
            (unsigned long long)sh.translations, (unsigned long long)sh.memoryHits, (unsigned long long)sh.cacheHits,
            (unsigned long long)sh.queued, (unsigned long long)sh.compiled, double(sh.compileNs) / 1e6,
            (unsigned long long)sh.failed, (unsigned long long)sh.pendingNow, (unsigned long long)sh.skippedDraws,
            double(sh.loadNs) / 1e6, ms(p.shaderNs), (unsigned long long)p.dkshLoads, (unsigned long long)(sh.codeBytes >> 10));
        const DescriptorStats ds = descriptor_stats_take();
        size_t surfBytes = 0, targetBytes = 0, surfCount = 0;
        surface_memory(surfBytes, targetBytes, surfCount);
        LOG("[dk] surfaces: %zu (%zu MiB, render targets %zu MiB); uploads %.1f/frame (%.0f KiB/frame, %.1f ms/s); clears "
            "%.1f ms/s, copies %.1f ms/s (%llu, %llu on the CPU), invalidates %.1f ms/s, scans %.1f ms/s (%llu blits), "
            "feedback copies %llu, rescales %llu; descriptors: %u images, %u samplers in use, %llu + %llu written, sampler "
            "cache %llu hits %llu evictions; hitches %llu",
            surfCount, surfBytes >> 20, targetBytes >> 20, perFrame(p.uploads), kib(p.uploadBytes), ms(p.uploadNs),
            ms(p.clearNs), ms(p.surfaceCopyNs), (unsigned long long)p.surfaceCopies, (unsigned long long)p.cpuSurfaceCopies,
            ms(p.invalidateNs), ms(p.scanNs), (unsigned long long)p.scanBlits, (unsigned long long)p.feedbackCopies,
            (unsigned long long)p.rescales, ds.imagesUsed, ds.samplersUsed, (unsigned long long)ds.imageWrites,
            (unsigned long long)ds.samplerWrites, (unsigned long long)ds.samplerHits,
            (unsigned long long)ds.samplerEvictions, (unsigned long long)g_hitches);
        LOG("[dk] internal resolution %.2f (requested %.2f; dynamic %s); %llu render targets resampled, %llu from kept "
            "images (%zu MiB kept); HUD at full resolution in %.0f%% of frames",
            double(res_scale()), double(requested_res_scale()), dynamicRes.on ? "on" : "off", (unsigned long long)p.rescales,
            (unsigned long long)p.poolHits, rescale_pool_bytes() >> 20, 100.0 * perFrame(p.hudSwitches));
        R.perf = {};  // (swap() takes the hitch base after present)
    }
    if (std::string passes = gpuPasses.report(); !passes.empty()) LOG("[dk] GPU passes: %s", passes.c_str());
    if (std::string sync = sync_report(frames); !sync.empty()) LOG("[dk] GPU sync %s", sync.c_str());
    if (std::string clocks = clock_report(); !clocks.empty()) LOG("[dk] clocks: %s", clocks.c_str());
    overlayStats.renderBusy = busy;
    overlayStats.gpuBusyPct = behindPct;
    overlayStats.draws = perFrame(c.draws - last.draws);
    g_times = {};
    last = c;
    lastFrame = R.frame;
    lastWait = wait;
    lastTicks = ticks;
    lastUnderrun = underrun;
    lastFsCalls = fsCalls;
    lastFsNs = fsNs;
}

}  // namespace

// GPU time per render pass (dk_draw.h): a timestamp in sampled frames
void gpu_pass_mark(const char* kind, const Surface* color, const Surface* depth) {
    if (g_gpuPassSampling) gpuPasses.mark(kind, color, depth);
}

// The frame's commands start here (dk.h): at the first GX2 command that records after a present, or at the
// present itself when none did
void begin_commands() {
    if (frame_open()) return;
    const uint64_t frame = R.frame + 1;
    const uint64_t t0 = now_ns();
    check_queue("the frame fence", frame);
    g_times.behind += !frame_done(R.frame);  // the previous frame's commands are still running
    frame_begin(frame);
    g_times.fenceNs += now_ns() - t0;
    // conservative (plan section 6.3): the texture, uniform and descriptor caches forget what the CPU
    // rewrote in this slot's stream slice four frames ago; and the previous frame's present pass, which may
    // sample the game's TV buffer itself (present_source), finishes before this frame's clears and draws
    // write to it (gpu_sync.cpp; also the tiled cache's state, WWHD_DK_TILED_CACHE)
    sync_frame_start();
    dkCmdBufBindImageDescriptorSet(R.cmd, image_descriptors(), kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(R.cmd, sampler_descriptors(), kSamplerDescriptors);
    static bool samplersWritten = false;
    if (!samplersWritten) {
        // the renderer's samplers (dk_surfaces.h): 0 linear, clamped (the settings overlay's textures);
        // 1 linear and 2 nearest, clamped (kPresentLinearSamplerId, kPresentNearestSamplerId: the present pass)
        DkSamplerDescriptor d[3];
        for (int i = 0; i < 3; i++) {
            DkSampler s;
            dkSamplerDefaults(&s);
            s.minFilter = s.magFilter = i == 2 ? DkFilter_Nearest : DkFilter_Linear;
            s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
            dkSamplerDescriptorInitialize(&d[i], &s);
        }
        static_assert(kPresentLinearSamplerId == 1 && kPresentNearestSamplerId == 2, "sampler slots");
        dkCmdBufPushData(R.cmd, sampler_descriptors(), d, sizeof d);
        dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        samplersWritten = true;
    }
    gpuPasses.frame_start(frame);  // (every 30th frame: its start's timestamp, sent to the GPU at once)
    // the lanes' per-frame work (docs/deko3d-plan.md, "P2 lanes")
    surfaces_frame_start();
    shaders_frame_start();
    draw_frame_start();
}

// The commands recorded so far go to the GPU (dk_draw.h): GX2Flush, GX2DrawDone and every few hundred draws
// (draw.cpp), so the GPU starts on a frame before it is complete. Recording continues in the same frame.
void submit_commands(const char* why) {
    if (!frame_open()) return;
    // nothing recorded since the last submit: nothing to send
    static uint64_t lastWork = ~0ull;
    const uint64_t work = R.drawCount + R.counts.clears + R.counts.copies + R.counts.scans + R.perf.uploads +
                          R.perf.feedbackCopies + R.perf.flushes;
    if (work == lastWork) return;
    const uint64_t t0 = now_ns();
    {
        Stage stage("deko3d: submitting commands");
        check_queue(why, R.frame + 1);
        dkQueueSubmitCommands(R.queue, dkCmdBufFinishList(R.cmd));
        dkQueueFlush(R.queue);
    }
    R.perf.flushes++;
    if (!strcmp(why, "draws")) R.perf.midFrameSubmits++;
    R.perf.flushNs += now_ns() - t0;
    lastWork = R.drawCount + R.counts.clears + R.counts.copies + R.counts.scans + R.perf.uploads +
               R.perf.feedbackCopies + R.perf.flushes;
    if (g_traceFrame) trace_event("submit (%s)", why);
}

namespace {

// GX2 swap: the present pass into the next swapchain image
void present() {
    const uint64_t frame = R.frame + 1;
    const bool capture = capturing();
    const uint64_t t0 = now_ns();
    g_times.presents++;
    const bool gpuBehind = !frame_done(R.frame);  // the GPU is still on the previous frame (dynamic resolution)
    begin_commands();  // (already open when a GX2 command recorded in this frame)
    {
        uint32_t ww, wh;
        window_wanted(ww, wh);
        if (ww != g_winW || wh != g_winH) {
            Stage stage("deko3d: recreating the swapchain");
            resize_window(ww, wh, frame);
        }
    }
    const uint64_t t1 = now_ns();
    int slot;
    {
        Stage stage("deko3d: acquiring a swapchain image");
        check_queue("acquiring a swapchain image", frame);
        slot = dkQueueAcquireImage(R.queue, g_swapchain);
    }
    const uint64_t t2 = now_ns();
    if (capture) LOG("[dk] capture frame %llu: frame open (fence) %.2f ms, swapchain image %d after %.2f ms",
                     (unsigned long long)frame, double(t1 - t0) / 1e6, slot, double(t2 - t1) / 1e6);
    if (g_traceFrame) trace_event("present");
    gpu_pass_mark("present", nullptr, nullptr);
    forget_state();  // the present pass binds its own targets and state
    // state the game's draws set that the passes below do not: back to the defaults
    static const DkViewportSwizzle kIdentity = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
    dkCmdBufSetViewportSwizzles(R.cmd, 0, &kIdentity, 1);
    dkCmdBufSetPrimitiveRestart(R.cmd, false, 0);
    // what the frame's passes rendered (and uploads still waiting for their barrier) is complete before the
    // picture is sampled (gpu_sync.cpp)
    sync_present();
    const PresentSource src = present_source();
    DkImageView color, depth;
    dkImageViewDefaults(&color, &g_swapImages[slot]);
    dkImageViewDefaults(&depth, &g_depth);
    const DkImageView* colors[] = {&color};
    dkCmdBufBindRenderTargets(R.cmd, colors, 1, &depth);
    set_view(0, 0, g_winW, g_winH);
    // WWHD_DK_TEST_PATTERN=1: P1's test pattern instead of the game's picture (the game runs behind it)
    static const bool testPattern = [] {
        const char* e = getenv("WWHD_DK_TEST_PATTERN");
        const bool on = e && *e && *e != '0';
        LOG("[dk] present: %s", on ? "the TEST PATTERN (WWHD_DK_TEST_PATTERN=1), not the game's picture"
                                   : "the game's TV picture (WWHD_DK_TEST_PATTERN=1 shows the test pattern)");
        return on;
    }();
    dkCmdBufClearColorFloat(R.cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);  // the bars, or no picture yet
    dkCmdBufClearDepthStencil(R.cmd, true, 1.0f, 0xFF, 0);
    static int shown = -1;  // the last frame's: 1 the game's picture, 0 black (logged when it changes)
    if (testPattern) draw_pattern(frame);
    else if (draw_picture(src, capture)) {
        if (shown != 1) LOG("[dk] frame %llu: presenting the game's TV picture", (unsigned long long)frame);
        shown = 1;
    } else {
        // black until the game copies a picture to the TV scan buffer
        if (shown != 0)
            LOG("[dk] frame %llu: no TV picture to present (%s): black", (unsigned long long)frame,
                !g_presentOk ? "the present shaders did not load"
                : !src.surface ? "no scan copy yet"
                               : "the picture has no size");
        shown = 0;
    }
    // the next frame draws into the buffer again: it gets its own copy only if the game copies it (gfx/gl)
    S.scanSrc = nullptr;
    if (S.tvSource) S.tvSource->hudFull = false;
    draw_fps();
    // the settings overlay (Minus held, overlay/overlay.h), over the pattern and the FPS counter
    // (the window's real size: the UI grows with it, overlay.cpp)
    if (ImDrawData* ui = overlay::frame(float(g_winW), float(g_winH), overlay_renderer_init)) overlay_draw(ui, int(g_winW), int(g_winH));
    gpuPasses.frame_end();
    frame_end();
    const uint64_t t3 = now_ns();
    check_queue("the submit", frame);
    dkQueueSubmitCommands(R.queue, dkCmdBufFinishList(R.cmd));
    {
        Stage stage("deko3d: present");
        check_queue("the present", frame);
        dkQueuePresentImage(R.queue, g_swapchain, slot);
    }
    const uint64_t t4 = now_ns();
    if (capture) LOG("[dk] capture frame %llu: recorded in %.2f ms, submitted and presented in %.2f ms", (unsigned long long)frame,
                     double(t3 - t2) / 1e6, double(t4 - t3) / 1e6);
    // the frame's pictures to PNG files (both sticks, WWHD_DUMP_*): waits for the GPU, copies its images back
    if (g_capture) capture_present(g_swapImages[slot], g_winW, g_winH, src);
    g_times.acquireNs += t2 - t1;
    g_times.submitNs += t4 - t3;
    frame_stats();
    dynamicRes.frame(now_ns(), gpuBehind, gpuPasses.lastIdleMs, gpuPasses.lastBusyMs, gpuPasses.collected);
    latch_res_scale();  // a new internal resolution from the next frame on
}

// WWHD_DK_TRACE_FRAMES=n,... (or WWHD_GL_TRACE_FRAMES): those frames' passes in the log, as gfx/gl's
std::vector<uint64_t> frame_list(const char* var, const char* fallbackVar) {
    std::vector<uint64_t> frames;
    const char* e = getenv(var);
    if (!e && fallbackVar) e = getenv(fallbackVar);
    if (e)
        for (const char* p = e; *p;) {
            char* end = nullptr;
            const unsigned long long v = strtoull(p, &end, 10);
            if (end == p) break;
            frames.push_back(v);
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
    return frames;
}

// Frames that took much longer than the game's 33 ms are logged with what the render thread did in them
// ([hitch], as gfx/gl): R.perf minus its copy taken after the previous present covers exactly this frame
void check_hitch(uint64_t now) {
    static uint64_t lastSwap = 0, lastDraws = 0, lastSkipped = 0, lastRenderWait = 0;
    static int logged = 0;
    const uint64_t renderWait = gx2::render_thread_wait_ns();
    if (lastSwap && now - lastSwap > 55'000'000ull) {
        g_hitches++;
        if (logged < 300) {
            logged++;
            const Renderer::Perf& p = R.perf;
            const Renderer::Perf& b = g_hitchBase;
            auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
            const double frameMs = ms(now - lastSwap);
            LOG("[hitch] frame %llu took %.0f ms: render thread busy %.0f ms, %llu draws (%.0f ms; %llu skipped), "
                "shaders %.1f ms (%llu DKSH loads), texture uploads %llu (%.1f ms), clears/copies/invalidates/scans "
                "%.1f ms, submits %llu (%.1f ms), present %.1f ms",
                (unsigned long long)R.frame, frameMs, frameMs - ms(renderWait - lastRenderWait),
                (unsigned long long)(R.drawCount - lastDraws), ms(p.drawNs - b.drawNs),
                (unsigned long long)(R.skippedDraws - lastSkipped), ms(p.shaderNs - b.shaderNs),
                (unsigned long long)(p.dkshLoads - b.dkshLoads), (unsigned long long)(p.uploads - b.uploads),
                ms(p.uploadNs - b.uploadNs),
                ms((p.clearNs + p.surfaceCopyNs + p.invalidateNs + p.scanNs) - (b.clearNs + b.surfaceCopyNs + b.invalidateNs + b.scanNs)),
                (unsigned long long)(p.flushes - b.flushes), ms(p.flushNs - b.flushNs), ms(p.presentNs - b.presentNs));
        }
    }
    lastSwap = now;
    lastDraws = R.drawCount;
    lastSkipped = R.skippedDraws;
    lastRenderWait = renderWait;
}

void swap() {
    const uint64_t start = now_ns();
    check_hitch(start);
    present();
    R.perf.presentNs += now_ns() - start;
    g_times.presentNs += now_ns() - start;
    g_hitchBase = R.perf;
    R.completed = std::atomic_ref<uint64_t>(R.frame).fetch_add(1) + 1;
    if (g_captureRequested.exchange(false)) {
        g_captureFrame = R.frame + 1;
        const Renderer::Counts& c = R.counts;
        LOG("[dk] capture of frame %llu requested: its passes and present pass follow; GX2 so far: %llu draws (%llu "
            "executed), %llu clears, %llu surface copies, %llu scan copies", (unsigned long long)g_captureFrame,
            (unsigned long long)c.draws, (unsigned long long)R.drawCount, (unsigned long long)c.clears,
            (unsigned long long)c.copies, (unsigned long long)c.scans);
    }
    static const std::vector<uint64_t> traced = frame_list("WWHD_DK_TRACE_FRAMES", "WWHD_GL_TRACE_FRAMES");
    g_traceFrame = (!traced.empty() && std::find(traced.begin(), traced.end(), R.frame + 1) != traced.end()) ||
                   R.frame + 1 == g_captureFrame;
    g_captureDraws = R.frame + 1 == g_captureFrame;
    // the next frame's PNG files: a capture writes everything; WWHD_DUMP_FRAMES=n,... the pictures (as gfx/gl:
    // frame_<n>.png, frame_<n>_window.png), WWHD_DUMP_TARGETS=n,... the render targets, WWHD_DUMP_TEXTURES=n,...
    // the sampled textures and their upload data (captures/<n>/)
    static const std::vector<uint64_t> dumpFrames = frame_list("WWHD_DUMP_FRAMES", nullptr);
    static const std::vector<uint64_t> dumpTargets = frame_list("WWHD_DUMP_TARGETS", nullptr);
    static const std::vector<uint64_t> dumpTextures = frame_list("WWHD_DUMP_TEXTURES", nullptr);
    auto listed = [](const std::vector<uint64_t>& v) { return std::find(v.begin(), v.end(), R.frame + 1) != v.end(); };
    uint32_t what = 0;
    if (listed(dumpFrames)) what |= kCapturePictures;
    if (listed(dumpTargets)) what |= kCaptureTargets;
    if (listed(dumpTextures)) what |= kCaptureTextures;
    if (g_captureDraws) what = kCaptureAll;
    if (what || g_capture) capture_arm(what, g_captureDraws ? "both sticks" : "WWHD_DUMP_*");
}

// start-up: a bar filling while shaders_init loads the DKSH caches (as gfx/gl shader_cache_progress), with its
// own small command buffer: nothing else records yet, and each bar waits for the GPU before the next
void shader_cache_progress(size_t done, size_t total) {
    static uint64_t last = 0;
    const uint64_t now = now_ns();
    if (done < total && now - last < 100'000'000) return;
    last = now;
    static DkMemBlock mem = nullptr;
    static DkCmdBuf cmd = nullptr;
    constexpr uint32_t kSize = 64u << 10;
    if (!cmd) {
        DkMemBlockMaker mm;
        dkMemBlockMakerDefaults(&mm, R.device, kSize);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        log_flush();
        mem = dkMemBlockCreate(&mm);
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, R.device);
        cmd = dkCmdBufCreate(&cm);
    }
    dkCmdBufClear(cmd);
    dkCmdBufAddMemory(cmd, mem, 0, kSize);
    check_queue("acquiring a swapchain image for the start-up bar", 0);
    const int slot = dkQueueAcquireImage(R.queue, g_swapchain);
    DkImageView color;
    dkImageViewDefaults(&color, &g_swapImages[slot]);
    const DkImageView* colors[] = {&color};
    dkCmdBufBindRenderTargets(cmd, colors, 1, nullptr);
    const DkViewport vp = {0.0f, 0.0f, float(g_winW), float(g_winH), 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    // the bar laid out for 1280x720, scaled with the window
    auto sx = [](uint32_t v) { return v * g_winW / 1280; };
    auto sy = [](uint32_t v) { return v * g_winH / 720; };
    auto fill = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h, float v) {
        const DkScissor sc = {x, y, w, h};
        dkCmdBufSetScissors(cmd, 0, &sc, 1);
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, v, v, v, 1.0f);
    };
    fill(0, 0, g_winW, g_winH, 0.0f);
    fill(sx(240), sy(344), sx(800), sy(32), 0.2f);
    const uint32_t w = uint32_t(sx(800) * done / std::max<size_t>(total, 1));
    if (w) fill(sx(240), sy(344), std::min<uint32_t>(w, sx(800)), sy(32), 0.9f);
    dkQueueSubmitCommands(R.queue, dkCmdBufFinishList(cmd));
    dkQueuePresentImage(R.queue, g_swapchain, slot);
    dkQueueWaitIdle(R.queue);  // (the command memory is reused by the next bar)
    if (done >= total) {  // the last one: the bar's resources go
        dkCmdBufDestroy(cmd);
        dkMemBlockDestroy(mem);
        cmd = nullptr;
        mem = nullptr;
    }
}

void init() {
    log_heap("before the deko3d setup");
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_message;
    // window origin top left, depth 0 to 1; clip-space y up (dk.h): the test pattern shows them
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    LOG("[dk] creating the device (deko3d %s library)", kDebugLib ? "debug" : "release");
    log_flush();  // deko3d aborts when a creation fails, without returning (debug_message)
    R.device = dkDeviceCreate(&dm);
    LOG("[dk] device created: flags 0x%X (origin upper left, depth 0 to 1, clip-space y up: the renderer's shaders "
        "negate y)", dm.flags);
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, R.device);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_EnableZcull;
    qm.commandMemorySize = 1u << 20;
    qm.flushThreshold = qm.commandMemorySize / 8;
    LOG("[dk] creating the queue");
    log_flush();
    R.queue = dkQueueCreate(&qm);
    LOG("[dk] queue created: graphics, command memory %u KiB", qm.commandMemorySize >> 10);
    memory_init();
    load_shaders();
    text_self_test();
    poll_operation_mode();
    window_wanted(g_winW, g_winH);  // the first swapchain at the current mode's size
    init_swapchain();
    shaders_init(shader_cache_progress);  // the game shaders' worker and caches (dk_shaders.h)
    log_heap("after the deko3d setup");
    input::init();
}

void run_main_loop() {
    while (appletMainLoop()) {
        input::update();
        poll_operation_mode();    // docked / handheld: the window size (present recreates the swapchain)
        hostui::run_posted();     // the settings overlay's changes, on this thread as on the desktop hosts
        switch_settings::tick();  // CPU / GPU clock overrides set again when the system changed them
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    LOG("[boot] host loop ended at frame %llu", (unsigned long long)std::atomic_ref<uint64_t>(R.frame).load());
    fflush(stderr);
    std::_Exit(0);
}

}  // namespace

const DkShader* shader(ShaderId id) { return g_shaderOk[id] ? &g_shaders[id] : nullptr; }

}  // namespace gfxdk

// ---- gfx/switch_renderer.h
namespace gfxsw {
PictureGrade picture_grade_now() { return gfxdk::picture_grade(); }
void set_picture_grade(const PictureGrade& g) {
    std::lock_guard<std::mutex> lk(gfxdk::g_gradeMu);
    gfxdk::g_gradeSet = g;
    gfxdk::g_gradeChanged = true;
}
int fps_overlay_mode() { return gfxdk::overlay_mode(); }
void set_fps_overlay_mode(int mode) { gfxdk::g_fpsMode = std::clamp(mode, 0, 2); }
float dynamic_res_scale() { return gfxdk::res_scale_shown(); }  // the internal resolution in use (dynamic or not)
std::string clock_report_now() { return gfxdk::clock_report(); }
void request_capture() { gfxdk::g_captureRequested = true; }
}  // namespace gfxsw

namespace render {
namespace {
// a GX2 command the renderer cannot handle is skipped (and reported) rather than ending the game, as gfx/gl;
// deko3d's own errors end it through debug_message / check_queue (fatal)
template <class F> void guarded(const char* what, F&& f) {
    try {
        gfxdk::begin_commands();
        f();
    } catch (const std::exception& e) {
        static int reported = 0;
        if (reported++ < 50) LOG("[dk] %s skipped: %s", what, e.what());
    }
}
}  // namespace
const Backend& deko3d_backend() {
    using gfxdk::R;
    static const Backend b = [] {
        Backend b{};
        b.api = Api::Deko3D;
        b.init = gfxdk::init;
        b.run_main_loop = gfxdk::run_main_loop;
        // GX2 render thread: the draw path (dk_draw.h) and the surfaces (dk_surfaces.h)
        b.draw = [](const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                    uint32_t baseVertex, uint32_t instances) {
            guarded("draw", [&] { gfxdk::draw(regs, prim, count, indexType, indexAddr, baseVertex, instances); });
        };
        b.clear_color = [](const uint32_t* regs, uint32_t cb, const float rgba[4]) {
            guarded("color clear", [&] { gfxdk::clear_color(regs, cb, rgba); });
        };
        b.clear_depth_stencil = [](const uint32_t* regs, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
            guarded("depth clear", [&] { gfxdk::clear_depth_stencil(regs, db, depth, stencil, flags); });
        };
        b.copy_surface = [](uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip,
                            uint32_t dstSlice) {
            guarded("surface copy", [&] { gfxdk::copy_surface(src, srcMip, srcSlice, dst, dstMip, dstSlice); });
        };
        b.copy_to_scan = [](uint32_t cb, uint32_t target) {
            guarded("scan copy", [&] { gfxdk::copy_to_scan(cb, target); });
        };
        b.swap = gfxdk::swap;
        b.set_frame_aspect = [](float) {};
        b.target_aspect_factors = [](uint32_t, uint32_t, float& kx, float& ky) {
            kx = ky = 1.0f;
            return false;
        };
        b.frames_completed = [] { return R.completed.load(); };
        b.with_autorelease_pool = [](void (*fn)()) { fn(); };
        b.set_tv_format = [](uint32_t format, bool tv) {
            if (tv) R.tvSrgb = (format & 0x400) != 0;
        };
        b.invalidate = [](uint32_t flags, uint32_t addr, uint32_t size) {
            gfxdk::note_invalidate(flags);  // (P4 index lists)
            guarded("invalidate", [&] { gfxdk::invalidate(flags, addr, size); });
        };
        // GX2Flush / GX2DrawDone: what is recorded goes to the GPU (dk_draw.h submit_commands); nothing the GPU
        // renders is written back to guest memory, so the GPU itself is not waited for (as gfx/gl)
        b.guest_flush = [] {
            R.counts.flushes++;
            guarded("flush", [] { gfxdk::submit_commands("GX2Flush"); });
        };
        b.wait_idle = [] {
            R.counts.waits++;
            R.streamGen++;  // guest data the game changes after GX2DrawDone is uploaded again
            gfxdk::note_wait_idle();
            guarded("wait", [] { gfxdk::submit_commands("GX2DrawDone"); });
        };
        b.ss_reset = [] {
            gfxdk::ss_reset_surfaces();
            gfxdk::reset_shader_memoization();
        };
        b.frame_count = [] { return std::atomic_ref<uint64_t>(R.frame).load(); };
        b.request_tv_dump = [](const std::string&, int) {};
        b.request_capture = [] { gfxsw::request_capture(); };
        b.shutdown = [] { gfxdk::save_shader_cache(); };  // the queue belongs to the render thread
        b.res_scale = gfxdk::requested_res_scale;
        b.set_res_scale = gfxdk::set_res_scale;
        b.ao_mode = gfxdk::ao_mode;  // (read-only, as gfx/gl: env.txt chooses it)
        b.set_ao_mode = [](int) {};
        b.ao_hires = [] { return false; };
        b.set_ao_hires = [](bool) {};
        b.aniso = gfxdk::aniso_enabled;
        b.set_aniso = gfxdk::set_aniso;
        b.fxaa = [] { return false; };
        b.set_fxaa = [](bool) {};
        b.feature_available = [](int f) { return f == render::kFeatureAniso; };
        return b;
    }();
    return b;
}
}  // namespace render

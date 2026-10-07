// The deko3d renderer's device, presentation and host loop, and its entry in the renderer table (dk.h).
// Phase P1 (docs/deko3d-plan.md): every GX2 swap presents a test pattern that shows the device's
// conventions (origin, y direction, depth range, depth test) in one screenshot, the FPS counter and the
// settings overlay (hold Minus). GX2 draws, clears and copies are counted, not executed.
#include <malloc.h>
#include <unistd.h>
extern "C" char* fake_heap_end;  // libnx: the end of the heap malloc grows into (sbrk)
#include <switch.h>

#include "dk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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
#include "text_fsh_dksh.h"
#include "text_vsh_dksh.h"

namespace gfxdk {
Renderer R;

namespace {
constexpr uint32_t kWidth = 1280, kHeight = 720, kSwapImages = 3;
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
          "GX2 so far: %llu draws, %llu clears, %llu surface copies, %llu scan copies (not executed in P1)",
          before, (unsigned long long)frame, g_debugMessages.load(), (unsigned long long)c.draws,
          (unsigned long long)c.clears, (unsigned long long)c.copies, (unsigned long long)c.scans);
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
    LOG("[dk] embedded shaders: %d of %d loaded (%zu bytes of DKSH)", ok, int(kShaderCount), bytes);
}

ImageAlloc g_swapMem[kSwapImages], g_depthMem;

void init_image(DkImage& image, ImageAlloc& mem, DkImageFormat format, uint32_t flags, const char* what) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.flags = flags;
    m.format = format;
    m.dimensions[0] = kWidth;
    m.dimensions[1] = kHeight;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    mem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&image, &layout, mem.block, mem.offset);
    LOG("[dk] %s: %ux%u, %u KiB at image heap offset 0x%X", what, kWidth, kHeight, mem.size >> 10, mem.offset);
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
    LOG("[dk] swapchain: %u RGBA8 images of %ux%u on the default window (%ux%u), swap interval 1", kSwapImages, kWidth,
        kHeight, nw, nh);
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
    uint32_t glyphs[768];
};

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
    set_view(uint32_t(left), uint32_t(top), uint32_t(std::min<int>(w, int(kWidth) - left)),
             uint32_t(std::min<int>(h, int(kHeight) - top)));
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
    set_view(0, 0, kWidth, kHeight);
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
    snprintf(status, sizeof status, "FRAME %llu, GX2 DRAWS NOT EXECUTED", (unsigned long long)frame);
    static const float fg[4] = {1, 1, 1, 1}, bg[4] = {0, 0, 0, 0.55f};
    draw_text(330, 36, 3,
              {"DEKO3D P1 TEST PATTERN", status, "", "THIS TEXT UPRIGHT: WINDOW ORIGIN TOP LEFT",
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
    const int scale = std::max(2, int(kHeight) / 180), margin = std::max(4, int(kHeight) / 90);
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

// ---- captures (both sticks clicked): the next frame's present
// pass is logged step by step
std::atomic<bool> g_captureRequested{false};
uint64_t g_captureFrame = ~0ull;
bool capturing() { return R.frame + 1 == g_captureFrame; }

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
    LOG("[dk] %.1f fps; GX2 counted, not executed (per frame): %.0f draws, %.0f clears, %.0f surface copies, %.0f scan "
        "copies, %.0f invalidates, %.0f flushes, %.0f waits; render thread busy %.0f ms/s (CPU %.0f ms/s), present %.0f "
        "ms/s (frame fence %.0f, swapchain image %.0f, submit %.0f); GPU behind at %.0f%% of presents; command memory "
        "per frame %.0f KiB (max %llu KiB, %llu frames over the %u MiB slice); stream %.1f KiB/frame (%llu full); "
        "heap never used %zu MiB; image heap %llu KiB in %llu chunks, shader code %llu KiB; files %llu (%.0f ms), audio "
        "gaps %.0f ms; deko3d messages %d",
        double(frames) / secs, perFrame(c.draws - last.draws), perFrame(c.clears - last.clears),
        perFrame(c.copies - last.copies), perFrame(c.scans - last.scans), perFrame(c.invalidates - last.invalidates),
        perFrame(c.flushes - last.flushes), perFrame(c.waits - last.waits), busy, cpuMs, ms(g_times.presentNs),
        ms(g_times.fenceNs), ms(g_times.acquireNs), ms(g_times.submitNs), behindPct,
        m.frames ? double(m.cmdBytesSum) / 1024.0 / double(m.frames) : 0.0, (unsigned long long)(m.cmdBytesMax >> 10),
        (unsigned long long)m.cmdOverflows, kCmdSliceSize >> 20,
        m.frames ? double(m.streamBytesSum) / 1024.0 / double(m.frames) : 0.0, (unsigned long long)m.streamFull,
        heap_never_used_mib(), (unsigned long long)(m.imageBytes >> 10), (unsigned long long)m.imageChunks,
        (unsigned long long)(m.codeBytes >> 10), (unsigned long long)(fsCalls - lastFsCalls), double(fsNs - lastFsNs) / 1e6,
        double(underrun - lastUnderrun) * 1000.0 / audio::kRate, g_debugMessages.load());
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

// GX2 swap: the present pass into the next swapchain image
void present() {
    const uint64_t frame = R.frame + 1;
    const bool capture = capturing();
    const uint64_t t0 = now_ns();
    check_queue("the frame fence", frame);
    g_times.presents++;
    g_times.behind += !frame_done(R.frame);  // the previous frame's commands are still running
    frame_begin(frame);
    const uint64_t t1 = now_ns();
    int slot;
    {
        Stage stage("deko3d: acquiring a swapchain image");
        check_queue("acquiring a swapchain image", frame);
        slot = dkQueueAcquireImage(R.queue, g_swapchain);
    }
    const uint64_t t2 = now_ns();
    if (capture) LOG("[dk] capture frame %llu: frame fence %.2f ms, swapchain image %d after %.2f ms", (unsigned long long)frame,
                     double(t1 - t0) / 1e6, slot, double(t2 - t1) / 1e6);
    // conservative (plan section 6.3): the texture, uniform and descriptor caches forget what the CPU
    // rewrote in this slot's stream slice four frames ago
    dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
    dkCmdBufBindImageDescriptorSet(R.cmd, image_descriptors(), kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(R.cmd, sampler_descriptors(), kSamplerDescriptors);
    static bool samplersWritten = false;
    if (!samplersWritten) {
        // sampler 0: linear, clamped (the settings overlay's textures)
        DkSampler s;
        dkSamplerDefaults(&s);
        s.minFilter = s.magFilter = DkFilter_Linear;
        s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
        DkSamplerDescriptor d;
        dkSamplerDescriptorInitialize(&d, &s);
        dkCmdBufPushData(R.cmd, sampler_descriptors(), &d, sizeof d);
        dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        samplersWritten = true;
    }
    DkImageView color, depth;
    dkImageViewDefaults(&color, &g_swapImages[slot]);
    dkImageViewDefaults(&depth, &g_depth);
    const DkImageView* colors[] = {&color};
    dkCmdBufBindRenderTargets(R.cmd, colors, 1, &depth);
    set_view(0, 0, kWidth, kHeight);
    dkCmdBufClearColorFloat(R.cmd, 0, DkColorMask_RGBA, 0.08f, 0.08f, 0.10f, 1.0f);
    dkCmdBufClearDepthStencil(R.cmd, true, 1.0f, 0xFF, 0);
    draw_pattern(frame);
    draw_fps();
    // the settings overlay (Minus held, overlay/overlay.h), over the pattern and the FPS counter
    if (ImDrawData* ui = overlay::frame(float(kWidth), float(kHeight), overlay_renderer_init)) overlay_draw(ui, kWidth, kHeight);
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
    g_times.fenceNs += t1 - t0;
    g_times.acquireNs += t2 - t1;
    g_times.submitNs += t4 - t3;
    frame_stats();
}

void swap() {
    const uint64_t start = now_ns();
    present();
    g_times.presentNs += now_ns() - start;
    R.completed = std::atomic_ref<uint64_t>(R.frame).fetch_add(1) + 1;
    if (g_captureRequested.exchange(false)) {
        g_captureFrame = R.frame + 1;
        const Renderer::Counts& c = R.counts;
        LOG("[dk] capture of frame %llu requested: its present pass follows; GX2 so far: %llu draws, %llu clears, "
            "%llu surface copies, %llu scan copies (not executed in P1)", (unsigned long long)g_captureFrame,
            (unsigned long long)c.draws, (unsigned long long)c.clears, (unsigned long long)c.copies,
            (unsigned long long)c.scans);
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
    init_swapchain();
    log_heap("after the deko3d setup");
    input::init();
}

void run_main_loop() {
    while (appletMainLoop()) {
        input::update();
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
float dynamic_res_scale() { return 1.0f; }  // no dynamic resolution before the game's picture is drawn (P3)
std::string clock_report_now() { return gfxdk::clock_report(); }
void request_capture() { gfxdk::g_captureRequested = true; }
}  // namespace gfxsw

namespace render {
const Backend& deko3d_backend() {
    using gfxdk::R;
    static const Backend b = [] {
        Backend b{};
        b.api = Api::Deko3D;
        b.init = gfxdk::init;
        b.run_main_loop = gfxdk::run_main_loop;
        // P1: the GX2 commands are counted only (render thread)
        b.draw = [](const uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { R.counts.draws++; };
        b.clear_color = [](const uint32_t*, uint32_t, const float*) { R.counts.clears++; };
        b.clear_depth_stencil = [](const uint32_t*, uint32_t, float, uint32_t, uint32_t) { R.counts.clears++; };
        b.copy_surface = [](uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { R.counts.copies++; };
        b.copy_to_scan = [](uint32_t, uint32_t) { R.counts.scans++; };
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
        b.invalidate = [](uint32_t, uint32_t, uint32_t) { R.counts.invalidates++; };
        b.guest_flush = [] { R.counts.flushes++; };
        b.wait_idle = [] { R.counts.waits++; };
        b.ss_reset = [] {};
        b.frame_count = [] { return std::atomic_ref<uint64_t>(R.frame).load(); };
        b.request_tv_dump = [](const std::string&, int) {};
        b.request_capture = [] { gfxsw::request_capture(); };
        b.shutdown = [] {};  // the queue belongs to the render thread; nothing is cached on the SD card yet
        b.res_scale = [] { return 1.0f; };
        b.set_res_scale = [](float) {};
        b.ao_mode = [] { return 0; };
        b.set_ao_mode = [](int) {};
        b.ao_hires = [] { return false; };
        b.set_ao_hires = [](bool) {};
        b.aniso = [] { return false; };
        b.set_aniso = [](bool) {};
        b.fxaa = [] { return false; };
        b.set_fxaa = [](bool) {};
        b.feature_available = [](int) { return false; };
        return b;
    }();
    return b;
}
}  // namespace render

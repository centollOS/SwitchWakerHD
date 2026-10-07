// The OpenGL renderer's device, presentation and host loop, and its entry in the renderer table.
// Switch: EGL on the default NWindow, libnx applet loop and controllers.
// Elsewhere (WWHD_HEADLESS_GL, a debugging aid): a surfaceless Mesa EGL context renders into an
// offscreen "window"; WWHD_DUMP_FRAMES / WWHD_DUMP_TARGETS write PNGs, WWHD_EXIT_AT_FRAME quits.
#include <malloc.h>
#ifdef __SWITCH__
#include <unistd.h>
extern "C" char* fake_heap_end;  // libnx: the end of the heap malloc grows into (sbrk)
#include <switch.h>
#endif

#include "gl.h"
#include <EGL/eglext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "audio_out.h"
#include "gfx/renderer.h"
#include "gx2/gx2.h"
#include "input.h"
#include "platform/host.h"
#include "platform/input_switch.h"
#include "runtime.h"
#include "shaders.h"
#include "settings.h"
#ifdef __SWITCH__
#include "imgui.h"
#include "overlay/hostui.h"
#include "overlay/overlay.h"
#include "platform/mesa_cache_switch.h"
#include "platform/settings_switch.h"
#endif

namespace gx2 { uint64_t flips_presented(); }

#ifdef __SWITCH__
// Mesa's GL thread (glthread) is built into switch-mesa, but its EGL never starts it (desktop Mesa does
// that through DRI config). These are Mesa's own functions in the static libraries.
extern "C" {
void* _glapi_get_context(void);
void _mesa_glthread_init(void* ctx);
void _mesa_glthread_finish(void* ctx);
}
#if defined(WWHD_MESA_STATS)
#include <mesa_switch.h>  // the Mesa of tools/switch/mesa: mesa_switch_glthread_start
#endif
#endif

namespace gfxgl {
Renderer R;

namespace {
constexpr size_t kStreamSize = 32u << 20;
void finish_gl_thread();
#ifdef __SWITCH__
}  // namespace
std::string mesa_probe_report(double secs);  // mesa_probe.cpp
namespace {
#endif
uint64_t g_hitches = 0;      // frames over 55 ms (check_hitch)
Renderer::Perf g_hitchBase;  // R.perf after the previous present
bool g_pipelineStats = false;  // GL_ARB_pipeline_statistics_query (GpuTiming)

// ARB_buffer_storage (core in 4.4; glad is generated for 4.3)
typedef void(APIENTRYP PFNGLBUFFERSTORAGEPROC_WWHD)(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
constexpr GLbitfield GL_MAP_PERSISTENT_BIT_WWHD = 0x0040, GL_MAP_COHERENT_BIT_WWHD = 0x0080;

void APIENTRY debug_message(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    static int logged = 0;
    if (logged++ < 200) LOG("[gl] %s: %s", type == GL_DEBUG_TYPE_ERROR ? "error" : "debug", message);
}

void init_egl() {
#ifdef __SWITCH__
    R.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    R.display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                   : EGL_NO_DISPLAY;
#endif
    if (!R.display || !eglInitialize(R.display, nullptr, nullptr)) throw std::runtime_error("EGL: no display");
    eglBindAPI(EGL_OPENGL_API);
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                    EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
#ifndef __SWITCH__
                                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,  // surfaceless Mesa has no window configs
#endif
                                    EGL_NONE};
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(R.display, configAttribs, &config, 1, &n) || !n) throw std::runtime_error("EGL: no config");
#ifdef __SWITCH__
    R.surface = eglCreateWindowSurface(R.display, config, (EGLNativeWindowType)nwindowGetDefault(), nullptr);
    if (!R.surface) throw std::runtime_error("EGL: no window surface");
#endif
    const EGLint contextAttribs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                     EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
    R.context = eglCreateContext(R.display, config, EGL_NO_CONTEXT, contextAttribs);
    if (!R.context) throw std::runtime_error("EGL: no OpenGL 4.3 core context");
    eglMakeCurrent(R.display, R.surface, R.surface, R.context);
    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) throw std::runtime_error("cannot load OpenGL functions");
    LOG("[gl] %s / %s / %s", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
        (const char*)glGetString(GL_VERSION));
#ifdef __SWITCH__
    eglSwapInterval(R.display, 1);
    EGLint ew = 0, eh = 0;
    eglQuerySurface(R.display, R.surface, EGL_WIDTH, &ew);
    eglQuerySurface(R.display, R.surface, EGL_HEIGHT, &eh);
    u32 nw = 0, nh = 0;
    nwindowGetDimensions(nwindowGetDefault(), &nw, &nh);
    LOG("[gl] window: EGL surface %dx%d, native window %ux%u", ew, eh, nw, nh);
#ifdef __SWITCH__
    log_heap("after the GL setup");
#endif
#endif
}

void init_objects() {
    // debug output costs driver time and floods the log with GLSL portability warnings
    if (getenv("WWHD_GL_DEBUG")) {
        glDebugMessageCallback(debug_message, nullptr);
        glEnable(GL_DEBUG_OUTPUT);
    }
    GLint units = 80;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
    R.scratchUnit = units - 1;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &R.uboAlignment);
    GLint ubos = 0, attribs = 0;
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &ubos);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribs);
    LOG("[gl] texture units %d, uniform buffer bindings %d, vertex attributes %d, UBO alignment %d", units, ubos, attribs,
        R.uboAlignment);
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    auto has = [&](const char* name) {
        for (GLint i = 0; i < extensions; i++)
            if (!strcmp((const char*)glGetStringi(GL_EXTENSIONS, i), name)) return true;
        return false;
    };
    for (const char* e : {"GL_ARB_clip_control", "GL_ARB_texture_view", "GL_ARB_copy_image", "GL_EXT_texture_sRGB_decode",
                          "GL_ARB_buffer_storage", "GL_ARB_viewport_array"})
        LOG("[gl] %s: %s", e, has(e) ? "yes" : "NO");
    g_pipelineStats = has("GL_ARB_pipeline_statistics_query");
    glGenVertexArrays(1, &R.vao);
    glBindVertexArray(R.vao);
    GLuint fbos[3];
    glGenFramebuffers(3, fbos);
    R.drawFbo = fbos[0];
    R.readFbo = fbos[1];
    R.blitFbo = fbos[2];
    glGenBuffers((GLsizei)R.streams.size(), R.streams.data());
    // persistent coherent mappings: one memcpy per upload instead of a driver map/unmap
    auto bufferStorage = has("GL_ARB_buffer_storage") && !getenv("WWHD_GL_NO_PERSISTENT")
                             ? (PFNGLBUFFERSTORAGEPROC_WWHD)eglGetProcAddress("glBufferStorage")
                             : nullptr;
    for (size_t i = 0; i < R.streams.size(); i++) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, R.streams[i]);
        if (bufferStorage) {
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_WWHD | GL_MAP_COHERENT_BIT_WWHD;
            bufferStorage(GL_COPY_WRITE_BUFFER, kStreamSize, nullptr, flags);
            R.streamPtr[i] = (uint8_t*)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, kStreamSize, flags);
        } else
            glBufferData(GL_COPY_WRITE_BUFFER, kStreamSize, nullptr, GL_STREAM_DRAW);
    }
    GLint binaryFormats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
    LOG("[gl] stream buffers: %s; program binary formats: %d", R.streamPtr[0] ? "persistent" : "orphaned", binaryFormats);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifndef __SWITCH__
    // the offscreen stand-in for the Switch's window
    GLuint rb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, R.windowW, R.windowH);
    glGenFramebuffers(1, &R.windowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, R.windowFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

Surface* g_presented = nullptr;  // the TV picture presented last (frame dumps, WWHD_GL_STATS)

std::vector<uint64_t> frame_list(const char* var, const char* fallback) {
    std::vector<uint64_t> frames;
    const char* e = getenv(var);
    if (!e) e = fallback;
    if (e)
        for (const char* p = e; *p;) {
            frames.push_back(strtoull(p, (char**)&p, 10));
            while (*p == ',' || *p == ' ') p++;
        }
    return frames;
}

// A capture on request (request_capture: both sticks clicked on the Switch): the next frame is traced
// into the log (WWHD_GL_TRACE_FRAMES) and its TV picture and render targets are written as PNGs, as
// WWHD_DUMP_FRAMES and WWHD_DUMP_TARGETS would. For pictures that go wrong where the desktop cannot go.
std::atomic<bool> g_captureRequested{false};
uint64_t g_captureFrame = ~0ull;

// WWHD_DUMP_FRAMES=n,...: the TV picture and the window; WWHD_DUMP_TARGETS=n,...: every color target
void frame_dumps(uint64_t frame) {
    static const std::vector<uint64_t> frames = frame_list("WWHD_DUMP_FRAMES", nullptr);
    static const std::vector<uint64_t> targets = frame_list("WWHD_DUMP_TARGETS", nullptr);
    const bool capture = frame == g_captureFrame;
    if (capture || std::find(frames.begin(), frames.end(), frame) != frames.end()) {
        std::string n = std::to_string(frame);
        if (g_presented) dump_surface(g_presented, "frame_" + n + ".png", R.tvSrgb);
        dump_framebuffer(R.windowFbo, R.windowW, R.windowH, "frame_" + n + "_window.png");
    }
    if (capture || std::find(targets.begin(), targets.end(), frame) != targets.end()) {
        // every surface this frame's draws rendered to (depth included), biggest pixel count first
        std::vector<Surface*> drawn;
        for (Surface* s : R.surfaceList)
            if (s->drawFrame == R.frame) drawn.push_back(s);
        std::sort(drawn.begin(), drawn.end(), [](Surface* a, Surface* b) {
            return uint64_t(a->width) * a->height * a->slices > uint64_t(b->width) * b->height * b->slices;
        });
        for (Surface* s : drawn)
            LOG("[gl] frame %llu target %08X: %ux%u%s, GX2 format %03X%s, %u draws", (unsigned long long)frame, s->addr,
                s->width, s->height, s->slices > 1 ? (" x" + std::to_string(s->slices)).c_str() : "", s->format,
                s->isDepth ? " (depth)" : "", s->frameDraws);
        int count = 0;
        for (auto& [addr, s] : R.surfaces) {
            if (!s->gpuWritten || s->fmt.depth || s->fmt.compressed) continue;
            char name[96];
            snprintf(name, sizeof name, "target_%llu_%08X_%ux%u_f%03X.png", (unsigned long long)frame, addr, s->width,
                     s->height, s->format);
            dump_surface(s.get(), name);
            count++;
        }
        LOG("[gl] frame %llu: dumped %d render targets", (unsigned long long)frame, count);
    }
    if (capture) LOG("[gl] capture of frame %llu done (frame_%llu.png, target_%llu_*.png)", (unsigned long long)frame,
                     (unsigned long long)frame, (unsigned long long)frame);
}

// Whether the GPU keeps up: a fence after each frame's commands, checked (without waiting) when the
// next frame is presented. If it has not signalled, the GPU is still on the previous frame: GPU-bound.
// (switch-mesa has no GPU clock, so timestamp queries cannot measure this.)
// One fence per frame, created at present right after the GL thread has caught up (so creating and
// polling them adds no wait). They tell how far the GPU got (R.gpuDoneFrame: stream buffers last
// written in that frame or earlier can be reused without a GL call) and whether it keeps up.
struct GpuBusy {
    std::deque<std::pair<uint64_t, GLsync>> fences;  // frame, fence after its commands
    uint64_t frames = 0, behind = 0;
    bool lastBehind = false;  // at this present, the GPU had not finished the previous frame
    void poll() {
        while (!fences.empty()) {
            GLint status = GL_SIGNALED;
            glGetSynciv(fences.front().second, GL_SYNC_STATUS, sizeof status, nullptr, &status);
            if (status != GL_SIGNALED) break;
            R.gpuDoneFrame = fences.front().first;
            glDeleteSync(fences.front().second);
            fences.pop_front();
        }
    }
    void frame() {
        poll();
        frames++;
        lastBehind = !fences.empty() && fences.back().first + 1 >= R.frame;  // the previous frame is not done
        behind += lastBehind;
        fences.push_back({R.frame, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
        while (fences.size() > 6) {  // never seen: the GPU is six frames behind
            Stage stage("waiting for the GPU: six frames behind");
            glClientWaitSync(fences.front().second, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000ull);
            poll();
        }
    }
    // wait until the GPU has finished `frame` (stream buffer reuse); rare
    void wait_for(uint64_t frame) {
        while (R.gpuDoneFrame < frame && !fences.empty() && fences.front().first <= frame) {
            Stage stage("waiting for the GPU: stream buffer of an earlier frame");
            glClientWaitSync(fences.front().second, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000ull);
            poll();
        }
    }
    // percentage of frames since the last call at which the GPU was still busy, or -1
    double take() {
        double pct = frames ? 100.0 * double(behind) / double(frames) : -1;
        frames = behind = 0;
        return pct;
    }
} gpuBusy;

// Dynamic resolution (WWHD_DYNAMIC_RES, on unless =0; =0.x sets the lowest factor, default 0.75): when the
// GPU is the limit (frames below 30 per second with the GPU still on the previous frame at present),
// screen-shaped render targets drop to a lower internal resolution, in steps of 0.05, by what the frame
// rate says is missing; with GPU time to spare at the start of frames (GpuPasses) it steps back up. A
// step up that the GPU cannot hold is undone, and the next try waits twice as long (up to a minute).
// The HUD is drawn into the same buffer as the scene, so it is scaled too.
bool dynamic_res_requested() {
    static const bool on = [] {
        const char* e = getenv("WWHD_DYNAMIC_RES");
        return !(e && atof(e) == 0.0 && *e == '0');
    }();
    return on;
}
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
        if (on) LOG("[gl] dynamic resolution: %.2f to %.2f when the GPU is the limit (WWHD_DYNAMIC_RES)", lo, hi);
    }
    static float step_down(float s) { return std::floor(s * 20.0f - 0.01f) / 20.0f; }
    void apply(float s, const char* why, double fps, double behindPct) {
        s = std::clamp(s, lo, hi);
        if (s == scale) return;
        LOG("[gl] dynamic resolution %.2f -> %.2f (%s: %.1f fps, GPU still busy at %.0f%% of presents)", scale, s, why, fps,
            behindPct);
        scale = s;
        set_res_scale(s);
        settle = true;
    }
    // once a frame at present, after GpuBusy::frame
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

#ifdef __SWITCH__
// The clocks the console runs at (CPU, GPU, memory), for the logs: overclocking tools change them, and
// a log of a GPU-bound view means little without the GPU clock. clkrst (8.0.0+) or pcv; nothing if the
// game may use neither.
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
        if (state < 0) LOG("[gl] clocks: the clock services (clkrst, pcv) cannot be used: not logged");
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
#endif

// The GPU's timestamps do not count real nanoseconds on the Switch: the Tegra X1's GPU timer runs at
// 19.2 MHz and is read as if it ran at 31.25 MHz (times x0.614). The factor is measured: the GPU
// timestamps of the sampled frames' starts against the CPU clock when they were queued, over the
// whole session (the queueing delay, under a frame, vanishes against minutes).
struct GpuClock {
    bool have = false;
    uint64_t cpu0 = 0, gpu0 = 0;
    double factor =
#ifdef __SWITCH__
        31.25 / 19.2;
#else
        1.0;
#endif
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
            LOG("[gl] GPU timer: %.4f real ns per GPU ns (measured over %.0f s); GPU times in this log are real time",
                factor, double(cpuNs - cpu0) / 1e9);
        }
    }
} gpuClock;

// GPU time per frame: a GL_TIME_ELAPSED query from the end of one present to the start of the next,
// read 8 frames later right after present's wait for the GL thread (with Mesa's GL thread, reading a
// query waits for it: there it costs nothing more). The span includes any time the GPU waited for
// commands, so it is the GPU's frame time when the GPU is the limit. WWHD_GL_PIPESTATS=1 (drivers with
// ARB_pipeline_statistics_query) also counts the vertex and fragment shader invocations and the
// primitives of every 50th frame: the GPU's work, to compare builds or settings at the same frame.
constexpr GLenum kVsInvocations = 0x82F0, kFsInvocations = 0x82F4, kPrimitivesSubmitted = 0x82EF;
struct GpuTiming {
    static constexpr int kSlots = 8;
    GLuint time[kSlots] = {}, vs[kSlots] = {}, fs[kSlots] = {}, prims[kSlots] = {};
    uint64_t frameOf[kSlots];
    bool ready = false, active = false, stats = false;
    double sumMs = 0;
    uint64_t samples = 0;
    void setup() {
        ready = true;
        for (auto& f : frameOf) f = ~0ull;
        glGenQueries(kSlots, time);
        stats = g_pipelineStats && getenv("WWHD_GL_PIPESTATS");
        if (stats) {
            glGenQueries(kSlots, vs);
            glGenQueries(kSlots, fs);
            glGenQueries(kSlots, prims);
        }
    }
    void begin(uint64_t frame) {
        if (!ready) setup();
        const int k = int(frame % kSlots);
        if (frameOf[k] != ~0ull) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(time[k], GL_QUERY_RESULT, &ns);
            sumMs += double(ns) * gpuClock.factor / 1e6;
            samples++;
            if (stats && frameOf[k] % 50 == 0) {
                GLuint64 v = 0, f = 0, p = 0;
                glGetQueryObjectui64v(vs[k], GL_QUERY_RESULT, &v);
                glGetQueryObjectui64v(fs[k], GL_QUERY_RESULT, &f);
                glGetQueryObjectui64v(prims[k], GL_QUERY_RESULT, &p);
                LOG("[gl] GPU work of frame %llu: %.2f ms, vertex shaders %llu, fragment shaders %llu, primitives %llu",
                    (unsigned long long)frameOf[k], double(ns) * gpuClock.factor / 1e6, (unsigned long long)v, (unsigned long long)f,
                    (unsigned long long)p);
            }
        }
        glBeginQuery(GL_TIME_ELAPSED, time[k]);
        if (stats) {
            glBeginQuery(kVsInvocations, vs[k]);
            glBeginQuery(kFsInvocations, fs[k]);
            glBeginQuery(kPrimitivesSubmitted, prims[k]);
        }
        frameOf[k] = frame;
        active = true;
    }
    void end() {
        if (!active) return;
        glEndQuery(GL_TIME_ELAPSED);
        if (stats) {
            glEndQuery(kVsInvocations);
            glEndQuery(kFsInvocations);
            glEndQuery(kPrimitivesSubmitted);
        }
        active = false;
    }
    double take() {
        double v = samples ? sumMs / double(samples) : -1;
        sumMs = 0;
        samples = 0;
        return v;
    }
} gpuTiming;

}  // namespace
// GPU time per render pass (WWHD_GL_GPU_PASSES, on unless =0). Every 30th frame, a GL_TIMESTAMP query
// goes where the frame's render targets change, at clears and at present; the next sampled frame
// reads them (right after present's wait for the GL thread, like GpuTiming) and the 5 s report lists
// the passes that took the most GPU time: what the GPU spends a frame on when it is the limit.
bool g_gpuPassSampling = false;
namespace {

struct GpuPasses {
    struct Mark {
        GLuint query;
        std::string label;
        uint64_t draws;
        uint64_t cpuNs;  // when it was queued
    };
    // the GPU idle at a sampled frame's start, against the CPU time from the swap to the first pass being
    // queued: equal when the GPU waits for the game to send the frame (not for its own work)
    double idleSumMs = 0, queueSumMs = 0;
    bool on = false, ready = false;
    uint64_t startCpu = 0;  // when the sampled frame's start was queued (GpuClock)
    double lastIdleMs = -1, lastBusyMs = -1;  // the last sampled frame: GPU idle at its start, and the rest
    uint64_t collected = 0;                   // sampled frames read
    std::vector<GLuint> pool;
    std::vector<Mark> marks;  // of the sampled frame whose results are pending
    std::string open;         // the pass being recorded
    struct Total {
        double ms = 0;
        uint64_t draws = 0;
    };
    std::unordered_map<std::string, Total> totals;
    uint64_t frames = 0;
    void setup() {
        ready = true;
        const char* e = getenv("WWHD_GL_GPU_PASSES");
        on = !(e && *e == '0') || dynamic_res_requested();  // dynamic resolution steps up by the sampled idle time
    }
    GLuint query() {
        if (pool.empty()) {
            pool.resize(64);
            glGenQueries(GLsizei(pool.size()), pool.data());
        }
        GLuint q = pool.back();
        pool.pop_back();
        return q;
    }
    void collect() {  // the previous sampled frame's results
        if (marks.size() < 2) return;
        std::vector<GLuint64> t(marks.size());
        for (size_t i = 0; i < marks.size(); i++) glGetQueryObjectui64v(marks[i].query, GL_QUERY_RESULT, &t[i]);
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
        for (auto& m : marks) pool.push_back(m.query);
        marks.clear();
    }
    void frame_start(uint64_t frame) {  // after present
        if (!ready) setup();
        if (!on) return;
        g_gpuPassSampling = false;
        if (frame % 30) return;
        collect();
        g_gpuPassSampling = true;
        startCpu = now_ns();
        mark("frame start", nullptr, nullptr);
    }
    void mark(const char* kind, const Surface* color, const Surface* depth) {
        flush_draws();  // the batched draws of the pass before go first
        char label[96];
        if (color || depth) {
            const Surface* s = color ? color : depth;
            snprintf(label, sizeof label, "%s %ux%u%s%s", kind, s->width, s->height,
                     color ? (" color " + std::to_string(color->format)).c_str() : "",
                     depth ? (" depth " + std::to_string(depth->format)).c_str() : "");
        } else
            snprintf(label, sizeof label, "%s", kind);
        // the GPU finishes everything before first (nouveau: a wait for idle): a timestamp alone is
        // written when the commands before it pass the geometry stages, so a pass's pixel work landed
        // in the next pass's time (a 1-draw pass after the scene took 2 ms)
        glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);
        GLuint q = query();
        glQueryCounter(q, GL_TIMESTAMP);
        marks.push_back({q, label, R.drawCount, now_ns()});
    }
    void frame_end() {  // before the swap
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
        char head[96];
        snprintf(head, sizeof head, "%.1f ms a frame in %zu kinds of passes (%llu sampled frames):", sum / double(frames),
                 totals.size(), (unsigned long long)frames);
        std::string out = head;
        for (size_t i = 0; i < v.size() && i < 10; i++) {
            char item[160];
            snprintf(item, sizeof item, " %s%s %.1f ms (%.0f draws)", i ? ";" : "", v[i].second.c_str(), v[i].first,
                     double(totals[v[i].second].draws) / double(frames));
            out += item;
        }
        char tail[160];
        snprintf(tail, sizeof tail, "; frame start: GPU idle %.1f ms, first pass queued %.1f ms after the swap (CPU)",
                 idleSumMs / double(frames), queueSumMs / double(frames));
        out += tail;
        idleSumMs = queueSumMs = 0;
        totals.clear();
        frames = 0;
        return out;
    }
} gpuPasses;
}  // namespace
void gpu_pass_mark(const char* kind, const Surface* color, const Surface* depth) {
    if (g_gpuPassSampling) gpuPasses.mark(kind, color, depth);
}

// ---- WWHD_GL_TRACE_FRAMES (gl.h)
bool g_traceFrame = false;
bool g_captureDraws = false;
uint64_t g_probeStart = [] {
    const char* e = getenv("WWHD_GL_PROBE_FRAME");
    return e ? uint64_t(strtoull(e, nullptr, 10)) : ~0ull;
}();
namespace {
struct PassTrace {
    std::string pass, last;  // the open pass's targets, the last pass's
    uint64_t draws = 0;
    std::vector<const Surface*> sampled;
    void close() {
        if (pass.empty()) return;
        std::string s;
        for (const Surface* t : sampled) s += " " + trace_name(t);
        LOG("[trace] frame %llu pass %s: %llu draws, samples%s", (unsigned long long)(R.frame + 1), pass.c_str(),
            (unsigned long long)draws, s.empty() ? " nothing" : s.c_str());
        pass.clear();
        draws = 0;
        sampled.clear();
    }
} passTrace;
}  // namespace
std::string trace_name(const Surface* s) {
    if (!s) return "-";
    char b[96];
    snprintf(b, sizeof b, "%08X:%ux%u/f%X%s%s", s->addr, s->width, s->height, s->format, s->isDepth ? "/depth" : "",
             s->gpuWritten ? "" : "/cpu");
    std::string n = b;
    // GamePad-picture state (gamepad_only): frames since its GamePad copy and its last read
    if (s->drcScanFrame != ~0ull) n += "/drc-" + std::to_string(R.frame - s->drcScanFrame);
    if (s->drcScanFrame != ~0ull || s->gamepadSource)
        n += s->readFrame == ~0ull ? "/unread" : "/read-" + std::to_string(R.frame - s->readFrame);
    if (s->gamepadSource) n += "/gpsrc";
    return n;
}
void trace_pass(const std::array<Surface*, 8>& colors, const Surface* depth) {
    passTrace.close();
    std::string p;
    for (int i = 0; i < 8; i++)
        if (colors[i]) p += " c" + std::to_string(i) + "=" + trace_name(colors[i]);
    p += " d=" + trace_name(depth);
    passTrace.pass = passTrace.last = p;
}
// A draw's textures are resolved before its render targets are bound (and a new pass traced), so its
// samples wait in pending until the draw is counted, in the pass it belongs to.
void trace_draw(const Surface* sampled) {
    static std::vector<const Surface*> pending;
    if (sampled) {
        if (std::find(pending.begin(), pending.end(), sampled) == pending.end()) pending.push_back(sampled);
        return;
    }
    if (passTrace.pass.empty()) passTrace.pass = passTrace.last + " (continued)";
    passTrace.draws++;
    for (const Surface* t : pending)
        if (std::find(passTrace.sampled.begin(), passTrace.sampled.end(), t) == passTrace.sampled.end())
            passTrace.sampled.push_back(t);
    pending.clear();
}
void trace_event(const char* fmt, ...) {
    passTrace.close();
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    LOG("[trace] frame %llu %s", (unsigned long long)(R.frame + 1), b);
}
namespace {

// CPU load per core (Switch). Horizon reports a core's idle time only to a thread running on that core,
// so a small thread visits cores 0-2 in turn every 5 s. The render thread's own CPU time comes from
// svcGetInfo on itself (the wall-clock "busy" figure also counts time it was preempted).
struct CpuLoad {
    std::atomic<int> pct[3] = {-1, -1, -1};
#ifdef __SWITCH__
    static uint64_t thread_ticks() {
        u64 t = 0;
        if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)))
            svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX);
        return t;
    }
    void start();
#else
    static uint64_t thread_ticks() { return 0; }
    void start() {}
#endif
} cpuLoad;
#ifdef __SWITCH__
void CpuLoad::start() {
    host::start_thread([] {
        auto& self = cpuLoad;
        u64 lastIdle[3] = {}, lastTick[3] = {};
        for (;;) {
            for (int core = 0; core < 3; core++) {
                svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
                svcSleepThread(1'000'000);  // lets the scheduler move this thread
                if (int(svcGetCurrentProcessorNumber()) != core) continue;
                u64 idle = 0;
                if (R_FAILED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX))) continue;
                u64 tick = armGetSystemTick();
                if (lastTick[core] && tick > lastTick[core]) {
                    double busy = 1.0 - double(idle - lastIdle[core]) / double(tick - lastTick[core]);
                    self.pct[core] = int(std::clamp(busy, 0.0, 1.0) * 100 + 0.5);
                }
                lastIdle[core] = idle;
                lastTick[core] = tick;
            }
            svcSleepThread(5'000'000'000ll);
        }
    }, 64 << 10);
}
#endif

struct OverlayStats {
    double fps = 0, renderBusy = -1, gpuBusyPct = -1, draws = 0;
} overlayStats;

// every 5 s: frames, draws, GL errors and where the render thread spent its time (ms per second).
// WWHD_GL_STATS=1 adds the mean colour of the TV picture and the window (full GPU readbacks).
void frame_stats() {
    static uint64_t lastFrame = 0, lastDraws = 0;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    // glGetError waits for Mesa's GL thread to run everything queued: only with WWHD_GL_DEBUG
    uint32_t errors = 0;
    GLenum firstError = GL_NO_ERROR;
    static const bool checkErrors = getenv("WWHD_GL_DEBUG") != nullptr;
    if (checkErrors)
        for (GLenum e; errors < 64 && (e = glGetError()) != GL_NO_ERROR; errors++)
            if (!firstError) firstError = e;
    auto& p = R.perf;
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / secs; };
    std::string memory;
#ifdef __SWITCH__
    // the process's heap is reserved up front: what malloc has handed out is the real use. mallinfo
    // walks the whole heap holding malloc's lock, which stalls every thread that allocates (the
    // audio ones too) for a visible moment: only with WWHD_GL_DEBUG, every 60 s
    static std::string heapText;
    static auto heapAt = now - std::chrono::seconds(60);
    if (checkErrors && now - heapAt >= std::chrono::seconds(60)) {
        heapAt = now;
        struct mallinfo heap = mallinfo();
        heapText = ", heap " + std::to_string(size_t(heap.uordblks) >> 20) + "/" + std::to_string(size_t(heap.arena) >> 20) + " MiB";
    }
    // the heap malloc has never grown into (its break against the end libnx gave it): cheap, no lock.
    // Freed blocks inside the used part are not counted, so this is the floor of what is left.
    {
        char* top = static_cast<char*>(sbrk(0));
        if (fake_heap_end && top && top != reinterpret_cast<char*>(-1)) {
            const size_t mib = heap_never_used_mib();
            memory = ", heap never used " + std::to_string(mib) + " MiB" + heapText;
            static size_t lowest = SIZE_MAX;
            if (mib < 256) {
                if (mib + 32 < lowest) {
                    lowest = mib;
                    LOG("[gl] memory is getting low: %zu MiB of heap never used; texture pool %zu MiB (emptied)", mib,
                        texture_pool_bytes() >> 20);
                }
                texture_pool_clear();
            }
        } else
            memory = heapText;
    }
#endif
    static uint64_t lastSyncWait = 0, lastSyncs = 0, lastHitches = 0;
    const uint64_t syncWait = gx2::game_sync_wait_ns(), syncs = gx2::game_syncs();
    const double gameWaitMs = ms(syncWait - lastSyncWait);
    const uint64_t syncCount = syncs - lastSyncs;
    lastSyncWait = syncWait;
    lastSyncs = syncs;
    static uint64_t lastWait = 0;
    uint64_t wait = gx2::render_thread_wait_ns();
    double busy = 1000.0 - ms(wait - lastWait);
    lastWait = wait;
    double gpuPct = gpuBusy.take();
    static uint64_t lastTicks = 0;
    uint64_t ticks = CpuLoad::thread_ticks();
#ifdef __SWITCH__
    double cpuMs = lastTicks ? double(ticks - lastTicks) * 1000.0 / double(armGetSystemTickFreq()) / secs : -1;
#else
    double cpuMs = -1;
#endif
    lastTicks = ticks;
    char cpu[160];
#ifdef __SWITCH__
    // Mesa's GL thread: its own CPU time (it turns every GL call into GPU commands)
    double glMs = -1;
    {
        extern Handle g_glThreadHandle;
        static uint64_t lastGl = 0;
        u64 t = 0;
        if (g_glThreadHandle != INVALID_HANDLE &&
            R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, g_glThreadHandle, UINT64_MAX))) {
            if (lastGl) glMs = double(t - lastGl) * 1000.0 / double(armGetSystemTickFreq()) / secs;
            lastGl = t;
        }
    }
    char glCpu[48] = "";
    if (glMs >= 0) snprintf(glCpu, sizeof glCpu, ", Mesa GL thread %.0f ms/s", glMs);
#else
    const char* glCpu = "";
#endif
    snprintf(cpu, sizeof cpu, "; CPU: render thread %.0f ms/s%s, cores %d/%d/%d%%", cpuMs, glCpu, cpuLoad.pct[0].load(),
             cpuLoad.pct[1].load(), cpuLoad.pct[2].load());
    // the game's file reads (music streams from the SD card every few seconds) and the sound the
    // audio device lacked (an audible gap)
    static uint64_t lastFsCalls = 0, lastFsNs = 0, lastUnderrun = 0;
    uint64_t fsCalls, fsNs, underrun, dropped;
    fs_stats(fsCalls, fsNs);
    audio::stats(underrun, dropped);
    char io[128];
    snprintf(io, sizeof io, "; files %llu (%.0f ms), audio gaps %.0f ms", (unsigned long long)(fsCalls - lastFsCalls),
             double(fsNs - lastFsNs) / 1e6, double(underrun - lastUnderrun) * 1000.0 / audio::kRate);
    lastFsCalls = fsCalls;
    lastFsNs = fsNs;
    lastUnderrun = underrun;
    memory += io;
    uint64_t draws = R.drawCount - lastDraws;
    double perFrame = R.frame > lastFrame ? double(draws) / double(R.frame - lastFrame) : 0.0;
    const uint64_t opsNs = p.clearNs + p.surfaceCopyNs + p.invalidateNs + p.scanNs;
    auto perFrameOf = [&](uint64_t n) { return R.frame > lastFrame ? double(n) / double(R.frame - lastFrame) : 0.0; };
    LOG("[gl] %.1f fps, %.0f draws/frame, %llu skipped, GL errors %u (first 0x%X); render thread busy %.0f ms/s "
        "(GX2 commands %.0f, flushes %.0f: %.0f per frame), game waited for it %.0f ms/s (%.1f times per frame), %llu hitches, GPU busy at %.0f%% of swaps; draws %.0f ms/s (lookup %.0f, indices %.0f, resources %.0f, "
        "state %.0f, submit %.0f; stream copies %.0f, fence waits %.0f; %.0f%% shader memo hits (+%.0f%% recent combinations), %.0f%% texture cache hits, "
        "%llu feedback copies; shaders %.0f: %llu new states (%llu of programs seen before), %llu compiled, %llu linked; texture uploads %.0f, %llu), "
        "clears %.0f (%.0f/frame), surface copies %.0f (%.0f/frame, %.0f on the CPU), invalidates %.0f (%.0f/frame), scan "
        "copies %.0f, present %.0f (waiting for the GL thread %.0f); streamed %.1f MB/s (reused %.1f; per frame: uniforms %.1f MB, "
        "indices %.1f MB, vertices %.1f MB); %zu surfaces%s%s",
        double(R.frame - lastFrame) / secs, perFrame, (unsigned long long)R.skippedDraws, errors, firstError, busy,
        busy - ms(p.drawNs) - ms(opsNs) - ms(p.presentNs) - ms(p.flushNs), ms(p.flushNs),
        R.frame > lastFrame ? double(p.flushes) / double(R.frame - lastFrame) : 0.0, gameWaitMs,
        R.frame > lastFrame ? double(syncCount) / double(R.frame - lastFrame) : 0.0,
        (unsigned long long)(g_hitches - lastHitches), gpuPct,
        ms(p.drawNs), ms(p.lookupNs), ms(p.indexNs), ms(p.resourceNs), ms(p.stateNs), ms(p.submitNs), ms(p.copyNs),
        ms(p.fenceWaitNs), draws ? 100.0 * double(p.memoHits) / double(draws) : 0.0,
        draws ? 100.0 * double(p.comboHits) / double(draws) : 0.0,
        p.textureLookups ? 100.0 * double(p.textureCacheHits) / double(p.textureLookups) : 0.0,
        (unsigned long long)p.feedbackCopies, ms(p.shaderNs), (unsigned long long)p.shaders,
        (unsigned long long)p.knownProgramShaders,
        (unsigned long long)p.compiled, (unsigned long long)p.linked, ms(p.uploadNs), (unsigned long long)p.uploads,
        ms(p.clearNs), perFrameOf(p.clears), ms(p.surfaceCopyNs), perFrameOf(p.surfaceCopies), perFrameOf(p.cpuSurfaceCopies),
        ms(p.invalidateNs), perFrameOf(p.invalidates), ms(p.scanNs), ms(p.presentNs), ms(p.glThreadWaitNs),
        double(p.streamBytes) / 1e6 / secs, double(p.reusedBytes) / 1e6 / secs,
        R.frame > lastFrame ? double(p.uboBytes) / 1e6 / double(R.frame - lastFrame) : 0.0,
        R.frame > lastFrame ? double(p.indexBytes) / 1e6 / double(R.frame - lastFrame) : 0.0,
        R.frame > lastFrame ? double(p.vertexBytes) / 1e6 / double(R.frame - lastFrame) : 0.0, R.surfaces.size(),
        memory.c_str(), cpuMs >= 0 ? cpu : "");
    if (draws) {
        auto pct = [&](uint64_t n) { return 100.0 * double(n) / double(draws); };
        LOG("[gl] GL state changed per draw: program %.0f%%, vertex buffers %.0f%%, attribute formats %.0f%%, "
            "textures %.0f%%, samplers %.0f%%, game uniform blocks %.0f%%, shader-constant blocks %.0f%% (of draws); "
            "driver draw calls %.0f%% of draws (%llu batched draws in %llu multi-draws, WWHD_GL_BATCH; vertex buffers "
            "rebased for %.0f%% of draws)",
            pct(p.chgProgram), pct(p.chgVertexBuffers), pct(p.chgAttribFormats), pct(p.chgTextures), pct(p.chgSamplers),
            pct(p.chgUbos), pct(p.chgUniformBlocks),
            pct(draws - p.batchedDraws + p.batches), (unsigned long long)p.batchedDraws, (unsigned long long)p.batches,
            pct(p.rebasedDraws));
    }
#ifdef __SWITCH__
    LOG("[gl] %s", mesa_probe_report(secs).c_str() + 2);  // its own line: the log cuts lines at 2 KB
#endif
    {
        // where the memory goes (round 25): the heap left, textures, the shader store and GL objects
        size_t texBytes, targetBytes, texCount, srcCount, storeBytes, objects, programs;
        texture_memory(texBytes, targetBytes, texCount);
        shader_memory(srcCount, storeBytes, objects, programs);
        char heap[48] = "";
#ifdef __SWITCH__
        snprintf(heap, sizeof heap, "heap never used %zu MiB; ", heap_never_used_mib());
#else
        snprintf(heap, sizeof heap, "malloc in use %zu MiB; ", size_t(mallinfo2().uordblks) >> 20);
#endif
        LOG("[gl] memory: %stextures %zu MiB in %zu surfaces (render targets %zu MiB, pool %zu MiB); shaders: %zu "
            "sources (%zu KB), %zu compiled objects, %zu programs", heap, texBytes >> 20, texCount, targetBytes >> 20,
            texture_pool_bytes() >> 20, srcCount, storeBytes >> 10, objects, programs);
    }
    if (double gpuMs = gpuTiming.take(); gpuMs >= 0)
        LOG("[gl] GPU time per frame %.1f ms (from one present to the next; the GPU's own time when it is the limit); "
            "internal resolution %.2f (%llu render targets resampled, %llu from kept textures; HUD at full resolution in %.0f%% of frames); "
            "TV picture copied in %.0f%% of frames",
            gpuMs, double(res_scale()), (unsigned long long)p.rescales, (unsigned long long)p.poolHits,
            100.0 * perFrameOf(p.hudSwitches),
            100.0 * perFrameOf(p.scanBlits));
    if (std::string passes = gpuPasses.report(); !passes.empty()) LOG("[gl] GPU passes: %s", passes.c_str());
#ifdef __SWITCH__
    if (std::string clocks = clock_report(); !clocks.empty()) LOG("[gl] clocks: %s", clocks.c_str());
    if (std::string mesa = mesa_cache::report(); !mesa.empty()) LOG("%s", mesa.c_str());
    {
        // the shader budget (shaders.h): compiles and links left for later frames (their draws skipped)
        static ShaderBudgetStats last;
        const ShaderBudgetStats now = shader_budget_stats();
        if (now.deferredCompiles != last.deferredCompiles || now.deferredLinks != last.deferredLinks)
            LOG("[gl] shader budget: %llu compiles and %llu links waited for a later frame",
                (unsigned long long)(now.deferredCompiles - last.deferredCompiles),
                (unsigned long long)(now.deferredLinks - last.deferredLinks));
        last = now;
    }
#endif
    if (p.gamepadDraws || p.gamepadClearsSkipped)
        LOG("[gl] GamePad picture: %.0f draws/frame into its own buffers (%.0f ms/s), %s (%.0f draws, %.0f clears per frame; "
            "WWHD_GL_SKIP_GAMEPAD)",
            perFrameOf(p.gamepadDraws), ms(p.gamepadDrawNs), skip_gamepad() ? "skipped" : "drawn", perFrameOf(p.gamepadSkipped),
            perFrameOf(p.gamepadClearsSkipped));
    overlayStats.renderBusy = busy;
    overlayStats.gpuBusyPct = gpuPct;
    overlayStats.draws = perFrame;
    p = {};
    static const bool means = getenv("WWHD_GL_STATS") != nullptr;
    if (means) {
        float mean[4] = {}, window[4] = {};
        if (g_presented) surface_mean(g_presented, mean);
        framebuffer_mean(R.windowFbo, R.windowW, R.windowH, window);
        LOG("[gl] TV mean %.3f %.3f %.3f, window mean %.3f %.3f %.3f", mean[0], mean[1], mean[2], window[0], window[1],
            window[2]);
    }
    lastHitches = g_hitches;
    lastFrame = R.frame;
    lastDraws = R.drawCount;
}

#ifndef GL_TEXTURE_SRGB_DECODE_EXT
#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#define GL_SKIP_DECODE_EXT 0x8A4A
#endif

// a full-window triangle sampling the scan texture: guest row 0 (texture row 0) at the top of the window
GLuint present_program() {
    static GLuint prog = [] {
        static const char* vsText =
            "#version 330 core\n"
            "out vec2 uv;\n"
            "void main() {\n"
            "    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
            "    uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
            "    gl_Position = vec4(p, 0.0, 1.0);\n"
            "}\n";
        static const char* fsText =
            "#version 330 core\n"
            "uniform sampler2D scan;\n"
            "uniform int encodeSrgb;\n"
            "uniform vec4 grade;\n"  // exposure, contrast, saturation, gamma (picture_grade())
            "in vec2 uv;\n"
            "out vec4 color;\n"
            "void main() {\n"
            "    vec3 c = texture(scan, uv).rgb;\n"
            "    if (encodeSrgb != 0) {\n"
            "        c = clamp(c * grade.x, 0.0, 1.0);\n"
            "        c = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));\n"
            "    } else\n"
            "        c = clamp(c * grade.x, 0.0, 1.0);\n"
            "    if (grade.y != 1.0) c = clamp(mix(c, c * c * (3.0 - 2.0 * c), grade.y - 1.0), 0.0, 1.0);\n"
            "    if (grade.z != 1.0) {\n"
            "        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
            "        c = clamp(mix(vec3(luma), c, grade.z), 0.0, 1.0);\n"
            "    }\n"
            "    if (grade.w != 1.0) c = pow(c, vec3(grade.w));\n"
            "    color = vec4(c, 1.0);\n"
            "}\n";
        auto compile = [](GLenum type, const char* text) {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &text, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[1024] = {};
                glGetShaderInfoLog(s, sizeof log, nullptr, log);
                LOG("[gl] present shader: %s", log);
            }
            return s;
        };
        GLuint p = glCreateProgram();
        glAttachShader(p, compile(GL_VERTEX_SHADER, vsText));
        glAttachShader(p, compile(GL_FRAGMENT_SHADER, fsText));
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            LOG("[gl] present program does not link; falling back to a blit");
            return 0u;
        }
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "scan"), R.scratchUnit);
        return p;
    }();
    return prog;
}

// test aid: WWHD_GL_TEST_GPU_LOAD=n adds n loop iterations per pixel of a screen-sized target at the
// internal resolution each frame: GPU work that shrinks with the resolution, to watch dynamic resolution
// work on a GPU that otherwise keeps up
void test_gpu_load() {
    static const int n = getenv("WWHD_GL_TEST_GPU_LOAD") ? atoi(getenv("WWHD_GL_TEST_GPU_LOAD")) : 0;
    if (n <= 0) return;
    static GLuint prog = 0, fbo = 0, tex = 0;
    static GLint iterLoc = -1;
    static uint32_t w = 0, h = 0;
    if (!prog) {
        const char* vs = "#version 330 core\nvoid main() { vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), "
                         "float((gl_VertexID & 2) * 2 - 1)); gl_Position = vec4(p, 0.0, 1.0); }\n";
        const char* fs = "#version 330 core\nuniform int n; out vec4 c; void main() { float a = gl_FragCoord.x * 0.001 + "
                         "gl_FragCoord.y; for (int i = 0; i < n; i++) a = fract(sin(a * 12.9898 + float(i)) * 43758.5453); "
                         "c = vec4(a); }\n";
        prog = glCreateProgram();
        for (auto [type, text] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs}}) {
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &text, nullptr);
            glCompileShader(sh);
            glAttachShader(prog, sh);
        }
        glLinkProgram(prog);
        iterLoc = glGetUniformLocation(prog, "n");
        glGenFramebuffers(1, &fbo);
        LOG("[gl] test GPU load: %d iterations per pixel (WWHD_GL_TEST_GPU_LOAD)", n);
    }
    const uint32_t tw = scaled_size(1280, res_scale()), th = scaled_size(720, res_scale());
    forget_gl_state();
    if (tw != w || th != h) {
        if (tex) glDeleteTextures(1, &tex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, tw, th);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);
        w = tw;
        h = th;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CLIP_DISTANCE0);  // (draw.cpp: the game's near-plane clip distance)
    glDisablei(GL_BLEND, 0);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (auto cc = clip_control()) cc(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glViewportIndexedf(0, 0, 0, float(w), float(h));
    glUseProgram(prog);
    glUniform1i(iterLoc, n);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
}

GLuint present_sampler() {
    static GLuint s = [] {
        GLuint s;
        glGenSamplers(1, &s);
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);  // bytes as stored, like a blit
        while (glGetError() != GL_NO_ERROR) {}
        return s;
    }();
    return s;
}

// ---- performance overlay in the top-left corner: WWHD_FPS=0 hides it, 1 (default) shows the frame
// rate, 2 adds render-thread load, GPU lag and draws per frame. A 3x5 pixel font; the text is drawn
// by one fragment shader from a bitmask per character.
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
        {'.', "000000000000010"},
    };
    for (auto& g : kFont)
        if (g.c == c) {
            uint32_t bits = 0;
            for (int i = 0; i < 15; i++) bits = bits << 1 | uint32_t(g.rows[i] == '1');
            return bits;
        }
    return 0;  // space and anything else
}

constexpr int kOverlayColumns = 16, kOverlayRows = 3;

GLuint overlay_program() {
    static GLuint prog = [] {
        static const char* vsText =
            "#version 330 core\n"
            "void main() {\n"
            "    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
            "    gl_Position = vec4(p, 0.0, 1.0);\n"
            "}\n";
        // glyph bits: row 0 (top) in bits 14-12, column 0 (left) the highest bit of a row
        static const char* fsText =
            "#version 330 core\n"
            "uniform int glyph[48];\n"
            "uniform ivec2 grid;\n"   // columns, rows
            "uniform vec3 box;\n"     // left, top (window coordinates), pixels per font pixel
            "out vec4 color;\n"
            "void main() {\n"
            "    ivec2 p = ivec2(floor(vec2(gl_FragCoord.x - box.x, box.y - gl_FragCoord.y) / box.z)) - ivec2(1);\n"
            "    bool on = false;\n"
            "    if (p.x >= 0 && p.y >= 0) {\n"
            "        ivec2 cell = p / ivec2(4, 6), sub = p - cell * ivec2(4, 6);\n"
            "        if (cell.x < grid.x && cell.y < grid.y && sub.x < 3 && sub.y < 5)\n"
            "            on = ((glyph[cell.y * grid.x + cell.x] >> ((4 - sub.y) * 3 + (2 - sub.x))) & 1) != 0;\n"
            "    }\n"
            "    color = on ? vec4(1.0, 0.95, 0.35, 1.0) : vec4(0.0, 0.0, 0.0, 0.6);\n"
            "}\n";
        auto compile = [](GLenum type, const char* text) {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &text, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[1024] = {};
                glGetShaderInfoLog(s, sizeof log, nullptr, log);
                LOG("[gl] overlay shader: %s", log);
            }
            return s;
        };
        GLuint p = glCreateProgram();
        glAttachShader(p, compile(GL_VERTEX_SHADER, vsText));
        glAttachShader(p, compile(GL_FRAGMENT_SHADER, fsText));
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            LOG("[gl] overlay program does not link; no FPS counter");
            return 0u;
        }
        return p;
    }();
    return prog;
}

void draw_overlay(int ww, int wh) {
    const int mode = overlay_mode();
    if (mode <= 0) return;
    // the frame rate over the last half second
    static uint64_t windowStart = now_ns(), windowFrame = R.frame;
    static double fps = 0;
    uint64_t now = now_ns();
    if (now - windowStart >= 500'000'000) {
        fps = double(R.frame - windowFrame) * 1e9 / double(now - windowStart);
        windowStart = now;
        windowFrame = R.frame;
    }
    GLuint prog = overlay_program();
    if (!prog) return;
    char lines[kOverlayRows][kOverlayColumns + 1] = {};
    int rows = 1;
    snprintf(lines[0], sizeof lines[0], "%.1f FPS", fps);
    if (mode >= 2) {
        auto& s = overlayStats;
        if (s.renderBusy >= 0) snprintf(lines[rows++], sizeof lines[0], "RT %.0f%% DR %.0f", s.renderBusy / 10, s.draws);
        if (s.gpuBusyPct >= 0) snprintf(lines[rows++], sizeof lines[0], "GPU BUSY %.0f%%", s.gpuBusyPct);
    }
    int columns = 0;
    for (int r = 0; r < rows; r++) columns = std::max(columns, int(strlen(lines[r])));
    GLint glyphs[kOverlayColumns * kOverlayRows] = {};
    for (int r = 0; r < rows; r++)
        for (int c = 0; lines[r][c]; c++) glyphs[r * columns + c] = GLint(glyph_bits(lines[r][c]));
    const int scale = std::max(2, wh / 180);  // 4 window pixels per font pixel at 720p
    const int margin = std::max(4, wh / 90);
    const int w = (columns * 4 + 1) * scale, h = (rows * 6 + 1) * scale;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CLIP_DISTANCE0);  // (draw.cpp: the game's near-plane clip distance)
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (auto cc = clip_control()) cc(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glViewportIndexedf(0, float(margin), float(wh - margin - h), float(w), float(h));
    glEnablei(GL_BLEND, 0);
    glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
    glUseProgram(prog);
    // looked up once: a query would wait for Mesa's GL thread every frame
    static const GLint glyphLoc = glGetUniformLocation(prog, "glyph"), gridLoc = glGetUniformLocation(prog, "grid"),
                       boxLoc = glGetUniformLocation(prog, "box");
    glUniform1iv(glyphLoc, columns * rows, glyphs);
    glUniform2i(gridLoc, columns, rows);
    glUniform3f(boxLoc, float(margin), float(wh - margin), float(scale));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisablei(GL_BLEND, 0);
    (void)ww;
}

// Picture adjustments from env.txt, applied when presenting (neutral = 1):
//   WWHD_EXPOSURE   scales the linear picture before sRGB encoding (< 1 tames bright areas)
//   WWHD_CONTRAST   S-curve around mid-grey; black and white stay put, so nothing clips (> 1 = more)
//   WWHD_SATURATION colour intensity (0 = grey, > 1 = more vivid)
//   WWHD_GAMMA      > 1 deepens mid-tones and shadows, < 1 lifts them
// The settings overlay changes them while the game runs (set_picture_grade): a generation tells
// present() to send the uniform again.
std::mutex g_gradeMu;
std::atomic<uint32_t> g_gradeGen{1};
PictureGrade g_gradeSet;
bool g_gradeChanged = false;
PictureGrade picture_grade() {
    {
        std::lock_guard<std::mutex> lk(g_gradeMu);
        if (g_gradeChanged) return g_gradeSet;
    }
    static const PictureGrade g = [] {
        PictureGrade g;
        auto read = [](const char* name, float& v, float lo, float hi) {
            const char* e = getenv(name);
            if (!e || !*e) return;
            v = std::clamp(strtof(e, nullptr), lo, hi);
            LOG("[gl] %s=%.2f", name, v);
        };
        read("WWHD_EXPOSURE", g.exposure, 0.25f, 4.0f);
        read("WWHD_CONTRAST", g.contrast, 0.0f, 2.0f);
        read("WWHD_SATURATION", g.saturation, 0.0f, 3.0f);
        read("WWHD_GAMMA", g.gamma, 0.5f, 2.0f);
        return g;
    }();
    return g;
}

// TV scan buffer -> window: fit 16:9, flip rows (guest images keep row 0 at the top)
void present() {
    flush_draws();
    if (g_traceFrame) trace_event("present");
    test_gpu_load();
    gpu_pass_mark("present", nullptr, nullptr);
    gpuTiming.end();
#ifndef __SWITCH__
    check_texture_errors();
#endif
    forget_gl_state();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.windowFbo);
#ifdef __SWITCH__
    glDrawBuffer(GL_BACK);
#endif
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_RASTERIZER_DISCARD);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    EGLint ww = R.windowW, wh = R.windowH;
#ifdef __SWITCH__
    // switch-mesa leaves the EGL surface size at 0x0: ask the native window
    u32 nw = 0, nh = 0;
    if (R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &nw, &nh)) && nw && nh) {
        ww = EGLint(nw);
        wh = EGLint(nh);
    }
#endif
    // the TV picture: its buffer itself when nothing wrote to it since the game copied it
    Surface* const presented = R.scanSrc ? R.scanSrc : R.tvScan.get();
    if (Surface* scan = presented) {
        float a = float(scan->width) / scan->height;
        int w = ww, h = int(ww / a);
        if (h > wh) { h = wh; w = int(wh * a); }
        int x = (ww - w) / 2, y = (wh - h) / 2;
        if (x != 0 || y != 0 || w != ww || h != wh) {  // bars around the picture (else every pixel is drawn)
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        GLuint prog = scan->target == GL_TEXTURE_2D ? present_program() : 0;
        if (prog) {
            glDisablei(GL_BLEND, 0);
            glDisable(GL_COLOR_LOGIC_OP);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
    glDisable(GL_CLIP_DISTANCE0);  // (draw.cpp: the game's near-plane clip distance)
            glDisable(GL_CULL_FACE);
            glDisable(GL_POLYGON_OFFSET_FILL);
            if (auto cc = clip_control()) cc(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
            glViewportIndexedf(0, float(x), float(y), float(w), float(h));
            glDepthRangef(0, 1);
            glUseProgram(prog);
            static const GLint encodeLoc = glGetUniformLocation(prog, "encodeSrgb");
            static const GLint gradeLoc = glGetUniformLocation(prog, "grade");
            static uint32_t gradeSent = 0;
            if (const uint32_t gen = g_gradeGen.load(std::memory_order_acquire); gen != gradeSent) {
                const PictureGrade g = picture_grade();
                glUniform4f(gradeLoc, g.exposure, g.contrast, g.saturation, g.gamma);
                gradeSent = gen;
            }
            static int encoded = -1;
            const int encode = R.tvSrgb.load(std::memory_order_relaxed) ? 1 : 0;
            if (encode != encoded) {
                glUniform1i(encodeLoc, encode);
                encoded = encode;
                LOG("[gl] presenting with %s", encode ? "sRGB encoding (sRGB TV format)" : "no encoding");
            }
            glActiveTexture(GL_TEXTURE0 + R.scratchUnit);
            glBindTexture(GL_TEXTURE_2D, scan->tex);
            glBindSampler(R.scratchUnit, present_sampler());
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindSampler(R.scratchUnit, 0);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, R.readFbo);
            attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, scan, 0, 0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, scan->width, scan->height, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT,
                              scan->fmt.kind == FormatInfo::FLOAT ? GL_LINEAR : GL_NEAREST);
            attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, nullptr, 0, 0);
        }
    }
    if (!presented) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    draw_overlay(ww, wh);
#ifdef __SWITCH__
    // the settings overlay (Minus held, overlay/overlay.h), over the picture and the FPS counter
    if (ImDrawData* ui = overlay::frame(float(ww), float(wh), overlay_renderer_init)) overlay_draw(ui, ww, wh);
#endif
    g_presented = presented;
    frame_dumps(R.frame + 1);
    frame_stats();
    // the next frame draws into the buffer again: it gets its own copy only if the game copies it
    R.scanSrc = nullptr;
    if (R.tvSource) R.tvSource->hudFull = false;  // the scene's resolution again (at its next clear or draw)
#ifdef __SWITCH__
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    {
        Stage stage("present: waiting for the GL thread");
        finish_gl_thread();  // switch-mesa's eglSwapBuffers flushes the driver from this thread
    }
    // the frame's texture allocation errors: glGetError waits for Mesa's GL thread to run everything
    // queued, which has just been done (at the start of present it waited for the whole frame: 10 ms/s
    // on average, up to 80 ms in a second at area changes)
    check_texture_errors();
#endif
    gpuPasses.frame_end();
    gpuBusy.frame();  // its fence queries wait for the GL thread too: right after the wait above
    dynamicRes.frame(now_ns(), gpuBusy.lastBehind, gpuPasses.lastIdleMs, gpuPasses.lastBusyMs, gpuPasses.collected);
#ifdef __SWITCH__
    {
        Stage stage("present: eglSwapBuffers");
        eglSwapBuffers(R.display, R.surface);
    }
#endif
    gpuTiming.begin(R.frame + 1);
    gpuPasses.frame_start(R.frame + 1);
    latch_res_scale();
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
}

// startup: a bar filling while the shader cache compiles (the game has not drawn anything yet)
void shader_cache_progress(size_t done, size_t total) {
    forget_gl_state();
#ifdef __SWITCH__
    static uint64_t last = 0;
    uint64_t now = now_ns();
    if (done < total && now - last < 100'000'000) return;
    last = now;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(240, 344, 800, 32);
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(240, 344, GLsizei(800 * done / std::max<size_t>(total, 1)), 32);
    glClearColor(0.9f, 0.9f, 0.9f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    eglSwapBuffers(R.display, R.surface);
#else
    (void)done;
    (void)total;
#endif
}

// ---- WWHD_GL_THREAD=1 (Switch): Mesa's GL thread. Our GL calls are recorded and a second thread runs
// Mesa and the nouveau driver, so the render thread's own work overlaps the driver's.
// Starting it needs st_manager::set_background_context, which switch-mesa's EGL leaves empty (the
// worker thread calls it first). The Mesa of tools/switch/mesa starts it itself (mesa_switch_glthread_start,
// patch 0005): its structures differ from the package's (gl_context::st at 0x22E78, not 0x22E88), and a
// fixed offset left the GL thread off in round 30's first build (every GL call on the render thread).
// devkitPro's package: the address is taken from that build's st_set_background_context:
//   ldr x0, [ctx, #0x22E88]  (gl_context::st)   ldr x2, [x0]  (st_context_private: the st_manager)
//   ldr x2, [x2, #24]        (set_background_context)
#ifdef __SWITCH__
void* g_glThreadCtx = nullptr;  // the context whose GL thread is running
constexpr size_t kGlContextSt = 0x22E88, kManagerSetBackgroundContext = 24, kGlContextGlThreadEnabled = 0x50 + 176;

Handle g_glThreadHandle = INVALID_HANDLE;  // Mesa's GL thread, for its CPU time in the stats

void gl_thread_started(void*, void*) {  // on Mesa's worker thread, before it runs any GL command
    // above the game's threads (0x3B), just below audio and the render thread (0x2C): a long driver
    // batch must not delay audio
    g_glThreadHandle = threadGetCurHandle();
    svcSetThreadPriority(threadGetCurHandle(), 0x2E);
    host::place_thread(0);  // preferring core 0 (the render thread prefers 2); never the main thread's core 1 with WWHD_CORE_LAYOUT
    LOG("[gl] GL thread running (priority 0x2E)");
}

void start_gl_thread() {
    const char* e = getenv("WWHD_GL_THREAD");
    if (!e || !*e || !strcmp(e, "0")) return;
#if defined(WWHD_MESA_STATS)
    if (!mesa_switch_glthread_start(reinterpret_cast<void (*)(st_context_iface*, util_queue_monitoring*)>(&gl_thread_started))) {
        LOG("[gl] GL thread: could not be started (mesa_switch_glthread_start)");
        return;
    }
    g_glThreadCtx = _glapi_get_context();
    LOG("[gl] GL thread started (WWHD_GL_THREAD, mesa_switch_glthread_start)");
    return;
#endif
    auto* ctx = static_cast<uint8_t*>(_glapi_get_context());
    auto* st = ctx ? *reinterpret_cast<uint8_t**>(ctx + kGlContextSt) : nullptr;
    auto* manager = st ? *reinterpret_cast<uint8_t**>(st) : nullptr;
    auto** slot = manager ? reinterpret_cast<void**>(manager + kManagerSetBackgroundContext) : nullptr;
    // the manager's first field is the pipe_screen: a pointer, as a sanity check of the offsets
    if (!slot || !*reinterpret_cast<void**>(manager)) {
        LOG("[gl] GL thread: Mesa's context layout is not the expected one; not started");
        return;
    }
    if (!*slot) *slot = reinterpret_cast<void*>(&gl_thread_started);
    _mesa_glthread_init(ctx);
    if (!ctx[kGlContextGlThreadEnabled]) {
        LOG("[gl] GL thread: could not be started (Mesa's queue or dispatch table was not created)");
        return;
    }
    g_glThreadCtx = ctx;
    LOG("[gl] GL thread started (WWHD_GL_THREAD)");
}
#endif

// waits until the GL thread has run every recorded command (before calls that use the driver directly)
void finish_gl_thread() {
#ifdef __SWITCH__
    if (g_glThreadCtx) {
        ScopedTime wait{R.perf.glThreadWaitNs};
        _mesa_glthread_finish(g_glThreadCtx);
    }
#endif
}

void init() {
    init_egl();
    init_objects();
    load_shader_cache(shader_cache_progress);
    cpuLoad.start();
#ifdef __SWITCH__
    start_gl_thread();  // last: the render thread takes the context next
#endif
    // the GX2 render thread takes the context (make_current)
    eglMakeCurrent(R.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    input::init();
}

void run_main_loop() {
#ifdef __SWITCH__
    while (appletMainLoop()) {
#else
    const char* exitAt = getenv("WWHD_EXIT_AT_FRAME");
    const uint64_t exitFrame = exitAt ? strtoull(exitAt, nullptr, 10) : 0;
    for (;;) {
        if (exitFrame && std::atomic_ref<uint64_t>(R.frame).load() >= exitFrame) break;
#endif
        input::update();
#ifdef __SWITCH__
        hostui::run_posted();  // the settings overlay's changes, on this thread as on the desktop hosts
        switch_settings::tick();  // CPU / GPU clock overrides set again when the system changed them
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    LOG("[boot] host loop ended at frame %llu", (unsigned long long)std::atomic_ref<uint64_t>(R.frame).load());
    fflush(stderr);
    std::_Exit(0);
}

void copy_to_scan(uint32_t cb, uint32_t target) {
    flush_draws();
    make_current();
    ScopedTime timer{R.perf.scanNs};
    R.perf.scans++;
    Surface* src = surface_from_color_buffer(cb);
    if (g_traceFrame && target != 1) trace_event("GamePad scan copy of %s", trace_name(src).c_str());
    if (src && target != 1) {  // the GamePad picture has no screen on the Switch yet
        if (src->drcScanFrame == ~0ull)
            LOG("[gl] GamePad picture: buffer %08X, %ux%u, format %X", src->addr, src->width, src->height, src->format);
        src->drcScanFrame = R.frame;
    }
    if (target != 1) return;
    if (!src || src->fmt.depth) return;
    src->tvScanFrame = R.frame;
    if (src->derivedFrom) src->derivedFrom->readFrame = R.frame;  // the TV shows what it was made from
    R.scanCopies++;
    if (g_traceFrame) trace_event("TV scan copy of %s", trace_name(src).c_str());
    // noted only: presenting reads src itself unless something writes to it first (scan_flush)
    R.tvSource = R.scanSrc = src;
}

}  // namespace

// the noted TV picture copied after all: src is about to change before the picture is shown
void scan_flush() {
    Surface* src = R.scanSrc;
    if (!src) return;
    R.scanSrc = nullptr;
    R.perf.scanBlits++;
    auto& scan = R.tvScan;
    if (!scan || scan->width != src->width || scan->height != src->height || scan->pw != src->pw || scan->ph != src->ph ||
        scan->fmt.internal != src->fmt.internal) {
        if (scan) destroy_surface_texture(scan.get());
        scan = std::make_unique<Surface>();
        scan->width = src->width;
        scan->height = src->height;
        scan->format = src->format;
        scan->fmt = src->fmt;
        scan->gpuWritten = true;
        scan->scale = src->scale;  // presenting scales it to the window
        create_surface_texture(scan.get());
    }
    blit(src, 0, 0, src->width, src->height, scan.get(), 0, 0, scan->width, scan->height);
}
namespace {

// Frames that took much longer than the game's 33 ms are logged with what the render thread did in
// them ([hitch]), to find periodic stalls. R.perf is reset only inside present() (frame_stats), so
// the difference to the copy taken after the previous present covers exactly this frame.
void check_hitch(uint64_t now) {
    static uint64_t lastSwap = 0, lastDraws = 0, lastGameWait = 0, lastRenderWait = 0, lastFsCalls = 0, lastFsNs = 0,
                    lastUnderrun = 0;
    const Renderer::Perf& base = g_hitchBase;
    static int logged = 0;
    const uint64_t gameWait = gx2::game_sync_wait_ns(), renderWait = gx2::render_thread_wait_ns();
    uint64_t fsCalls, fsNs, underrun, dropped;
    fs_stats(fsCalls, fsNs);
    audio::stats(underrun, dropped);
    if (lastSwap && now - lastSwap > 55'000'000ull) {
        g_hitches++;
        if (logged < 300) {
            logged++;
            const auto& p = R.perf;
            const double frameMs = double(now - lastSwap) / 1e6;
            LOG("[hitch] frame %llu took %.0f ms: render thread busy %.0f ms, %llu draws (%.0f ms), game waited %.0f ms; "
                "stream wraps %llu, fence waits %.1f ms, shaders compiled %llu linked %llu (%.0f ms), texture uploads %llu "
                "(%.0f ms), clears/copies/invalidates %.1f ms, flushes %.1f ms, new shader states %llu; files %llu "
                "(%.0f ms), audio gap %.0f ms",
                (unsigned long long)R.frame, frameMs, frameMs - double(renderWait - lastRenderWait) / 1e6,
                (unsigned long long)(R.drawCount - lastDraws), double(p.drawNs - base.drawNs) / 1e6,
                double(gameWait - lastGameWait) / 1e6, (unsigned long long)(p.streamWraps - base.streamWraps),
                double(p.fenceWaitNs - base.fenceWaitNs) / 1e6, (unsigned long long)(p.compiled - base.compiled),
                (unsigned long long)(p.linked - base.linked), double(p.shaderNs - base.shaderNs) / 1e6,
                (unsigned long long)(p.uploads - base.uploads), double(p.uploadNs - base.uploadNs) / 1e6,
                double((p.clearNs + p.surfaceCopyNs + p.invalidateNs + p.scanNs) -
                       (base.clearNs + base.surfaceCopyNs + base.invalidateNs + base.scanNs)) / 1e6,
                double(p.flushNs - base.flushNs) / 1e6, (unsigned long long)(p.shaders - base.shaders),
                (unsigned long long)(fsCalls - lastFsCalls), double(fsNs - lastFsNs) / 1e6,
                double(underrun - lastUnderrun) * 1000.0 / audio::kRate);
        }
    }
    lastFsCalls = fsCalls;
    lastFsNs = fsNs;
    lastUnderrun = underrun;
    lastSwap = now;
    lastDraws = R.drawCount;
    lastGameWait = gameWait;
    lastRenderWait = renderWait;
}

void swap() {
    make_current();
    uint64_t start = now_ns();
    check_hitch(start);
    present();
    R.perf.presentNs += now_ns() - start;
    g_hitchBase = R.perf;
    R.streamGen++;
    R.completed = std::atomic_ref<uint64_t>(R.frame).fetch_add(1) + 1;
    static const std::vector<uint64_t> traced = frame_list("WWHD_GL_TRACE_FRAMES", nullptr);
    if (g_captureRequested.exchange(false)) {
        g_captureFrame = R.frame + 1;
        g_probeStart = g_captureFrame + 1;  // then the probe frames (draw.cpp)
        LOG("[gl] capture of frame %llu requested: its passes follow, its pictures go to the SD card; probe frames "
            "from %llu", (unsigned long long)g_captureFrame, (unsigned long long)g_probeStart);
    }
    g_traceFrame = (!traced.empty() && std::find(traced.begin(), traced.end(), R.frame + 1) != traced.end()) ||
                   R.frame + 1 == g_captureFrame;
    g_captureDraws = R.frame + 1 == g_captureFrame;
}
}  // namespace

void request_capture() { g_captureRequested = true; }

void make_current() {
    static thread_local bool current = false;
    if (current) return;
    if (!eglMakeCurrent(R.display, R.surface, R.surface, R.context)) fatal("cannot make the OpenGL context current");
    glBindVertexArray(R.vao);
    current = true;
}

namespace {
struct UploadEntry {
    uint64_t key = 0, stamp = ~0ull, gen = 0;
    size_t size = 0;
    StreamSlice slice;
};
FrameTable<UploadEntry> uploadTable;
}  // namespace

StreamSlice stream_upload(const void* data, size_t size, size_t alignment, size_t zeroTail) {
    // alignment: a power of two, or (vertex rebasing in draw.cpp) a vertex stride
    GLintptr offset = (alignment & (alignment - 1)) == 0
                          ? (R.streamOffset + GLintptr(alignment) - 1) & ~GLintptr(alignment - 1)
                          : (R.streamOffset + GLintptr(alignment) - 1) / GLintptr(alignment) * GLintptr(alignment);
    if (size > kStreamSize) size = kStreamSize;
    if (zeroTail > kStreamSize - size) zeroTail = kStreamSize - size;
    const size_t total = size + zeroTail;
    const bool persistent = R.streamPtr[0] != nullptr;
    if (offset + GLintptr(total) > GLintptr(kStreamSize)) {
        // next buffer: slices already bound for this draw stay in the previous one
        R.streamGen++;
        if (persistent) {
            // the GPU may still read the next buffer. Each buffer remembers the last frame that wrote
            // to it; the per-frame fences (GpuBusy) say which frames the GPU finished, so normally
            // nothing waits. (A fence created here would make the render thread wait for Mesa's GL
            // thread to run everything queued: a stall several times a second.)
            R.streamIndex = (R.streamIndex + 1) % R.streams.size();
            const uint64_t lastUse = R.streamLastFrame[R.streamIndex];
            R.perf.streamWraps++;
            if (lastUse != ~0ull && lastUse > R.gpuDoneFrame) {
                ScopedTime wait{R.perf.fenceWaitNs};
                if (lastUse >= R.frame) {  // used earlier in this very frame: no frame fence yet
                    Stage stage("waiting for the GPU: stream buffer of this frame");
                    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 10'000'000'000ull);
                    glDeleteSync(f);
                } else
                    gpuBusy.wait_for(lastUse);
            }
        } else {
            // orphaning gives the next buffer fresh storage while the GPU finishes reading the old one
            R.streamIndex = (R.streamIndex + 1) % R.streams.size();
            glBindBuffer(GL_COPY_WRITE_BUFFER, R.streams[R.streamIndex]);
            glBufferData(GL_COPY_WRITE_BUFFER, kStreamSize, nullptr, GL_STREAM_DRAW);
        }
        offset = 0;
    }
    GLuint buffer = R.streams[R.streamIndex];
    R.streamLastFrame[R.streamIndex] = R.frame;
    if (persistent) {
        SampledTime copy{R.perf.copyNs, R.timedDraw};
        memcpy(R.streamPtr[R.streamIndex] + offset, data, size);
        if (zeroTail) memset(R.streamPtr[R.streamIndex] + offset + size, 0, zeroTail);
    } else {
        SampledTime copy{R.perf.copyNs, R.timedDraw};
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        void* p = glMapBufferRange(GL_COPY_WRITE_BUFFER, offset, total,
                                   GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (p) {
            memcpy(p, data, size);
            if (zeroTail) memset(static_cast<uint8_t*>(p) + size, 0, zeroTail);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
        }
    }
    R.streamOffset = offset + GLintptr(total);
    R.perf.streamBytes += total;
    return {buffer, offset};
}

// Guest vertex and uniform data may not change between GX2Invalidate calls (or GX2DrawDone) while
// a frame's draws can still read it, so an address uploaded once this frame is reused.
StreamSlice stream_guest(uint32_t addr, size_t size, size_t alignment, size_t zeroTail) {
    static const bool enabled = !getenv("WWHD_GL_NO_DEDUP");
    const uint64_t key = uint64_t(addr) | uint64_t(alignment) << 32 | uint64_t(zeroTail) << 48;
    // a frame's entries are told apart by stamp, so nothing is freed or allocated per frame
    const auto* e = uploadTable.find(key, R.frame);
    if (enabled && e->stamp == R.frame && e->gen == R.streamGen && e->size >= size) {
        R.perf.reusedBytes += size;
        return e->slice;
    }
    StreamSlice slice = stream_upload(mem::ptr(addr), size, alignment, zeroTail);
    uploadTable.put({key, R.frame, R.streamGen, size, slice});  // after the upload: a buffer switch advances the generation
    return slice;
}
PictureGrade picture_grade_now() { return picture_grade(); }
void set_picture_grade(const PictureGrade& g) {
    {
        std::lock_guard<std::mutex> lk(g_gradeMu);
        g_gradeSet = g;
        g_gradeChanged = true;
    }
    g_gradeGen.fetch_add(1, std::memory_order_release);
}
int fps_overlay_mode() { return overlay_mode(); }
void set_fps_overlay_mode(int mode) { g_fpsMode = std::clamp(mode, 0, 2); }
float dynamic_res_scale() { return dynamicRes.on ? dynamicRes.scale : 1.0f; }
#ifdef __SWITCH__
std::string clock_report_now() { return clock_report(); }
#endif

}  // namespace gfxgl

namespace render {
namespace {
// a GX2 command the renderer cannot handle is skipped (and reported once) rather than ending the game
template <class F> void guarded(const char* what, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        static int reported = 0;
        if (reported++ < 50) LOG("[gl] %s skipped: %s", what, e.what());
    }
}
}  // namespace
const Backend& opengl_backend() {
    static const Backend b = [] {
        Backend b{};
        b.api = Api::OpenGL;
        b.init = gfxgl::init;
        b.run_main_loop = gfxgl::run_main_loop;
        b.draw = [](const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                    uint32_t baseVertex, uint32_t instances) {
            guarded("draw", [&] { gfxgl::draw(regs, prim, count, indexType, indexAddr, baseVertex, instances); });
        };
        b.clear_color = [](const uint32_t* regs, uint32_t cb, const float rgba[4]) {
            guarded("color clear", [&] { gfxgl::clear_color(regs, cb, rgba); });
        };
        b.clear_depth_stencil = [](const uint32_t* regs, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
            guarded("depth clear", [&] { gfxgl::clear_depth_stencil(regs, db, depth, stencil, flags); });
        };
        b.copy_surface = [](uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip,
                            uint32_t dstSlice) {
            guarded("surface copy", [&] { gfxgl::copy_surface(src, srcMip, srcSlice, dst, dstMip, dstSlice); });
        };
        b.copy_to_scan = [](uint32_t cb, uint32_t target) {
            guarded("scan copy", [&] { gfxgl::copy_to_scan(cb, target); });
        };
        b.swap = gfxgl::swap;
        b.set_frame_aspect = [](float) {};
        b.target_aspect_factors = [](uint32_t, uint32_t, float& kx, float& ky) {
            kx = ky = 1.0f;
            return false;
        };
        b.frames_completed = [] { return gfxgl::R.completed.load(); };
        b.with_autorelease_pool = [](void (*fn)()) { fn(); };
        b.set_tv_format = [](uint32_t format, bool tv) {
            if (tv) gfxgl::R.tvSrgb = (format & 0x400) != 0;
        };
        b.invalidate = gfxgl::invalidate;
        b.guest_flush = [] {
            gfxgl::make_current();
            gfxgl::ScopedTime t{gfxgl::R.perf.flushNs};
            gfxgl::R.perf.flushes++;
            glFlush();
        };
        // GX2DrawDone: the game only needs the commands executed (guest memory is read when they run);
        // nothing the GPU renders is written back to guest memory, so the GPU itself is not waited for
        b.wait_idle = [] {
            gfxgl::make_current();
            gfxgl::R.streamGen++;
            gfxgl::ScopedTime t{gfxgl::R.perf.flushNs};
            gfxgl::R.perf.flushes++;
            glFlush();
        };
        b.ss_reset = [] {
            gfxgl::ss_reset_surfaces();
            gfxgl::reset_shader_memoization();
        };
        b.frame_count = [] { return std::atomic_ref<uint64_t>(gfxgl::R.frame).load(); };
        b.request_tv_dump = [](const std::string&, int) {};
        b.request_capture = [] {};
        b.shutdown = [] {};
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

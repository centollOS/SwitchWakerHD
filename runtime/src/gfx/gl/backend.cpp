// The OpenGL renderer's device, presentation and host loop, and its entry in the renderer table.
// Switch: EGL on the default NWindow, libnx applet loop and controllers.
// Elsewhere (WWHD_HEADLESS_GL, a debugging aid): a surfaceless Mesa EGL context renders into an
// offscreen "window"; WWHD_DUMP_FRAMES / WWHD_DUMP_TARGETS write PNGs, WWHD_EXIT_AT_FRAME quits.
#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#endif

#include "gl.h"
#include <EGL/eglext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gfx/renderer.h"
#include "gx2/gx2.h"
#include "input.h"
#include "platform/host.h"
#include "platform/input_switch.h"
#include "runtime.h"
#include "shaders.h"

namespace gx2 { uint64_t flips_presented(); }

namespace gfxgl {
Renderer R;

namespace {
constexpr size_t kStreamSize = 32u << 20;

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

// WWHD_DUMP_FRAMES=n,...: the TV picture and the window; WWHD_DUMP_TARGETS=n,...: every color target
void frame_dumps(uint64_t frame) {
    static const std::vector<uint64_t> frames = frame_list("WWHD_DUMP_FRAMES", nullptr);
    static const std::vector<uint64_t> targets = frame_list("WWHD_DUMP_TARGETS", nullptr);
    if (std::find(frames.begin(), frames.end(), frame) != frames.end()) {
        std::string n = std::to_string(frame);
        if (R.tvScan) dump_surface(R.tvScan.get(), "frame_" + n + ".png");
        dump_framebuffer(R.windowFbo, R.windowW, R.windowH, "frame_" + n + "_window.png");
    }
    if (std::find(targets.begin(), targets.end(), frame) != targets.end()) {
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
}

// How far the GPU runs behind the render thread: at each swap the GPU clock is read (the time the
// commands are submitted) and a timestamp query records when the GPU reaches that point. A lag near
// zero means the GPU waits for the CPU (CPU-bound); a lag of a frame or more means it is GPU-bound.
struct GpuLag {
    static constexpr int kQueries = 8;
    GLuint query[kQueries] = {};
    GLint64 submitted[kQueries] = {};
    bool pending[kQueries] = {};
    bool supported = true;
    double totalMs = 0;
    uint64_t samples = 0;
    void frame() {
        if (!supported) return;
        if (!query[0]) glGenQueries(kQueries, query);
        for (int i = 0; i < kQueries; i++) {
            if (!pending[i]) continue;
            GLint available = 0;
            glGetQueryObjectiv(query[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) continue;
            GLuint64 reached = 0;
            glGetQueryObjectui64v(query[i], GL_QUERY_RESULT, &reached);
            pending[i] = false;
            if (reached >= GLuint64(submitted[i])) {
                totalMs += double(reached - GLuint64(submitted[i])) / 1e6;
                samples++;
            }
        }
        for (int i = 0; i < kQueries; i++)
            if (!pending[i]) {
                GLint64 now = 0;
                glGetInteger64v(GL_TIMESTAMP, &now);
                if (!now) {  // no GPU clock: report nothing
                    supported = false;
                    return;
                }
                submitted[i] = now;
                glQueryCounter(query[i], GL_TIMESTAMP);
                pending[i] = true;
                return;
            }
    }
    // mean lag since the last call, or -1 without samples
    double take() {
        double mean = samples ? totalMs / double(samples) : -1;
        totalMs = 0;
        samples = 0;
        return mean;
    }
} gpuLag;

struct OverlayStats {
    double fps = 0, renderBusy = -1, gpuLagMs = -1, draws = 0;
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
    uint32_t errors = 0;
    GLenum firstError = GL_NO_ERROR;
    for (GLenum e; errors < 64 && (e = glGetError()) != GL_NO_ERROR; errors++)
        if (!firstError) firstError = e;
    auto& p = R.perf;
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / secs; };
    std::string memory;
#ifdef __SWITCH__
    // the process's heap is reserved up front: what malloc has handed out is the real use
    struct mallinfo heap = mallinfo();
    memory = ", heap " + std::to_string(size_t(heap.uordblks) >> 20) + "/" + std::to_string(size_t(heap.arena) >> 20) + " MiB";
#endif
    static uint64_t lastWait = 0;
    uint64_t wait = gx2::render_thread_wait_ns();
    double busy = 1000.0 - ms(wait - lastWait);
    lastWait = wait;
    double lag = gpuLag.take();
    uint64_t draws = R.drawCount - lastDraws;
    double perFrame = R.frame > lastFrame ? double(draws) / double(R.frame - lastFrame) : 0.0;
    LOG("[gl] %.1f fps, %.0f draws/frame, %llu skipped, GL errors %u (first 0x%X); render thread busy %.0f ms/s, "
        "GPU lag %.1f ms; draws %.0f ms/s (lookup %.0f, indices %.0f, resources %.0f, state %.0f, submit %.0f; "
        "%.0f%% shader memo hits; shaders %.0f: %llu new states, %llu compiled, %llu linked; texture uploads %.0f, %llu), "
        "present %.0f; streamed %.1f MB/s (reused %.1f; per frame: uniforms %.1f MB, indices %.1f MB, vertices %.1f MB); "
        "%zu surfaces%s",
        double(R.frame - lastFrame) / secs, perFrame, (unsigned long long)R.skippedDraws, errors, firstError, busy, lag,
        ms(p.drawNs), ms(p.lookupNs), ms(p.indexNs), ms(p.resourceNs), ms(p.stateNs), ms(p.submitNs),
        draws ? 100.0 * double(p.memoHits) / double(draws) : 0.0, ms(p.shaderNs), (unsigned long long)p.shaders,
        (unsigned long long)p.compiled, (unsigned long long)p.linked, ms(p.uploadNs), (unsigned long long)p.uploads,
        ms(p.presentNs), double(p.streamBytes) / 1e6 / secs, double(p.reusedBytes) / 1e6 / secs,
        R.frame > lastFrame ? double(p.uboBytes) / 1e6 / double(R.frame - lastFrame) : 0.0,
        R.frame > lastFrame ? double(p.indexBytes) / 1e6 / double(R.frame - lastFrame) : 0.0,
        R.frame > lastFrame ? double(p.vertexBytes) / 1e6 / double(R.frame - lastFrame) : 0.0, R.surfaces.size(),
        memory.c_str());
    overlayStats.renderBusy = busy;
    overlayStats.gpuLagMs = lag;
    overlayStats.draws = perFrame;
    p = {};
    static const bool means = getenv("WWHD_GL_STATS") != nullptr;
    if (means) {
        float mean[4] = {}, window[4] = {};
        if (R.tvScan) surface_mean(R.tvScan.get(), mean);
        framebuffer_mean(R.windowFbo, R.windowW, R.windowH, window);
        LOG("[gl] TV mean %.3f %.3f %.3f, window mean %.3f %.3f %.3f", mean[0], mean[1], mean[2], window[0], window[1],
            window[2]);
    }
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
            "in vec2 uv;\n"
            "out vec4 color;\n"
            "void main() { color = vec4(texture(scan, uv).rgb, 1.0); }\n";
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
int overlay_mode() {
    static const int mode = [] {
        const char* e = getenv("WWHD_FPS");
        return e && *e ? atoi(e) : 1;
    }();
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
        if (s.gpuLagMs >= 0) snprintf(lines[rows++], sizeof lines[0], "GPU LAG %.1fMS", s.gpuLagMs);
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
    glUniform1iv(glGetUniformLocation(prog, "glyph"), columns * rows, glyphs);
    glUniform2i(glGetUniformLocation(prog, "grid"), columns, rows);
    glUniform3f(glGetUniformLocation(prog, "box"), float(margin), float(wh - margin), float(scale));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisablei(GL_BLEND, 0);
    (void)ww;
}

// TV scan buffer -> window: fit 16:9, flip rows (guest images keep row 0 at the top)
void present() {
    forget_gl_state();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.windowFbo);
#ifdef __SWITCH__
    glDrawBuffer(GL_BACK);
#endif
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_RASTERIZER_DISCARD);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    EGLint ww = R.windowW, wh = R.windowH;
#ifdef __SWITCH__
    // switch-mesa leaves the EGL surface size at 0x0: ask the native window
    u32 nw = 0, nh = 0;
    if (R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &nw, &nh)) && nw && nh) {
        ww = EGLint(nw);
        wh = EGLint(nh);
    }
#endif
    if (Surface* scan = R.tvScan.get()) {
        float a = float(scan->width) / scan->height;
        int w = ww, h = int(ww / a);
        if (h > wh) { h = wh; w = int(wh * a); }
        int x = (ww - w) / 2, y = (wh - h) / 2;
        GLuint prog = scan->target == GL_TEXTURE_2D ? present_program() : 0;
        if (prog) {
            glDisablei(GL_BLEND, 0);
            glDisable(GL_COLOR_LOGIC_OP);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_POLYGON_OFFSET_FILL);
            if (auto cc = clip_control()) cc(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
            glViewportIndexedf(0, float(x), float(y), float(w), float(h));
            glDepthRangef(0, 1);
            glUseProgram(prog);
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
    draw_overlay(ww, wh);
    frame_dumps(R.frame + 1);
    frame_stats();
#ifdef __SWITCH__
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    eglSwapBuffers(R.display, R.surface);
#endif
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

void init() {
    init_egl();
    init_objects();
    load_shader_cache(shader_cache_progress);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    LOG("[boot] host loop ended at frame %llu", (unsigned long long)std::atomic_ref<uint64_t>(R.frame).load());
    fflush(stderr);
    std::_Exit(0);
}

void copy_to_scan(uint32_t cb, uint32_t target) {
    make_current();
    if (target != 1) return;  // the GamePad picture has no screen on the Switch yet
    Surface* src = surface_from_color_buffer(cb);
    if (!src || src->fmt.depth) return;
    R.scanCopies++;
    auto& scan = R.tvScan;
    if (!scan || scan->width != src->width || scan->height != src->height || scan->fmt.internal != src->fmt.internal) {
        if (scan) destroy_surface_texture(scan.get());
        scan = std::make_unique<Surface>();
        scan->width = src->width;
        scan->height = src->height;
        scan->format = src->format;
        scan->fmt = src->fmt;
        scan->gpuWritten = true;
        create_surface_texture(scan.get());
    }
    blit(src, 0, 0, src->width, src->height, scan.get(), 0, 0, scan->width, scan->height);
}

void swap() {
    make_current();
    uint64_t start = now_ns();
    gpuLag.frame();
    present();
    R.perf.presentNs += now_ns() - start;
    R.streamGen++;
    R.completed = std::atomic_ref<uint64_t>(R.frame).fetch_add(1) + 1;
}
}  // namespace

void make_current() {
    static thread_local bool current = false;
    if (current) return;
    if (!eglMakeCurrent(R.display, R.surface, R.surface, R.context)) fatal("cannot make the OpenGL context current");
    glBindVertexArray(R.vao);
    current = true;
}

StreamSlice stream_upload(const void* data, size_t size, size_t alignment) {
    GLintptr offset = (R.streamOffset + GLintptr(alignment) - 1) & ~GLintptr(alignment - 1);
    if (size > kStreamSize) size = kStreamSize;
    const bool persistent = R.streamPtr[0] != nullptr;
    if (offset + GLintptr(size) > GLintptr(kStreamSize)) {
        // next buffer: slices already bound for this draw stay in the previous one
        R.streamGen++;
        if (persistent) {
            // the GPU may still read the next buffer: wait for the fence set when it was left
            R.streamFence[R.streamIndex] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            R.streamIndex = (R.streamIndex + 1) % R.streams.size();
            if (GLsync fence = R.streamFence[R.streamIndex]) {
                glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 10'000'000'000ull);
                glDeleteSync(fence);
                R.streamFence[R.streamIndex] = nullptr;
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
    if (persistent)
        memcpy(R.streamPtr[R.streamIndex] + offset, data, size);
    else {
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        void* p = glMapBufferRange(GL_COPY_WRITE_BUFFER, offset, size,
                                   GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (p) {
            memcpy(p, data, size);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
        }
    }
    R.streamOffset = offset + GLintptr(size);
    R.perf.streamBytes += size;
    return {buffer, offset};
}

// Guest vertex and uniform data may not change between GX2Invalidate calls (or GX2DrawDone) while
// a frame's draws can still read it, so an address uploaded once this frame is reused.
StreamSlice stream_guest(uint32_t addr, size_t size, size_t alignment) {
    struct Entry {
        uint64_t gen = 0;
        size_t size = 0;
        StreamSlice slice;
    };
    static std::unordered_map<uint64_t, Entry> uploaded;
    static uint64_t clearedFrame = ~0ull;
    static const bool enabled = !getenv("WWHD_GL_NO_DEDUP");
    const uint64_t key = uint64_t(addr) | uint64_t(alignment) << 32;
    if (clearedFrame != R.frame) {
        uploaded.clear();
        clearedFrame = R.frame;
    }
    if (auto it = uploaded.find(key);
        enabled && it != uploaded.end() && it->second.gen == R.streamGen && it->second.size >= size) {
        R.perf.reusedBytes += size;
        return it->second.slice;
    }
    StreamSlice slice = stream_upload(mem::ptr(addr), size, alignment);
    uploaded[key] = {R.streamGen, size, slice};  // after the upload: moving to the next buffer advances the generation
    return slice;
}
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
        b.set_tv_format = [](uint32_t, bool) {};
        b.invalidate = gfxgl::invalidate;
        b.guest_flush = [] {
            gfxgl::make_current();
            glFlush();
        };
        // GX2DrawDone: the game only needs the commands executed (guest memory is read when they run);
        // nothing the GPU renders is written back to guest memory, so the GPU itself is not waited for
        b.wait_idle = [] {
            gfxgl::make_current();
            gfxgl::R.streamGen++;
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

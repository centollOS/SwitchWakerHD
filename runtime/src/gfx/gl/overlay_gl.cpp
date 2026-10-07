// The settings overlay's Dear ImGui draw data, drawn by the OpenGL renderer over the TV picture in
// present() (Switch). Its own vertex array and program, so the draw path's state cache and vertex array
// (R.vao) are left as they were; present() forgets the GL state after the swap anyway. Vertices and
// indices go through the stream buffer (a glBufferSubData over 8 KB made Mesa's GL thread wait).
#include "gl.h"
#include "settings.h"

#include <cstring>
#include <vector>

#include "imgui.h"
#include "runtime.h"

namespace gfxgl {
namespace {

const char* kVertex = R"glsl(#version 330 core
layout(location=0) in vec2 pos;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 col;
uniform vec4 xform;  // scale.xy, translate.xy
out vec4 color;
out vec2 tc;
void main() {
 color=col;
 tc=uv;
 gl_Position=vec4(pos*xform.xy+xform.zw,0.0,1.0);
}
)glsl";
const char* kFragment = R"glsl(#version 330 core
uniform sampler2D image;
in vec4 color;
in vec2 tc;
layout(location=0) out vec4 result;
void main() { result=color*texture(image,tc); }
)glsl";

struct Resources {
    GLuint program = 0, vao = 0, sampler = 0;
    GLint xformLoc = -1, imageLoc = -1;
    bool failed = false;
};
Resources res;

GLuint compile(GLenum type, const char* text) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOG("[overlay] GL shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool setup() {
    if (res.program) return true;
    if (res.failed) return false;
    res.failed = true;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) return false;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOG("[overlay] GL program does not link; no settings overlay");
        glDeleteProgram(p);
        return false;
    }
    res.program = p;
    res.xformLoc = glGetUniformLocation(p, "xform");
    res.imageLoc = glGetUniformLocation(p, "image");
    glGenVertexArrays(1, &res.vao);
    glBindVertexArray(res.vao);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(R.vao);
    glGenSamplers(1, &res.sampler);
    glSamplerParameteri(res.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(res.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(res.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(res.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    res.failed = false;
    LOG("[overlay] OpenGL overlay renderer ready");
    return true;
}

// ImGui 1.92 texture protocol: create, update (whole texture) and destroy font atlas pages
void update_texture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        GLuint t = (GLuint)(uintptr_t)tex->GetTexID();
        if (t) glDeleteTextures(1, &t);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (tex->Status != ImTextureStatus_WantCreate && tex->Status != ImTextureStatus_WantUpdates) return;
    if (tex->Format != ImTextureFormat_RGBA32) {
        LOG("[overlay] unexpected texture format %d", (int)tex->Format);
        tex->SetStatus(ImTextureStatus_OK);
        return;
    }
    GLuint t = (GLuint)(uintptr_t)tex->GetTexID();
    glActiveTexture(GL_TEXTURE0 + R.scratchUnit);
    if (tex->Status == ImTextureStatus_WantCreate || !t) {
        if (t) glDeleteTextures(1, &t);
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex->Width, tex->Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        tex->SetTexID((ImTextureID)(uintptr_t)t);
    } else {
        glBindTexture(GL_TEXTURE_2D, t);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex->Width, tex->Height, GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixels());
    glBindTexture(GL_TEXTURE_2D, 0);
    tex->SetStatus(ImTextureStatus_OK);
}

}  // namespace

void overlay_renderer_init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "wwhd_opengl";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
}

// into the bound draw framebuffer (the window), ww x wh pixels, top-left origin like ImGui
void overlay_draw(ImDrawData* d, int ww, int wh) {
    if (!d || d->DisplaySize.x <= 0 || d->DisplaySize.y <= 0 || !setup()) return;
    if (d->Textures)
        for (ImTextureData* tex : *d->Textures)
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);
    if (d->TotalVtxCount <= 0) return;
    const float sx = ww / d->DisplaySize.x, sy = wh / d->DisplaySize.y;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_RASTERIZER_DISCARD);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (auto cc = clip_control()) cc(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glViewportIndexedf(0, 0, 0, float(ww), float(wh));
    glDepthRangef(0, 1);
    glEnablei(GL_BLEND, 0);
    glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(res.program);
    // ImGui's y grows downwards; GL's window y upwards
    const float scaleX = 2.0f / d->DisplaySize.x, scaleY = -2.0f / d->DisplaySize.y;
    glUniform4f(res.xformLoc, scaleX, scaleY, -1.0f - d->DisplayPos.x * scaleX, 1.0f - d->DisplayPos.y * scaleY);
    glUniform1i(res.imageLoc, R.scratchUnit);
    glActiveTexture(GL_TEXTURE0 + R.scratchUnit);
    glBindSampler(R.scratchUnit, res.sampler);
    // all lists' vertices, then all indices, into the stream buffer (one slice each)
    std::vector<uint8_t> vtx(size_t(d->TotalVtxCount) * sizeof(ImDrawVert)), idx(size_t(d->TotalIdxCount) * sizeof(ImDrawIdx));
    size_t vOff = 0, iOff = 0;
    for (const ImDrawList* l : d->CmdLists) {
        memcpy(vtx.data() + vOff, l->VtxBuffer.Data, size_t(l->VtxBuffer.Size) * sizeof(ImDrawVert));
        memcpy(idx.data() + iOff, l->IdxBuffer.Data, size_t(l->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vOff += size_t(l->VtxBuffer.Size) * sizeof(ImDrawVert);
        iOff += size_t(l->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }
    const StreamSlice vs = stream_upload(vtx.data(), vtx.size(), 16), is = stream_upload(idx.data(), idx.size(), 16);
    glBindVertexArray(res.vao);
    glBindBuffer(GL_ARRAY_BUFFER, vs.buffer);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)(vs.offset + offsetof(ImDrawVert, pos)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)(vs.offset + offsetof(ImDrawVert, uv)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void*)(vs.offset + offsetof(ImDrawVert, col)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, is.buffer);
    const GLenum indexType = sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    GLuint bound = ~0u;
    int vtxBase = 0, idxBase = 0;
    for (const ImDrawList* l : d->CmdLists) {
        for (const ImDrawCmd& c : l->CmdBuffer) {
            if (c.UserCallback) continue;
            float x0 = (c.ClipRect.x - d->DisplayPos.x) * sx, y0 = (c.ClipRect.y - d->DisplayPos.y) * sy;
            float x1 = (c.ClipRect.z - d->DisplayPos.x) * sx, y1 = (c.ClipRect.w - d->DisplayPos.y) * sy;
            x0 = std::max(x0, 0.0f);
            y0 = std::max(y0, 0.0f);
            x1 = std::min(x1, float(ww));
            y1 = std::min(y1, float(wh));
            if (x1 <= x0 || y1 <= y0) continue;
            GLuint t = (GLuint)(uintptr_t)c.GetTexID();
            if (!t) continue;
            if (t != bound) {
                glBindTexture(GL_TEXTURE_2D, t);
                bound = t;
            }
            glScissor(int(x0), int(wh - y1), int(x1 - x0), int(y1 - y0));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(c.ElemCount), indexType,
                                     (void*)(uintptr_t(is.offset) + uintptr_t((c.IdxOffset + idxBase) * sizeof(ImDrawIdx))),
                                     GLint(c.VtxOffset + vtxBase));
        }
        vtxBase += l->VtxBuffer.Size;
        idxBase += l->IdxBuffer.Size;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisablei(GL_BLEND, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindSampler(R.scratchUnit, 0);
    glBindVertexArray(R.vao);
}

}  // namespace gfxgl

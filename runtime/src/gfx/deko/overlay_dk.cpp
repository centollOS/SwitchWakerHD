// The settings overlay's Dear ImGui draw data, drawn by the deko3d renderer over the picture in the
// present pass (port of gfx/gl/overlay_gl.cpp). Textures (ImGui 1.92's protocol: the font atlas pages)
// live in the image heap with a descriptor each (slots 1-63 of the image descriptor set) and are drawn
// with sampler 0; vertices, indices, the transform and texture uploads go through the stream ring.
#include "dk.h"

#include <cstring>
#include <vector>

#include "imgui.h"
#include "runtime.h"

namespace gfxdk {
namespace {

constexpr uint32_t kFirstSlot = 1, kSlots = 64;  // image descriptor slots for the overlay's textures
struct Texture {
    bool used = false;
    DkImage image;
    ImageAlloc mem;
};
Texture g_tex[kSlots];

void destroy_texture(uint32_t slot) {
    if (slot < kFirstSlot || slot >= kSlots || !g_tex[slot].used) return;
    image_free_later(g_tex[slot].mem);  // the GPU may still draw with it this frame
    g_tex[slot].used = false;
}

// ImGui 1.92 texture protocol: create, update (the whole texture) and destroy font atlas pages
void update_texture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        destroy_texture(uint32_t(tex->GetTexID()));
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
    uint32_t slot = uint32_t(tex->GetTexID());
    if (tex->Status == ImTextureStatus_WantCreate || slot < kFirstSlot || slot >= kSlots || !g_tex[slot].used) {
        destroy_texture(slot);
        slot = 0;
        for (uint32_t i = kFirstSlot; i < kSlots && !slot; i++)
            if (!g_tex[i].used) slot = i;
        if (!slot) {
            LOG("[overlay] deko3d: no free texture slot for a %dx%d texture", tex->Width, tex->Height);
            tex->SetStatus(ImTextureStatus_OK);
            return;
        }
        Texture& t = g_tex[slot];
        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, R.device);
        m.format = DkImageFormat_RGBA8_Unorm;
        m.dimensions[0] = uint32_t(tex->Width);
        m.dimensions[1] = uint32_t(tex->Height);
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &m);
        t.mem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
        dkImageInitialize(&t.image, &layout, t.mem.block, t.mem.offset);
        t.used = true;
        DkImageView view;
        dkImageViewDefaults(&view, &t.image);
        DkImageDescriptor d;
        dkImageDescriptorInitialize(&d, &view, false, false);
        dkCmdBufPushData(R.cmd, image_descriptors() + slot * sizeof(DkImageDescriptor), &d, sizeof d);
        tex->SetTexID(ImTextureID(slot));
        LOG("[overlay] deko3d texture %u: %dx%d", slot, tex->Width, tex->Height);
    }
    // the pixels through the stream ring, copied into the image by the GPU
    const uint32_t bytes = uint32_t(tex->Width) * uint32_t(tex->Height) * 4;
    StreamAlloc s = stream_alloc(bytes, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    if (!s) return;  // logged; tried again next frame (the status stays)
    memcpy(s.cpu, tex->GetPixels(), bytes);
    DkImageView view;
    dkImageViewDefaults(&view, &g_tex[slot].image);
    const DkCopyBuf src = {s.gpu, 0, 0};
    const DkImageRect rect = {0, 0, 0, uint32_t(tex->Width), uint32_t(tex->Height), 1};
    dkCmdBufCopyBufferToImage(R.cmd, &src, &view, &rect, 0);
    // the copy and the descriptor done before any draw samples the texture
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    tex->SetStatus(ImTextureStatus_OK);
}

}  // namespace

void overlay_renderer_init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "wwhd_deko3d";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    LOG("[overlay] deko3d overlay renderer ready");
}

// into the bound render target (the swapchain image), ww x wh pixels, top-left origin like ImGui
void overlay_draw(ImDrawData* d, int ww, int wh) {
    if (!d || d->DisplaySize.x <= 0 || d->DisplaySize.y <= 0) return;
    const DkShader* vs = shader(kImguiVs);
    const DkShader* fs = shader(kImguiFs);
    if (!vs || !fs) return;
    if (d->Textures)
        for (ImTextureData* tex : *d->Textures)
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);
    if (d->TotalVtxCount <= 0) return;
    // all lists' vertices, then all indices, into the stream ring
    const uint32_t vBytes = uint32_t(d->TotalVtxCount) * sizeof(ImDrawVert), iBytes = uint32_t(d->TotalIdxCount) * sizeof(ImDrawIdx);
    StreamAlloc vtx = stream_alloc(vBytes, 16), idx = stream_alloc(iBytes, 16), ubo = stream_alloc(256, DK_UNIFORM_BUF_ALIGNMENT);
    if (!vtx || !idx || !ubo) return;
    size_t vOff = 0, iOff = 0;
    for (const ImDrawList* l : d->CmdLists) {
        memcpy(static_cast<uint8_t*>(vtx.cpu) + vOff, l->VtxBuffer.Data, size_t(l->VtxBuffer.Size) * sizeof(ImDrawVert));
        memcpy(static_cast<uint8_t*>(idx.cpu) + iOff, l->IdxBuffer.Data, size_t(l->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vOff += size_t(l->VtxBuffer.Size) * sizeof(ImDrawVert);
        iOff += size_t(l->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }
    // ImGui's pixels to normalized coordinates; y grows downwards in both (dk.h)
    const float xform[4] = {2.0f / d->DisplaySize.x, 2.0f / d->DisplaySize.y, -1.0f - d->DisplayPos.x * 2.0f / d->DisplaySize.x,
                            -1.0f - d->DisplayPos.y * 2.0f / d->DisplaySize.y};
    memcpy(ubo.cpu, xform, sizeof xform);
    const DkShader* sh[] = {vs, fs};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_GraphicsMask, sh, 2);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(R.cmd, &rs);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, true);
    dkCmdBufBindColorState(R.cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkBlendStateSetFactors(&bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
    dkCmdBufBindBlendStates(R.cmd, 0, &bs, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = false;
    ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(R.cmd, &ds);
    const DkViewport vp = {0, 0, float(ww), float(wh), 0.0f, 1.0f};
    dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
    const DkBufExtents u = {ubo.gpu, 256};
    dkCmdBufBindUniformBuffers(R.cmd, DkStage_Vertex, 0, &u, 1);
    static const DkVtxAttribState attribs[] = {
        DkVtxAttribState{0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
    };
    static const DkVtxBufferState buffers[] = {DkVtxBufferState{sizeof(ImDrawVert), 0}};
    dkCmdBufBindVtxAttribState(R.cmd, attribs, 3);
    dkCmdBufBindVtxBufferState(R.cmd, buffers, 1);
    const DkBufExtents vb = {vtx.gpu, vBytes};
    dkCmdBufBindVtxBuffers(R.cmd, 0, &vb, 1);
    dkCmdBufBindIdxBuffer(R.cmd, sizeof(ImDrawIdx) == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, idx.gpu);
    const float sx = ww / d->DisplaySize.x, sy = wh / d->DisplaySize.y;
    uint32_t bound = 0;
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
            const uint32_t slot = uint32_t(c.GetTexID());
            if (slot < kFirstSlot || slot >= kSlots || !g_tex[slot].used) continue;
            if (slot != bound) {
                const DkResHandle h = dkMakeTextureHandle(slot, 0);
                dkCmdBufBindTextures(R.cmd, DkStage_Fragment, 0, &h, 1);
                bound = slot;
            }
            const DkScissor sc = {uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
            dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
            dkCmdBufDrawIndexed(R.cmd, DkPrimitive_Triangles, c.ElemCount, 1, c.IdxOffset + uint32_t(idxBase),
                                int32_t(c.VtxOffset) + vtxBase, 0);
        }
        vtxBase += l->VtxBuffer.Size;
        idxBase += l->IdxBuffer.Size;
    }
}

}  // namespace gfxdk

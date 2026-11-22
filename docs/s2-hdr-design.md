# Switch 2 HDR output: design note (s2/hdr-info)

Status (2026-11): not possible yet. libounce's VI2 only *reports* HDR capability
(`Vi2DisplayModeCapability.hdr_output_formats`, bits 0-1, formats unknown); there is no call to select an HDR
output mode, no colour-space / mastering metadata API (the `Vi2PresentOptions.metadata[0x20]` tail is "active
service input" whose layout is unknown), and drako3d's swapchain accepts RGBA8 / BGRA8 / RGBX8 / RGB10A2 only
("FP16/HDR swapchains (requires libounce to support them too)" in its README's future plans). The port therefore logs
the capability (`[s2] display HDR formats: ...`) and shows it read-only in the Switch 2 tab. It does not fake HDR
(no inverse tone mapping into an SDR buffer).

## What has to exist first

1. libounce: a VI2 call (or present-option bits) that selects an HDR output mode on the display, the meaning of the
   `hdr_output_formats` bits (likely HDR10/PQ and something else), and the HDR metadata layout in
   `Vi2PresentOptions.metadata`.
2. drako3d: swapchain images in `RGBA16_Float` (scRGB) and/or `RGB10A2_Unorm` tagged PQ / BT.2020, passed through to
   the VI2 image layout (`gsvMakeImageLayout` format).

## Plan for the port once they do

- Option "HDR output" (Switch 2 tab, default off; greyed out unless `hdr_output_formats != 0` and the swapchain
  format is available). Re-checked on every display event (dock / undock, TV change).
- Swapchain: recreate (as `resize_window` does for a size change) with the HDR format; the game's picture stays SDR
  (the Wii U renders in 8-bit sRGB-ish space), so HDR is purely an output transform in the present pass.
- Present pass (present_fsh.glsl): SDR -> linear (the game's gamma, about 2.2), BT.709 -> BT.2020 primaries, scale by
  a "paper white" setting (default 203 nits, slider 100-400), optional gentle highlight expansion of the brightest
  ~5 % (off by default: no invented detail), then PQ encode (RGB10A2) or write linear scRGB (FP16).
  The colour grade sliders (exposure etc.) keep working before the transform. FXAA / FSR run before it, in SDR.
- HUD / overlay (ImGui, FPS counter) drawn at paper white, after the transform.
- Metadata: MaxCLL = paper white x expansion peak, MaxFALL from the mean; BT.2020 primaries, D65.
- Screenshots / video clips: keep capturing the SDR picture before the transform.
- Fallback: if any step fails (swapchain creation, VI2 refusal), log it and return to RGBA8 SDR.

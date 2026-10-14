// The story notebook's own icon in the Delivery Bag window (story_notebook.cpp): where the port draws
// it over the notebook's bag place. Pure code, unit-tested in runtime/tools/story_notebook_test.cpp.
//
// The bag place is a menu item (hdMenuItem_c) whose layout panes hang in the menu's pane tree: the
// item's icon pane (N_ItemIcon_00) under the item's root pane, under the bag window's panes, up to the
// screen layout's root. A pane's place is its translation in its parent's space (scaled by the
// parent), y up, the screen layout's origin in the middle of the screen (the game's own
// hdItemPane_getGlobalPosition sums the same chain). The menu's animations (the window opening and
// closing, the slide, the cursor's highlight scale) move, scale and fade these panes, so the icon
// follows them by reading the chain every frame.
#pragma once
#include <cstdint>
#include <vector>

namespace story_notebook {

// one pane of the chain, the icon pane first, the screen layout's root last
struct PaneNode {
    float tx = 0, ty = 0;  // translation in the parent's space
    float sx = 1, sy = 1;  // scale
    bool visible = true;
};

struct IconPlace {
    bool shown = false;
    float x = 0, y = 0;  // the icon's center on the screen (HUD coordinates, y down)
    float size = 0;      // width and height
    float alpha = 1;
};

// The icon's place: the chain composed from the root down, mapped onto a screen of width x height
// whose layout has its origin in the middle. Not shown when any pane above the icon pane is hidden
// (the icon pane itself is the game's sentinel icon, which the port keeps hidden), or when the
// window has faded out. `base` is the icon's size at scale 1; `glb_alpha` the item root pane's
// global alpha (0..255, the window's fades).
inline IconPlace place_icon(const std::vector<PaneNode>& chain, float base, uint8_t glb_alpha, float width, float height) {
    IconPlace out;
    if (chain.empty() || !glb_alpha) return out;
    for (size_t i = 1; i < chain.size(); i++)
        if (!chain[i].visible) return out;
    float x = 0, y = 0, sx = 1, sy = 1;  // the icon pane's origin and scale in screen-layout space
    for (size_t i = chain.size(); i-- > 0;) {
        const PaneNode& p = chain[i];
        x += p.tx * sx, y += p.ty * sy;
        sx *= p.sx, sy *= p.sy;
    }
    if (!(sx > 0.01f && sy > 0.01f && sx < 100.0f && sy < 100.0f)) return out;  // also NaN: a pane being built
    const float scale = (sx + sy) * 0.5f;
    out.shown = true;
    out.x = width * 0.5f + x;
    out.y = height * 0.5f - y;
    out.size = base * scale;
    out.alpha = float(glb_alpha) / 255.0f;
    return out;
}

}  // namespace story_notebook

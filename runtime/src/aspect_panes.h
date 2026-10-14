// Pure pane policy shared by the guest hooks and their synthetic tests.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace aspect::panes {
struct Transform { float x, y, sx, sy; };

// ---- How a TV layout's panes follow another screen shape. kx, ky: how many times wider and taller
// the screen is than 16:9 (one of them is 1); 640*(kx-1) and 360*(ky-1) are the extra half width and
// half height. Every rule below is written in those terms, so one table holds at any aspect ratio.

// An anchor moves a pane with the screen edges: x -1 left, +1 right; y -1 bottom, +1 top; 0 stays.
struct Anchor { float x, y; };
inline constexpr Anchor kTop{0, 1}, kBottom{0, -1}, kTopLeft{-1, 1};
inline Transform shift(Anchor a, float x, float y, float sx, float sy, float kx, float ky) {
    return {x + a.x * 640 * (kx - 1), y + a.y * 360 * (ky - 1), sx, sy};
}
// The HUD's anchor: whichever edges the pane sits near, beyond 300 (200) units from the centre. By
// position rather than per pane, because the message window moves itself between its top and
// bottom places.
inline Anchor anchor_at(float x, float y) {
    return {x <= -300 ? -1.0f : x >= 300 ? 1.0f : 0.0f, y <= -200 ? -1.0f : y >= 200 ? 1.0f : 0.0f};
}
// A pane the game slides between two places with an animation, anchored differently at each: the
// anchor blends with its progress from y0 to y1.
struct Slide { float y0; Anchor a0; float y1; Anchor a1; };
inline Transform slide(const Slide& s, float x, float y, float sx, float sy, float kx, float ky) {
    float t = std::clamp((y - s.y0) / (s.y1 - s.y0), 0.0f, 1.0f);
    return shift({s.a0.x + t * (s.a1.x - s.a0.x), s.a0.y + t * (s.a1.y - s.a0.y)}, x, y, sx, sy, kx, ky);
}
// Full-screen panes stretch only as far as they must to cover the expanded screen: a 1280x720
// background by exactly kx, ky; a larger one less, or not at all while it still covers (the
// telescope's circular W_Wipe_00 is 1716x1716: its circle stays round at 16:10 and 21:9).
inline float cover(float k, float native, float size) {
    return size > 0 ? std::max(1.0f, k * std::min(1.0f, native / size)) : k;
}
// A bar framing the native screen from one side grows out to the new edge. Its origin sits on that
// edge, so scale grows from there. w, h: its scaled size.
enum class Side { Top, Bottom, Left, Right };
inline Transform extend(Side e, float x, float y, float sx, float sy, float w, float h, float kx, float ky) {
    Transform t{x,y,sx,sy};
    float dx = 640*(kx-1), dy = 360*(ky-1);
    if (e == Side::Top || e == Side::Bottom) {
        if (h > 0) t.sy *= (h + dy) / h;
        t.y += e == Side::Top ? dy : -dy;
        t.sx *= kx;  // centred, full width
    } else {
        if (w > 0) t.sx *= (w + dx) / w;
        t.x += e == Side::Right ? dx : -dx;
    }
    return t;
}

enum class Rule {
    Hud,        // follows the edges it sits near (anchor_at)
    Anchored,   // follows fixed edges (PaneRule::anchor)
    Slide,      // PaneRule::path
    Fill,       // stretched to cover the screen; only a leaf at the centre covering 1280x720
    Frame,      // grows out from PaneRule::side; only a leaf
    Projected,  // its position scales with the screen: placed at an actor's projected position
    Marker,     // not moved itself: marks its layout's root as placed at an actor (the root scales)
};
enum class Where { RootChild, Any };  // a child of the layout's root, or any pane below the root
struct PaneRule {
    std::string_view name;
    Where where;
    Rule rule;
    std::string_view child = {};  // only while the pane has a direct child of this name
    float min_y = -1e9f;          // only while the pane is at least this high
    Anchor anchor = {};
    Slide path = {};
    Side side = Side::Top;
};
constexpr PaneRule hud(std::string_view n, std::string_view child = {}) { return {n, Where::RootChild, Rule::Hud, child}; }
constexpr PaneRule anchored(std::string_view n, Anchor a, float min_y = -1e9f) { return {n, Where::Any, Rule::Anchored, {}, min_y, a}; }
constexpr PaneRule fill(std::string_view n) { return {n, Where::Any, Rule::Fill}; }
constexpr PaneRule frame(std::string_view n, Side s) { return {n, Where::Any, Rule::Frame, {}, -1e9f, {}, {}, s}; }
constexpr PaneRule projected(std::string_view n, Where w = Where::RootChild) { return {n, w, Rule::Projected}; }
constexpr PaneRule marker(std::string_view n) { return {n, Where::Any, Rule::Marker}; }

// Every pane of the HD layouts with a rule of its own. Anything else is content: it keeps its place
// in the 16:9 layout space, centred, and is clipped to it (menus stay centred).
inline constexpr PaneRule kRules[] = {
    // HUD at the screen edges (Main_00: hearts, magic, rupees, keys, boss gauge, arrows...; the
    // message windows' N_Position_00; timers)
    hud("N_TV_00"), hud("N_DRC_00"), hud("N_Default_00"), hud("N_HeartPos_01"), hud("L_Rupy_00"),
    hud("L_CompassClock_00"), hud("L_DungeonKey_00"), hud("N_SwimTimePos_00"), hud("L_BossHP_00"),
    hud("L_Arrow_00"), hud("L_BatteryVol1_00"), hud("L_RupySwordCounter_00"), hud("N_Position_00"),
    hud("N_Time_00"),
    // The action prompt at the bottom centre ("A Speak", CommandGuide_00): the game leaves it at its
    // authored 0,-247 rather than placing it at an actor.
    hud("L_CommandA_00"),
    // N_All_00 is a generic container name several layouts use, among them the message window's
    // choice box, whose cursor the game places from the drawn row positions (moving it misplaced the
    // Yes/No cursor at 16:10): only the button cluster (CommandGuide_00) and the place name
    // (PlaceName_00), told apart by their children.
    hud("N_All_00", "P_ZR_00"), hud("N_All_00", "T_PlaceName_00"),
    // The Telescope's and Pictograph Box's stick prompts ("Zoom", "Look Around") run along the top
    // beside the button cluster: they move up with it, so the X prompt stays below "Look Around".
    anchored("P_LStick_00", kTop), anchored("P_RStick_00", kTop),
    // The Wind Waker's conducting bar (BatonInput_00: the beat at y 321, the notes at 245). The message
    // window's own N_BatonSign_00 (MsgWindowEvent_00, a song's notes at -26) stays: hence the height.
    anchored("N_InputPoint_00", kTop, 200), anchored("N_BatonSign_00", kTop, 200),
    // The Auction (Auction_00): the A meter at -7,-280 is bottom HUD. The bid box slides
    // (Auction_00_PositionAuctionRupy) from above the message window at y -56, following it to the
    // bottom edge, to the top-left corner at y 295 while bidding.
    {"N_Meter_00", Where::Any, Rule::Hud},
    {"N_AuctionRupy_00", Where::Any, Rule::Slide, {}, -1e9f, {}, {-56, kBottom, 295, kTopLeft}},
    // The Pictograph Box's black frame (PictographBox_00): bars from the native screen's edges.
    frame("P_FramerT_00", Side::Top), frame("P_FrameB_00", Side::Bottom), frame("P_FrameL_00", Side::Left),
    frame("P_FramerR_00", Side::Right),
    // Full-screen backgrounds, filters and transition wipes. The pause pictures hold the captured
    // game picture, not the menu frame (PR #16).
    fill("PF_PauseTV_00"), fill("PF_PauseDRC_00"), fill("BootBase_00"), fill("BootDeco_00"), fill("P_Base_00"),
    fill("P_Base_01"), fill("PF_BG_00"), fill("P_BG_00"), fill("W_BG_00"), fill("P_NoDataBG_00"), fill("P_Dark_00"),
    fill("P_Dark_01"), fill("P_Light_00"), fill("P_Light_01"), fill("P_Wave00"), fill("P_Wave_01"),
    fill("P_OpeningPic_00"), fill("W_Wipe_00"), fill("P_BgPattern_00"), fill("W_Shutter_00"), fill("W_FadeWipe_00"),
    fill("W_FadeWipe_01"), fill("P_FadeWipe_00"),
    // Placed at an actor's projected position, scaled with the screen: the game projects with the
    // camera's own (widened) projection and maps it to the 16:9 layout space. The Hero's Charm's enemy
    // gauge: in the HUD (Main_00) the game moves N_EnemyHP_00 inside the part L_EnemyHP_00, which
    // stays at 0,0 (measured at 16:10: the gauge then sits where it does at 16:9 relative to the
    // targeting arrow). Markers: layouts whose roots the game translates to an actor; a translated
    // root alone is not evidence of that (selectors and map floors translate theirs too).
    projected("L_EnemyHP_00"), projected("N_EnemyHP_00", Where::Any),
    marker("N_EnemyHP_00"), marker("T_CommandA_00"), marker("P_SetSeatASpecial_00"),
};

// The rule of a pane, nullptr for content (and for markers: is_marker). root_child: its parent is
// the layout's root; leaf: no children; x, y: its translation; w, h: its scaled size;
// has_child(name): whether it has a direct child of that name.
template <class HasChild>
const PaneRule* rule_for(std::string_view name, bool root_child, bool leaf, float x, float y, float w, float h,
                         HasChild&& has_child) {
    for (const PaneRule& r : kRules) {
        if (r.name != name || r.rule == Rule::Marker) continue;
        if ((r.where == Where::RootChild && !root_child) || y < r.min_y) continue;
        if (r.rule == Rule::Fill && !(leaf && std::fabs(x) < 8 && std::fabs(y) < 8 && w >= 1270 && h >= 710)) continue;
        if (r.rule == Rule::Frame && !leaf) continue;
        if (!r.child.empty() && !has_child(r.child)) continue;
        return &r;
    }
    return nullptr;
}
inline bool is_marker(std::string_view name) {
    for (const PaneRule& r : kRules)
        if (r.rule == Rule::Marker && r.name == name) return true;
    return false;
}
// the pane's translation and scale under its rule; w, h: its scaled size
inline Transform apply(const PaneRule& r, float x, float y, float sx, float sy, float w, float h, float kx, float ky) {
    switch (r.rule) {
    case Rule::Hud: return shift(anchor_at(x, y), x, y, sx, sy, kx, ky);
    case Rule::Anchored: return shift(r.anchor, x, y, sx, sy, kx, ky);
    case Rule::Slide: return slide(r.path, x, y, sx, sy, kx, ky);
    case Rule::Fill: return {x, y, sx * cover(kx, 1280, w), sy * cover(ky, 720, h)};
    case Rule::Frame: return extend(r.side, x, y, sx, sy, w, h, kx, ky);
    case Rule::Projected: return {x * kx, y * ky, sx, sy};
    case Rule::Marker: break;
    }
    return {x, y, sx, sy};
}

inline bool parked_hud(float x, float y) {
    return std::fabs(x) > 672 || std::fabs(y) > 392;
}
// Intersect the guest scissor with the centred native layout region. Call only for TV content;
// render-target aspect factors exclude resolution scaling, so this also works at 2x/3x.
inline void clip(uint32_t w, uint32_t h, float kx, float ky,
                 uint32_t& x, uint32_t& y, uint32_t& ex, uint32_t& ey) {
    if (kx == 1 && ky == 1) return;
    // Match the renderer's scissor rounding tolerance: e.g. 800/(10/9) can be just below
    // 720 in float, which must not trim an extra pixel from both edges at 16:10.
    uint32_t left = uint32_t(std::ceil((w - w / kx) * .5f - .01f));
    uint32_t top = uint32_t(std::ceil((h - h / ky) * .5f - .01f));
    x = std::max(x, left); y = std::max(y, top);
    ex = std::min(ex, w - left); ey = std::min(ey, h - top);
}
}

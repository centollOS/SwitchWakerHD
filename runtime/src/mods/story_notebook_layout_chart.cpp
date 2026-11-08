// The story notebook detail view's mini sea chart as layout panes (story_notebook_layout_chart.h).
#include "story_notebook_layout_chart.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ppc.h"
#include "runtime.h"
#include "story_lyt_screen.h"
#include "story_notebook_page.h"

namespace story_notebook_layout::chart {
namespace {
namespace nb = story_notebook;
namespace ls = story_lyt_screen;

constexpr uint32_t kTranslate = 0x1C;  // hdPartsPaneSRT_c translate x, y
// nw::lyt picture pane: its material at +0xF4; the material's first texture at +0x28 (a GX2 texture:
// +4 width, +8 height), as the game's own P_MapLinkFace_00 (story_notebook_game.cpp chart_icons)
constexpr uint32_t kPictureMaterial = 0xF4, kMaterialTexture = 0x28;

struct Point {
    float x = 0, y = 0;
};

// what the layout shows now (from built(): nothing applied yet)
struct Shown {
    bool valid = false;  // built() found the sea's corners
    Point tl, tr, bl, br;  // the inset sea quad, in C_Chart's coordinates (layout: y up)
    bool face = false;     // the game's Link icon resolved
    bool applied = false;
    ChartView view;
};
Shown g_shown;

bool trace() {
    static const bool on = [] { const char* e = getenv("WWHD_NOTEBOOK_TRACE"); return e && *e == '1'; }();
    return on;
}

bool guest_pointer(uint32_t a) {
    return (a >= mem::kMem2Start && a < mem::kMem2End - 0x200) || (a >= mem::kMem1 && a < mem::kMem1 + mem::kMem1Size - 0x200);
}
float pane_f(uint32_t pane, uint32_t off) { return pane ? u32_as_f32(ld32(pane + off)) : 0.0f; }
Point translate_of(Cpu* c, const char* name) {
    const uint32_t p = ls::pane(c, name);
    return {pane_f(p, kTranslate), pane_f(p, kTranslate + 4)};
}

// card_chart's at(): a share of the squares to the (inset) sea, bilinearly
Point at(float u, float v) {
    const Shown& s = g_shown;
    const float tx = s.tl.x + (s.tr.x - s.tl.x) * u, ty = s.tl.y + (s.tr.y - s.tl.y) * u;
    const float bx = s.bl.x + (s.br.x - s.bl.x) * u, by = s.bl.y + (s.br.y - s.bl.y) * u;
    return {tx + (bx - tx) * v, ty + (by - ty) * v};
}

void show(Cpu* c, const char* name, bool on) { ls::set_visible(ls::pane(c, name), on); }
std::string dot_name(int i) {
    char n[16];
    std::snprintf(n, sizeof n, "C_Dot%02d", i);
    return n;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> anims() { return {{"ChartGlow", "G_ChartGlow"}}; }

ChartView view_of(const nb::Page& page, const nb::Quest& q) {
    ChartView v;
    v.shown = page.mode() == nb::Page::Mode::Detail;
    if (q.has_place) {
        v.place = true;
        nb::chart_share(q.x, q.z, v.place_u, v.place_v);
    }
    const nb::Scene& s = page.scene();
    if (s.link_known) {
        v.link = true;
        nb::chart_share(s.link_x, s.link_z, v.link_u, v.link_v);
    }
    return v;
}

bool same(const ChartView& a, const ChartView& b) {
    return a.shown == b.shown && a.place == b.place && a.link == b.link && a.place_u == b.place_u && a.place_v == b.place_v &&
           a.link_u == b.link_u && a.link_v == b.link_v;
}

int route(float lx, float ly, float px, float py, float (&xs)[kDots], float (&ys)[kDots]) {
    const float dx = px - lx, dy = py - ly, len = std::sqrt(dx * dx + dy * dy);
    const int dots = int(len / 9);
    int n = 0;
    for (int i = 1; i < dots && n < kDots; i++, n++) xs[n] = lx + dx * float(i) / float(dots), ys[n] = ly + dy * float(i) / float(dots);
    return n;
}

void built(Cpu* c) {
    g_shown = Shown{};
    const bool corners = ls::pane(c, "C_SeaTL") && ls::pane(c, "C_SeaTR") && ls::pane(c, "C_SeaBL") && ls::pane(c, "C_SeaBR");
    if (!corners) {
        LOG("[notebook] the layout has no chart (C_Sea* panes): the detail view shows none");
        return;
    }
    g_shown.tl = translate_of(c, "C_SeaTL"), g_shown.tr = translate_of(c, "C_SeaTR");
    g_shown.bl = translate_of(c, "C_SeaBL"), g_shown.br = translate_of(c, "C_SeaBR");
    g_shown.valid = true;
    // Link's icon: the game's texture, if the attached archive gave it to the picture
    const uint32_t face = ls::pane(c, "C_LinkFace");
    const uint32_t material = face ? ld32(face + kPictureMaterial) : 0;
    const uint32_t tex = guest_pointer(material) ? ld32(material + kMaterialTexture) : 0;
    g_shown.face = guest_pointer(tex) && ld32(tex + 4) > 0 && ld32(tex + 4) <= 1024 && ld32(tex + 8) > 0 && ld32(tex + 8) <= 1024;
    show(c, "C_LinkFace", g_shown.face);
    show(c, "C_LinkMark", !g_shown.face);
    if (g_shown.face)
        LOG("[notebook] the layout's mini chart uses the sea chart's Link icon (%ux%u)", ld32(tex + 4), ld32(tex + 8));
    else
        LOG("[notebook] the sea chart's Link icon didn't resolve (archive %s): the mini chart shows its own marker", kGameArchive);
    ls::play(c, "ChartGlow");
}

void apply(Cpu* c, const ChartView& v) {
    if (!g_shown.valid) return;
    if (g_shown.applied && same(v, g_shown.view)) return;
    g_shown.applied = true;
    g_shown.view = v;
    show(c, "C_Chart", v.shown);
    if (!v.shown) return;
    Point place, link;
    if (v.place) {
        place = at(v.place_u, v.place_v);
        ls::set_translate(ls::pane(c, "C_Place"), place.x, place.y);
    }
    show(c, "C_Place", v.place);
    if (v.link) {
        link = at(v.link_u, v.link_v);
        ls::set_translate(ls::pane(c, "C_Link"), link.x, link.y);
    }
    show(c, "C_Link", v.link);
    float xs[kDots], ys[kDots];
    const int n = v.link && v.place ? route(link.x, link.y, place.x, place.y, xs, ys) : 0;
    if (trace())
        LOG("[notebook] layout chart: link %s share %.4f %.4f at %.1f %.1f; place %s share %.4f %.4f at %.1f %.1f; %d route dots",
            v.link ? "on" : "off", v.link_u, v.link_v, link.x, link.y, v.place ? "on" : "off", v.place_u, v.place_v, place.x, place.y, n);
    for (int i = 0; i < kDots; i++) {
        const uint32_t dot = ls::pane(c, dot_name(i).c_str());
        if (i < n) ls::set_translate(dot, xs[i], ys[i]);
        ls::set_visible(dot, i < n);
    }
}

}  // namespace story_notebook_layout::chart

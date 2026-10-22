// The story notebook detail view's mini sea chart as panes of the notebook's own layout (local only, story
// mods are WIP): the panes C_* of tools/story_mods/make_notebook_chart.py, on the detail page's chart card.
//
// What the HUD draws (story_notebook_page.h card_chart), the layout's way: the grid is static (thin pictures
// on the HUD's exact lines, made by the generator); every frame Link's icon, the quest's place and the route
// between them are moved to the bilinear place of their share of the 7 x 7 squares (chart_share) on the
// card's sea, whose inset corners the layout carries (C_SeaTL / TR / BL / BR, read once after the build).
// The place's glow pulses as the layout's animation ChartGlow (looping, played after the build).
//
// Link's icon is the sea chart's own: the picture C_LinkFace names the game's texture MapLinkS_00^l, which
// resolves when the notebook's parts also attach the game's archive LinkPos_00 (kGameArchive: one of the
// archives the main scene caches for its screens, hd_scene_main_screens registerMainScreens, so it is
// resident whenever the item screen exists). If the texture didn't resolve, the plain marker C_LinkMark
// is shown instead (the HUD's fallback look).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Cpu;

namespace story_notebook {
class Page;
struct Quest;
}  // namespace story_notebook

namespace story_notebook_layout::chart {

inline constexpr const char* kGameArchive = "LinkPos_00";  // the archive with the sea chart's Link icon
inline constexpr int kDots = 40;                           // the route's dot pool (C_Dot00..)

struct ChartView {
    bool shown = false;               // the detail view shows the chart
    bool place = false;               // the quest's step has a place on the chart
    float place_u = 0, place_v = 0;   // its share of the 7 x 7 squares (chart_share)
    bool link = false;                // Link is on the sea
    float link_u = 0, link_v = 0;
};

// the animation entries the chart adds to the notebook's client (name, group)
std::vector<std::pair<std::string, std::string>> anims();

// the chart as the detail view shows it for the quest (pure: unit-testable)
ChartView view_of(const story_notebook::Page& page, const story_notebook::Quest& q);
bool same(const ChartView& a, const ChartView& b);

// The layout's side (game thread, story_lyt_screen callbacks):
void built(Cpu* c);                           // after the build: the sea's corners, the Link icon, the glow
void apply(Cpu* c, const ChartView& v);       // every frame the detail view is drawn (cheap when unchanged)

// the route's dots between two layout points (the HUD's: one every 9 px, the ends left out); n <= kDots
int route(float lx, float ly, float px, float py, float (&xs)[kDots], float (&ys)[kDots]);

}  // namespace story_notebook_layout::chart

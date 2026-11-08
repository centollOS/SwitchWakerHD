// The story notebook's detail view (one quest) as panes of the notebook's game layout: the children of
// N_Detail (tools/story_mods/make_notebook_detail.py, pane names "D_..."), filled from the quest as the
// HUD's detail page shows it (story_notebook_page.h framed_detail()): the title, the sender (the mod's seal
// in its ink with its initial, or the quest's portrait in the layout's portrait slot 5), "Step n of m" with one marker
// per step, the step done before, the three bands (next step, where, reward) wrapped with the game's line
// breaker, the status pill and the chart ring. The bottom plates are the list's (N_PlateBack /
// N_PlateTrack); place_prompts() centres their prompts for the labels the view names and shows the A
// plate's chart scroll and cursor (D_TrackScroll, D_TrackCursor*, put into N_PlateTrack by the generator).
//
// view_of() and same() are pure; built() / apply() / place_prompts() run on the game's UI thread with the
// layout built (story_lyt_screen.h).
#pragma once
#include <cstdint>
#include <string>

#include "story_notebook_list.h"

struct Cpu;

namespace story_notebook {
class Page;
}

namespace story_notebook_layout::detail {

inline constexpr int kSteps = 12;  // progress markers (D_Step0..11; the HUD draws at most 12)

struct DetailView {
    std::string title, sender, initial, progress, done_before;
    int seal = 0;                    // the mod's ink (0..5, page.h emblem_colour) for its seal
    uint16_t portrait = 0;           // the quest's key when it has a portrait (portrait slot 5), else 0
    int steps = 0;                   // markers shown (0..kSteps)
    uint8_t marker[kSteps] = {};     // step i + 1: 0 upcoming, 1 the current one, 2 done
    std::string bodies[3];           // next step, where, reward
    std::string head0 = "Next step"; // the first band's head ("Finished" for a done quest)
    bool where = true;               // the Where band (not for a done quest)
    std::string status;              // the pill's word
    int pill = 0;                    // its tint: 0 none (in progress), 1 New, 2 Not yet, 3 Done
    bool tracked = false;            // the chart ring and "On the chart"
    bool track_plate = false;        // the A plate (not for a done quest)
    std::string back_label, track_label;
};

// the quest of `row` as the detail view shows it
DetailView view_of(const story_notebook::Page& page, const story_notebook::Row& row);
bool same(const DetailView& a, const DetailView& b);

// right after the layout is built: the text panes' capacities
void built(Cpu* c);
// the view onto the D_ panes (texts only where they changed since the last apply after built())
void apply(Cpu* c, const DetailView& v);
// the bottom plates' prompts centred for their labels (T_BackIcon / T_BackLabel, T_TrackIcon /
// T_TrackLabel), the A plate's chart scroll with them when `scroll`; the labels are set here too
void place_prompts(Cpu* c, const std::string& back_label, const std::string& track_label, bool scroll);

}  // namespace story_notebook_layout::detail

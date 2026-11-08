// The story notebook's page as its game layout shows it (story_notebook_layout.cpp): the page's state
// (story_notebook_page.h) mapped onto what the layout's panes get. Pure code, unit-tested in
// runtime/tools/story_notebook_layout_test.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "story_notebook_list.h"

namespace story_notebook {
class Page;
}

namespace story_notebook_layout {

inline constexpr int kRows = 5;  // the layout's rows (N_Row0..N_Row4)

struct RowView {
    std::string title, next, status, initial;
    bool dim = false, tracked = false, initial_shown = false;
    uint16_t portrait = 0;  // the quest's key when it has a portrait (drawn in the row's medallion), else 0
    int pill = 0;  // 0 none, 1 New, 2 Not yet, 3 Done
};
struct View {
    int mode = 0;  // 0 closed, 1 list, 2 detail (version 1: not drawn by the layout)
    int tab = 0;
    std::string tab_labels[2];
    std::vector<RowView> rows;  // the rows shown (from the top), at most kRows
    int selected = -1;          // in rows
    bool up = false, down = false, empty = false, track_plate = false;
    std::string empty1, empty2;
    uint64_t serial = 0;  // changes when anything above changes
};

// a quest's portrait (its key) written into the layout's portrait slot (0..4 the rows, 5 the detail
// page's sender); false when it has none or it can't be shown (layout draw, game thread)
bool portrait_into(int slot, uint16_t key);

// the page as the layout shows it (serial left 0)
View view_of(const story_notebook::Page& page, const std::vector<story_notebook::Row>& all);

}  // namespace story_notebook_layout

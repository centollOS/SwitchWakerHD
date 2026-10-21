#include "../src/interp_sploosh.h"
#include <cassert>

int main() {
    std::array<uint8_t, 64> cells{};
    cells[0] = 102; cells[1] = 103; cells[2] = 104; // unshot ships are not markers
    cells[40] = 1; cells[50] = 1; cells[30] = 3;
    const auto before = interp::sploosh_marker_cells(cells.data());
    assert(before[0] == 31 && before[20] == 41 && before[21] == 51);
    // A miss before earlier misses reassigns their pooled models to other cells.
    cells[10] = 1;
    const auto miss = interp::sploosh_marker_cells(cells.data());
    assert(miss[0] == before[0]);
    assert(miss[20] == 11 && miss[21] == 41 && miss[22] == 51);
    assert(miss[20] != before[20] && miss[21] != before[21]);
    // A hit likewise changes only the affected hit identities, not the misses.
    cells[20] = 3;
    const auto hit = interp::sploosh_marker_cells(cells.data());
    assert(hit[0] == 21 && hit[1] == 31);
    for (unsigned i = 20; i < 52; ++i) assert(hit[i] == miss[i]);
    assert(interp::sploosh_marker_cells(cells.data()) == hit); // every hold pass
    cells[63] = 1; // appending a later cell must preserve every existing assignment
    const auto appended = interp::sploosh_marker_cells(cells.data());
    for (unsigned i = 0; i < 23; ++i) assert(appended[i] == hit[i]);
    assert(appended[23] == 64);
    interp::SplooshMarkers markers{};
    markers[0] = {0x1000, hit[0]}; markers[20] = {0x2000, hit[20]};
    assert(interp::sploosh_marker_cell(&markers, 0x1000) == 21);
    assert(interp::sploosh_marker_cell(&markers, 0x2000) == 11);
    assert(interp::sploosh_marker_cell(&markers, 0x3000) == 0); // board/cursor/ships
    assert(interp::sploosh_marker_cell(nullptr, 0x1000) == 0); // outside board draw
    cells.fill(0);
    for (auto identity : interp::sploosh_marker_cells(cells.data())) assert(identity == 0);
}

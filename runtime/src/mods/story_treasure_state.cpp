// Mod treasure charts, part (b): the chart state the salvage code asks (story_treasure.h).
//
// The salvage code asks the save's collect-map bits with the index save number - 1 (decomp d_salvage.cpp,
// d_a_salvage.cpp; dSv_player_map_c in d_save.cpp, which asserts 0 <= i_no < 128):
//   - isOpenMap 025B80C8: daSalvage_c::proc_wait turns a treasure point's ring on while the chart is opened
//     and not complete;
//   - isCompleteMap 025B8228: dSalvage_control_c::entry skips a finished chart's point, proc_wait too;
//   - onCompleteMap 025B8140: daSalvage_c::end_salvage 02465D30, after the get-item event (the chest was
//     opened and its item taken), then onEventBit(0x3E02).
// For a reserved save number (a mod chart's) these answer from the chart's record (story_chart: owned,
// opened in the chart list, finished) and onCompleteMap finishes the chart, so its red circle follows. The
// game's own map bits are never read or written for a reserved number (they don't exist past 128), and the
// event bit 0x3E02 that end_salvage sets after a mod chart's salvage is put back as the game had it.
// Any other number runs the game's code unchanged. The hooks are installed with the other parts' (install()
// in story_treasure_salvage.cpp), only when a mod declares charts.
#include "story_treasure.h"
#include "story_scenes.h"

#include "guest_addr.h"
#include "runtime.h"

namespace story_treasure {
namespace {

const uint32_t kSavePtr = GD(0x101F84DC);        // save base pointer (d_com_inf_game)
const uint32_t kIsOpenMap = GC(0x025B80C8);      // dSv_player_map_c::isOpenMap(no)
const uint32_t kIsCompleteMap = GC(0x025B8228);  // dSv_player_map_c::isCompleteMap(no)
const uint32_t kOnCompleteMap = GC(0x025B8140);  // dSv_player_map_c::onCompleteMap(no)
const uint32_t kEndSalvage = GC(0x02465D30);     // daSalvage_c::end_salvage(): onCompleteMap, onEventBit(0x3E02)
constexpr uint32_t kSaveEvent = 0x644;           // the save's event bits (dSv_event_c: save + 0x20 + 0x624)
constexpr uint32_t kSalvagedEvent = 0x3E02;      // byte 0x3E, bit 0x02 (dSv_event_c::onEventBit)

uint32_t g_finished = 0;  // the reserved save number onCompleteMap finished during end_salvage

// the declared chart of a reserved save number, as story_chart::charts() lists it
bool chart_view(uint32_t save_no, story_chart::ChartView& out) {
    const int index = mod_chart_index(save_no);
    if (index < 0) return false;
    std::vector<story_chart::ChartView> charts = story_chart::charts();
    if (size_t(index) >= charts.size()) return false;
    out = charts[size_t(index)];
    return true;
}

}  // namespace

int32_t salvage_state(uint32_t save_no) {
    story_chart::ChartView v;
    return chart_view(save_no, v) ? v.state : story_chart::kResultInvalid;
}
// the game's opened bit: set when the player opens the chart in the list (obtainChart); a finished chart
// stays opened
bool chart_record_open(uint32_t save_no) {
    story_chart::ChartView v;
    return chart_view(save_no, v) && ((v.state == story_chart::kResultOwned && v.opened) || v.state == story_chart::kResultDone);
}
// finished; a reserved number no chart declares has nothing to salvage: complete too
bool chart_record_complete(uint32_t save_no) {
    story_chart::ChartView v;
    return !chart_view(save_no, v) || v.state == story_chart::kResultDone;
}
int32_t finish_salvaged(uint32_t save_no) {
    story_chart::ChartView v;
    if (!chart_view(save_no, v) || v.state == story_chart::kResultNone) return story_chart::kResultInvalid;  // not Link's
    const int32_t r = story_chart::finish(v.mod, v.chart);
    LOG("[treasure] %s: the treasure of chart \"%s\" was salvaged (save number %u): %d", v.mod.c_str(), v.chart.c_str(), save_no, r);
    if (r == story_chart::kResultDone) story_scenes::trigger("salvage", v.mod, v.chart);
    return r;
}

namespace {
// isOpenMap(no) / isCompleteMap(no), r4 = save number - 1
void map_is_open(Cpu* c) {
    if (is_mod_map_index(c->r[4])) return void(c->r[3] = chart_record_open(c->r[4] + 1) ? 1 : 0);
    original(c, kIsOpenMap);
}
void map_is_complete(Cpu* c) {
    if (is_mod_map_index(c->r[4])) return void(c->r[3] = chart_record_complete(c->r[4] + 1) ? 1 : 0);
    original(c, kIsCompleteMap);
}
// onCompleteMap(no): a reserved number finishes its chart, nothing of the game's is written
void map_on_complete(Cpu* c) {
    if (is_mod_map_index(c->r[4])) {
        finish_salvaged(c->r[4] + 1);
        note_mod_chest();  // its chest, going away next, doesn't count in the game's chest counter (part a)
        g_finished = c->r[4] + 1;
        return;
    }
    original(c, kOnCompleteMap);
}
// end_salvage(): the game's code as it is (it frees the registry slot and clears mSalvageId); when it
// finished a mod chart (onCompleteMap above), the event bit it sets right after is put back as it was
void end_salvage(Cpu* c) {
    const uint32_t base = ld32(kSavePtr);
    const uint32_t at = base + kSaveEvent + (kSalvagedEvent >> 8);
    const uint8_t mask = kSalvagedEvent & 0xFF;
    const bool was_set = base && (ld8(at) & mask);
    g_finished = 0;
    original(c, kEndSalvage);
    if (g_finished && base && ld32(kSavePtr) == base && !was_set) st8(at, uint8_t(ld8(at) & ~mask));
    g_finished = 0;
}
}  // namespace

std::vector<Hook> state_hooks() {
    return {{kIsOpenMap, map_is_open}, {kIsCompleteMap, map_is_complete}, {kOnCompleteMap, map_on_complete}, {kEndSalvage, end_salvage}};
}

}  // namespace story_treasure

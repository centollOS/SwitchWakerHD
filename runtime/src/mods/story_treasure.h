// Mod treasure charts like the game's own: a mod chart (story_chart.h, chart numbers 61 and up) gets the
// game's salvage point at its place, the light ring while Link sails there with the chart, the crane, a
// chest and its item; opening the chest finishes the chart (the red circle on the sea chart).
//
// How the game does it (decomp d_salvage.cpp, d_a_salvage.cpp, d_a_salvage_tbox.cpp):
//   - Each salvage point is a Salvage actor (process 0x191) of a sea room. Its parameters: kind (bits
//     28..31, 0 = treasure chart), save number (20..27), chart square (12..19), item number (4..11), type
//     (0..3; 1 = the get-item salvage). dSalvage_control_c::entry 025B44A0 registers it in the salvage
//     registry; the first Salvage actor is the registry's owner and draws the rings, the others only
//     register and go away.
//   - A treasure point's save number n (1..128) is the chart's collect-map bit n - 1 (dSv_player_map_c):
//     entry skips it once isCompleteCollectMap(n - 1); daSalvage_c::proc_wait shows the ring while
//     isOpenCollectMap(n - 1) and not complete; daSalvage_c::end_salvage (after the chest's item was taken)
//     sets onCompleteCollectMap(n - 1) and the event bit 0x3E02.
//   - The chest (daSalvage_tbox, 0x192) only gets (type << 8) | item number; it doesn't know the point.
//
// Mod charts use save numbers the game never uses (kModSaveFirst.., outside 1..128), so the game's
// collect-map and event bits are never read or set for them; vanilla points never reach our code.
// Three parts implement this header:
//   (a) salvage points  story_treasure_salvage.cpp: spawns the Salvage actor of each owned, unfinished
//       mod chart when its sea room's actors are set up, and registers it (the game's entry() rejects
//       save numbers above 128).
//   (b) chart state     story_treasure_state.cpp: the collect-map checks of the salvage code answer
//       reserved numbers from the chart's record; end_salvage of a reserved number calls finish().
//   (c) chest item      story_treasure_chest.cpp: the chest gives the chart's item (a game item through
//       the game's own path, or a mod item).
// Nothing is installed unless a loaded mod declares charts (story_chart::charts() is not empty).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "story_chart.h"

struct Cpu;

namespace story_treasure {

// ---- reserved save numbers ----
// A mod chart's salvage point carries 129 + the chart's declaration index (story_chart::charts() order,
// i.e. chart number 61 + index): 129..160. The game's collect-map index of it (save number - 1, as
// isCompleteCollectMap and friends receive it) is 128..159.
inline constexpr uint32_t kModSaveFirst = 129, kModSaveCount = story_chart::kMaxCharts;
static_assert(kModSaveFirst > 128 && kModSaveFirst + kModSaveCount - 1 <= 255, "the save number is a u8 above the game's 1..128");
inline constexpr uint32_t kFirstModChartNumber = 61;
inline constexpr bool is_mod_save_no(uint32_t save_no) { return save_no >= kModSaveFirst && save_no < kModSaveFirst + kModSaveCount; }
inline constexpr bool is_mod_map_index(uint32_t index) { return is_mod_save_no(index + 1); }  // index = save number - 1
inline constexpr int mod_chart_index(uint32_t save_no) { return is_mod_save_no(save_no) ? int(save_no - kModSaveFirst) : -1; }
inline constexpr uint32_t mod_save_no(size_t index) { return kModSaveFirst + uint32_t(index); }
inline constexpr uint32_t chart_number_of(uint32_t save_no) { return save_no - kModSaveFirst + kFirstModChartNumber; }

// ---- (a) salvage points (story_treasure_salvage.cpp) ----
// The sea room (1..49) of a place on the Great Sea, as the chart list places the chart (story_chart
// build_entry): column = lround(x / 100000), row = lround(z / 100000), room = (row + 3) * 7 + column + 3 + 1;
// 0 outside the sea.
int sea_room(float x, float z);
struct SalvagePoint {
    uint32_t save_no;  // kModSaveFirst + declaration index
    int room;          // sea room 1..49
    float x, z;        // the chart's place (y: the sea's surface, 0)
    uint8_t item_no;   // the parameter's item number: chest_item_for(save_no).game_item
};
// The salvage points of the declared mod charts in that sea room Link hasn't finished. Like the game's own
// (entry() registers every treasure point whose chart isn't complete), owned or not: the point stays
// unused (no ring, no crane) until daSalvage_c::proc_wait finds the chart opened (b), so a chart Link
// gets while in its room works at once.
std::vector<SalvagePoint> salvage_points_for_room(int room);
// The save number of the point the salvage actor is working on now (daSalvage_c::mSalvageId's registry
// slot), 0 when none: (c) asks it when the chest gives its item.
uint32_t active_save_no();
// The chest of a mod chart's point: the game's chest (daSTBox_c::_delete 024683D8) counts every treasure chart
// chest it gave in an event register (0xADFF); a mod chart's chest doesn't count. note_mod_chest(): (b) calls it
// when a reserved point is finished (onCompleteMap in end_salvage, before the chest goes away); the chest's
// _delete then leaves the game's event bytes as they were.
void note_mod_chest();
// The declared mod chart of a reserved save number (story_chart::charts()[index]); false for any other.
bool chart_for_save_no(uint32_t save_no, std::string& mod, std::string& chart);

// ---- (b) chart state (story_treasure_state.cpp) ----
// The chart's record for a reserved save number: story_chart::kResultNone / kResultOwned / kResultDone
// (kResultInvalid for a number that isn't reserved or a chart that isn't declared).
int32_t salvage_state(uint32_t save_no);
bool chart_record_open(uint32_t save_no);      // isOpenCollectMap: owned and opened in the chart list
bool chart_record_complete(uint32_t save_no);  // isCompleteCollectMap: finished
int32_t finish_salvaged(uint32_t save_no);     // the chest was opened: story_chart::finish of that chart

// ---- (c) chest item (story_treasure_chest.cpp) ----
struct ChestItem {
    uint8_t game_item = 0;   // the parameters' item number (bits 4..11): the game item, or for a mod item (c)'s
                             // marker number (kChestMarker 0x1B, no game data uses it)
    int32_t mod_item = -1;   // a mod item's story key (story_items::item_key), -1: the game item itself
};
ChestItem chest_item_for(uint32_t save_no);

// ---- installation ----
// Each part lists its game-function replacements; install() (part a) installs all of them or none (when a
// loaded mod already replaces one of them), and only when a mod declares charts.
using Hook = std::pair<uint32_t, void (*)(Cpu*)>;
std::vector<Hook> salvage_hooks();  // (a)
std::vector<Hook> state_hooks();    // (b)
std::vector<Hook> chest_hooks();    // (c)
// original(c, fn): the game's function behind a replacement; call(c, fn, args): a game function
void set_game_calls(void (*original)(Cpu*, uint32_t), uint32_t (*call)(Cpu*, uint32_t, std::initializer_list<uint32_t>));
// for the hooks of all three parts (part a keeps them): the replaced game function itself, a game function
void original(Cpu* c, uint32_t fn);
uint32_t call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args);
// guest memory for the spawn's position, angle and scale (part a): kScratchBytes
inline constexpr uint32_t kScratchBytes = 0x40;
void set_scratch_memory(uint32_t guest);
void install(const std::function<std::string(uint32_t)>& replaced_by, const std::function<void(uint32_t, void (*)(Cpu*))>& replace);
bool installed();

}  // namespace story_treasure

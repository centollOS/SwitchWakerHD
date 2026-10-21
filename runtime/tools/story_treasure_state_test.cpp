// Mod treasure charts, part (b) (runtime/src/mods/story_treasure_state.cpp): the salvage code's collect-map
// questions for reserved save numbers answer from the chart's record, end_salvage finishes the chart, and the
// game's own map bits and its event bit 0x3E02 stay as the game has them. The game's functions are stand-ins
// with the game's behaviour on a synthetic save; no game files are needed.
#include "mods/story_chart.h"
#include "mods/story_mail_book.h"
#include "mods/story_store.h"
#include "mods/story_treasure.h"
#include "runtime.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace story_treasure;
using story_chart::kResultDone;
using story_chart::kResultInvalid;
using story_chart::kResultNone;
using story_chart::kResultOwned;

void log_msg(const char* fmt, ...) { (void)fmt; }

namespace {

constexpr uint32_t kGlobals = 0x101F0000, kArena = 0x20000000, kSave = kArena;
// the save's collect-map bits (dSv_player_map_c at save + 0xE4: owned, opened, complete; 4 x u32 each)
constexpr uint32_t kMap = kSave + 0xE4, kMapOpen = kMap + 0x20, kMapComplete = kMap + 0x30, kEvent3E = kSave + 0x644 + 0x3E;
constexpr uint32_t kIsOpenMap = 0x025B80C8, kIsCompleteMap = 0x025B8228, kOnCompleteMap = 0x025B8140, kEndSalvage = 0x02465D30;

void* map(uint32_t at, uint32_t size) {
#ifdef _WIN32
    return VirtualAlloc(mem::ptr(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return mmap(mem::ptr(at), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
}

std::map<uint32_t, void (*)(Cpu*)> hooked;
std::vector<std::string> calls;
uint32_t salvage_slot = 0;  // the save number of the point end_salvage finishes
bool map_bit(uint32_t base, uint32_t no) { return (ld32(base + 4 * (no >> 5)) >> (no & 31)) & 1; }
void set_map_bit(uint32_t base, uint32_t no) { st32(base + 4 * (no >> 5), ld32(base + 4 * (no >> 5)) | (1u << (no & 31))); }

uint32_t hook(uint32_t fn, uint32_t no) {
    Cpu c{};
    c.r[3] = kMap, c.r[4] = no;
    hooked.at(fn)(&c);
    return c.r[3];
}
void end_salvage(uint32_t save_no) {
    salvage_slot = save_no;
    Cpu c{};
    c.r[3] = 0x30000000;  // the salvage actor (the stand-in doesn't use it)
    hooked.at(kEndSalvage)(&c);
}
std::vector<uint8_t> save_bytes() {  // the game's part of the save (the story book is elsewhere)
    std::vector<uint8_t> out(0x1000);
    for (uint32_t i = 0; i < out.size(); i++) out[i] = ld8(kSave + i);
    return out;
}
void set_record(const std::string& mod, const std::string& chart, uint8_t value) {
    story_mail::Book b = story_store::book();
    assert(b.set(story_mail::kRecordMarker, story_chart::chart_key(mod, chart), value));
}

}  // namespace

// part (a) keeps these for all parts; the test's stand-ins: the game's functions as the game does them
namespace story_treasure {
unsigned g_mod_chests = 0;
void note_mod_chest() { ++g_mod_chests; }
void original(Cpu* c, uint32_t fn) {
    switch (fn) {
    case kIsOpenMap: case kIsCompleteMap: case kOnCompleteMap:
        assert(c->r[4] < 128);  // the game asserts 0 <= i_no && i_no < 128
        if (fn == kOnCompleteMap) set_map_bit(kMapComplete, c->r[4]);
        else c->r[3] = map_bit(fn == kIsOpenMap ? kMapOpen : kMapComplete, c->r[4]);
        calls.push_back("game map " + std::to_string(c->r[4]));
        break;
    case kEndSalvage: {  // kind 0: onCompleteMap(slot - 1), then onEventBit(0x3E02), frees the slot
        Cpu call = *c;
        call.r[3] = kMap, call.r[4] = salvage_slot - 1;
        hooked.at(kOnCompleteMap)(&call);
        st8(kEvent3E, ld8(kEvent3E) | 0x02);
        calls.push_back("end_salvage");
        break;
    }
    default: assert(!"unexpected original");
    }
}
uint32_t call(Cpu*, uint32_t, std::initializer_list<uint32_t>) {
    assert(!"part (b) calls no other game function");
    return 0;
}
}  // namespace story_treasure

int main() {
    assert(map(kGlobals, 0x10000) && map(kArena, 0x10000));
    story_store::set_memory(kArena + 0xE000);
    for (const Hook& h : state_hooks()) hooked[h.first] = h.second;
    assert(hooked.size() == 4 && hooked.count(kIsOpenMap) && hooked.count(kIsCompleteMap) && hooked.count(kOnCompleteMap) &&
           hooked.count(kEndSalvage));

    // the reserved numbers: 129 + declaration index, the map index 128..159
    static_assert(mod_save_no(0) == 129 && is_mod_map_index(128) && !is_mod_map_index(127) && is_mod_map_index(159) && !is_mod_map_index(160));

    // no save loaded, nothing declared: every reserved number is nothing to salvage
    assert(salvage_state(129) == kResultInvalid && !chart_record_open(129) && chart_record_complete(129));
    st32(0x101F84DC, kSave);
    assert(story_chart::define("cq", "old-map", "Old map", "", 1759, -202572) == story_chart::kResultNone);
    assert(story_chart::define("cq", "second", "Second", "", 100000, 0) == story_chart::kResultNone);
    assert(story_chart::define("cq", "third", "Third", "", 0, 100000) == story_chart::kResultNone);
    // the game's own charts: some owned, opened and complete
    set_map_bit(kMap + 0x10, 2), set_map_bit(kMapOpen, 2), set_map_bit(kMap + 0x10, 4), set_map_bit(kMapOpen, 4);
    set_map_bit(kMapComplete, 2);
    const std::vector<uint8_t> game_before = save_bytes();
    assert(!(ld8(kEvent3E) & 2));

    // declared, not owned: no ring, not complete (part a doesn't spawn it: not Link's)
    assert(salvage_state(129) == kResultNone && hook(kIsOpenMap, 128) == 0 && hook(kIsCompleteMap, 128) == 0);
    assert(story_chart::give("cq", "old-map") == kResultOwned);
    // owned, not opened in the chart list: still no ring, as the game's unopened chart
    assert(salvage_state(129) == kResultOwned && hook(kIsOpenMap, 128) == 0 && hook(kIsCompleteMap, 128) == 0);
    set_record("cq", "old-map", story_chart::kChartOpened);  // the player opened it in the list (obtainChart)
    assert(hook(kIsOpenMap, 128) == 1 && hook(kIsCompleteMap, 128) == 0);  // proc_wait: the ring shows
    // a reserved number no chart declares: complete (nothing to salvage), never open
    assert(salvage_state(132) == kResultInvalid && hook(kIsOpenMap, 131) == 0 && hook(kIsCompleteMap, 131) == 1);
    assert(hook(kIsCompleteMap, 159) == 1 && hook(kIsOpenMap, 159) == 0);
    assert(calls.empty());  // the game's map functions were never asked for a reserved number

    // the chest was opened: end_salvage finishes the chart; nothing of the game's save changes, not even 0x3E02
    end_salvage(129);
    assert((calls == std::vector<std::string>{"end_salvage"}));
    assert(g_mod_chests == 1);  // its chest won't count in the game's chest counter (part a)
    assert(salvage_state(129) == kResultDone && story_chart::state("cq", "old-map") == kResultDone);
    assert(hook(kIsCompleteMap, 128) == 1 && hook(kIsOpenMap, 128) == 1);  // finished: the point is skipped
    assert(save_bytes() == game_before);
    // finishing again changes nothing
    end_salvage(129);
    assert(salvage_state(129) == kResultDone && save_bytes() == game_before);
    // a chart Link doesn't own: end_salvage finishes nothing (finish_salvaged refuses), the game's save as it was
    assert(finish_salvaged(130) == kResultInvalid);
    end_salvage(130);
    assert(salvage_state(130) == kResultNone && save_bytes() == game_before);

    // a game chart: the game's code answers and writes, as without the mod
    calls.clear();
    assert(hook(kIsOpenMap, 2) == 1 && hook(kIsCompleteMap, 2) == 1 && hook(kIsOpenMap, 4) == 1 && hook(kIsCompleteMap, 4) == 0);
    assert(hook(kIsOpenMap, 127) == 0 && hook(kIsCompleteMap, 0) == 0);
    end_salvage(5);
    assert(map_bit(kMapComplete, 4) && (ld8(kEvent3E) & 2) && hook(kIsCompleteMap, 4) == 1);
    assert((calls == std::vector<std::string>{"game map 2", "game map 2", "game map 4", "game map 4", "game map 127", "game map 0",
                                              "game map 4", "end_salvage", "game map 4"}));
    // the event bit already set by the game: a mod chart's salvage leaves it set
    assert(story_chart::give("cq", "third") == kResultOwned);
    set_record("cq", "third", story_chart::kChartOpened);
    const std::vector<uint8_t> after_game = save_bytes();
    end_salvage(131);
    assert(salvage_state(131) == kResultDone && (ld8(kEvent3E) & 2) && save_bytes() == after_game);
    std::puts("story_treasure_state_test: ok");
    return 0;
}

namespace story_scenes { void trigger(const std::string&,const std::string&,const std::string&,unsigned) {} }

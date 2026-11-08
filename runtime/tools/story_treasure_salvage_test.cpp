// Mod treasure charts, part (a) (runtime/src/mods/story_treasure_salvage.cpp): reserved save numbers,
// the salvage points of a sea room, the spawn after a room's actors, the registration of a reserved point
// in a synthetic salvage registry, the point being salvaged, and the all-or-nothing install. The game's
// functions and the other parts (b, c) are stubbed; no game files are needed.
#include "mods/story_treasure.h"
#include "mods/story_chart.h"
#include "mods/story_store.h"
#include "runtime.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace story_treasure;

void log_msg(const char* fmt, ...) {
    (void)fmt;
}

namespace {

constexpr uint32_t kGlobals = 0x101F0000, kGlobals2 = 0x10460000, kArena = 0x20000000;
constexpr uint32_t kSave = kArena, kRegistry = kArena + 0x2000, kActor = kArena + 0x6000, kScratch = kArena + 0x7000;
constexpr uint32_t kStageName = 0x1046F0B0 + 0x5134, kTagData = 0x10475634, kSalvageId = 0x10475638;
constexpr uint32_t kRoomReLoader = 0x025C2E8C, kEntry = 0x025B44A0, kCreate = 0x025D5834, kChestDelete = 0x024683D8;

void* map(uint32_t at, uint32_t size) {
#ifdef _WIN32
    return VirtualAlloc(mem::ptr(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return mmap(mem::ptr(at), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
}

// the game
std::vector<std::string> calls;
struct Create {
    uint32_t proc, params, room, argument, cb;
    float x, y, z, sx, sy, sz;
    uint16_t ax, ay, az;
};
std::vector<Create> creates;
void original(Cpu* c, uint32_t fn) {
    if (fn == kRoomReLoader) calls.push_back("roomReLoader " + std::to_string(int32_t(c->r[5])));
    else if (fn == kChestDelete) {  // daSTBox_c::_delete of a chart chest: the counter register (made-up byte)
        const uint32_t ev = ld32(0x101F84DC) + 0x644;
        st8(ev + 0xA1, uint8_t(ld8(ev + 0xA1) + 1));
        calls.push_back("chest delete");
    }
    else if (fn == kEntry) calls.push_back("entry");
    else calls.push_back("other");
}
uint32_t game_call(Cpu*, uint32_t fn, std::initializer_list<uint32_t> args) {
    assert(fn == kCreate && args.size() == 8);
    const uint32_t* a = args.begin();
    auto f = [](uint32_t at) { return u32_as_f32(ld32(at)); };
    creates.push_back({a[0], a[1], a[3], a[6], a[7], f(a[2]), f(a[2] + 4), f(a[2] + 8), f(a[5]), f(a[5] + 4), f(a[5] + 8), ld16(a[4]),
                       ld16(a[4] + 2), ld16(a[4] + 4)});
    calls.push_back("create");
    return 0x1234;
}

std::map<uint32_t, void (*)(Cpu*)> installed_hooks;
void (*hooks_of(uint32_t addr))(Cpu*) {
    auto it = installed_hooks.find(addr);
    assert(it != installed_hooks.end());
    return it->second;
}

// registry helpers (dSalvage_control_c, d_salvage.h)
uint32_t slot(uint32_t no) { return kRegistry + 4 + no * 0x38; }
void reset_registry() {  // dSalvage_control_c::init
    memset(mem::ptr(kRegistry), 0, 0x2304);
    for (uint32_t i = 0; i < 160; i++) st8(slot(i) + 0x2C, 0xFF);
}
void make_actor(uint32_t params, int8_t room, float x, float z, float scale = 1.0f) {
    memset(mem::ptr(kActor), 0, 0x3AC);
    st32(kActor + 0xB0, params);
    st8(kActor + 0x326, uint8_t(room));
    st32(kActor + 0x314, f32_as_u32(x));
    st32(kActor + 0x318, f32_as_u32(0.0f));
    st32(kActor + 0x31C, f32_as_u32(z));
    for (uint32_t i = 0; i < 12; i += 4) st32(kActor + 0x330 + i, f32_as_u32(scale));
}
void call_entry(uint32_t emitter = 0) {
    Cpu c{};
    c.r[3] = kRegistry, c.r[4] = kActor, c.r[5] = emitter;
    hooks_of(kEntry)(&c);
}

}  // namespace

// ---- the other parts, stubbed ----
namespace story_treasure {
void stub_hook(Cpu*) {}
std::vector<Hook> state_hooks() { return {{0x025B8228, stub_hook}, {0x025B80C8, stub_hook}, {0x02465D30, stub_hook}}; }
std::vector<Hook> chest_hooks() { return {{0x025D7E88, stub_hook}}; }
bool chart_record_complete(uint32_t save_no) {
    const int i = mod_chart_index(save_no);
    const auto charts = story_chart::charts();
    return i >= 0 && size_t(i) < charts.size() && charts[size_t(i)].state == story_chart::kResultDone;
}
ChestItem chest_item_for(uint32_t save_no) {
    return save_no == kModSaveFirst ? ChestItem{0x05, -1} : ChestItem{0x1B, 3};  // a green rupee / a mod item
}
}  // namespace story_treasure

namespace {

void test_numbers() {
    static_assert(!is_mod_save_no(0) && !is_mod_save_no(128) && is_mod_save_no(129) && is_mod_save_no(160) && !is_mod_save_no(161));
    static_assert(is_mod_map_index(128) && !is_mod_map_index(127) && is_mod_map_index(159) && !is_mod_map_index(160));
    static_assert(mod_chart_index(129) == 0 && mod_chart_index(160) == 31 && mod_chart_index(128) == -1);
    static_assert(mod_save_no(5) == 134 && chart_number_of(129) == 61 && chart_number_of(mod_save_no(3)) == 64);
    // the sea's rooms, row by row from the north-west (A1 = 1, D4 = 25, G7 = 49)
    assert(sea_room(0, 0) == 25 && sea_room(-300000, -300000) == 1 && sea_room(300000, 300000) == 49);
    assert(sea_room(-300000, 300000) == 43 && sea_room(149999, 0) == 26 && sea_room(150001, 0) == 27);
    assert(sea_room(350000, 0) == 28 && sea_room(350001, 0) == 0 && sea_room(0, -400000) == 0 && sea_room(NAN, 0) == 0);
}

void test_points() {
    // no charts declared: no points, nothing installed
    assert(salvage_points_for_room(25).empty());
    set_game_calls(original, game_call);
    set_scratch_memory(kScratch);
    install([](uint32_t) { return std::string(); }, [](uint32_t, void (*)(Cpu*)) { assert(!"no charts declared"); });
    assert(!installed());

    st32(0x101F84DC, kSave);
    assert(story_chart::define("mod", "old-map", "Old map", "", 12000, -3000) == story_chart::kResultNone);    // D4, room 25
    assert(story_chart::define("mod", "far-map", "Far map", "", 210000, 190000) == story_chart::kResultNone);  // F6, room 41
    assert(story_chart::define("mod", "next", "Next", "", -8000, 4000) == story_chart::kResultNone);          // D4 too
    // every chart that isn't finished, owned or not (the ring waits for the chart, part b)
    auto pts = salvage_points_for_room(25);
    assert(pts.size() == 2 && pts[0].save_no == 129 && pts[1].save_no == 131 && pts[0].room == 25);
    assert(pts[0].x == 12000 && pts[0].z == -3000 && pts[0].item_no == 0x05 && pts[1].item_no == 0x1B);
    assert(salvage_points_for_room(41).size() == 1 && salvage_points_for_room(41)[0].save_no == 130);
    assert(salvage_points_for_room(24).empty() && salvage_points_for_room(0).empty() && salvage_points_for_room(50).empty());
    assert(story_chart::give("mod", "next") == story_chart::kResultOwned && story_chart::finish("mod", "next") == story_chart::kResultDone);
    pts = salvage_points_for_room(25);
    assert(pts.size() == 1 && pts[0].save_no == 129);

    std::string mod, chart;
    assert(chart_for_save_no(130, mod, chart) && mod == "mod" && chart == "far-map");
    assert(!chart_for_save_no(128, mod, chart) && !chart_for_save_no(132, mod, chart) && !chart_for_save_no(0, mod, chart));
}

void test_install() {
    // a mod replacing one of the functions: nothing installed
    install([](uint32_t addr) { return addr == 0x025B80C8 ? std::string("other mod") : std::string(); },
            [](uint32_t, void (*)(Cpu*)) { assert(!"conflict: all or nothing"); });
    assert(!installed());
    install([](uint32_t) { return std::string(); }, [](uint32_t addr, void (*fn)(Cpu*)) { installed_hooks[addr] = fn; });
    assert(installed() && installed_hooks.size() == 7 && installed_hooks.count(0x024683D8) && installed_hooks.count(kRoomReLoader) && installed_hooks.count(kEntry) &&
           installed_hooks.count(0x025B8228) && installed_hooks.count(0x025D7E88));
}

void test_spawn() {
    // a room of the sea: the game's actors first, then one Salvage actor per point
    memcpy(mem::ptr(kStageName), "sea\0\0\0\0", 8);
    calls.clear(), creates.clear();
    Cpu c{};
    c.r[3] = 0x30000000, c.r[4] = 0x30001000, c.r[5] = 25;
    hooks_of(kRoomReLoader)(&c);
    assert(calls.size() == 2 && calls[0] == "roomReLoader 25" && calls[1] == "create");
    const Create& k = creates[0];
    assert(k.proc == 0x191 && k.room == 25 && k.argument == 0xFFFFFFFFu && k.cb == 0);
    assert(k.params == ((129u << 20) | (0x05u << 4) | 1u) && (k.params >> 28) == 0);  // kind 0, save 129, item, type 1
    assert(k.x == 12000 && k.y == 0 && k.z == -3000 && k.sx == 1 && k.sy == 1 && k.sz == 1 && !k.ax && !k.ay && !k.az);
    // a room without points, another stage, the stage's own actors: only the game
    calls.clear(), creates.clear();
    c.r[5] = 24;
    hooks_of(kRoomReLoader)(&c);
    c.r[5] = 0xFFFFFFFF;
    hooks_of(kRoomReLoader)(&c);
    memcpy(mem::ptr(kStageName), "sea_T\0\0", 8);
    c.r[5] = 25;
    hooks_of(kRoomReLoader)(&c);
    assert(creates.empty() && calls.size() == 3);
    memcpy(mem::ptr(kStageName), "sea\0\0\0\0", 8);
}

void test_entry() {
    reset_registry();
    // the game's own points go to the game's entry(), untouched
    calls.clear();
    make_actor((0u << 28) | (5u << 20) | (0x80u << 4) | 1, 25, 0, 0);
    call_entry();
    make_actor((2u << 28) | (130u << 20), 25, 0, 0);  // a reserved number, but not a treasure point
    call_entry();
    make_actor((0u << 28) | (128u << 20), 25, 0, 0);
    call_entry();
    assert(calls.size() == 3 && calls[0] == "entry" && calls[2] == "entry");
    assert(ld8(kRegistry + 1) == 0);
    // a game point registered before ours (the game's entry would have)
    st8(kRegistry + 1, 3);
    for (uint32_t i = 128; i < 131; i++) st8(slot(i) + 0x2C, 25), st8(slot(i) + 0x2D, uint8_t(i - 127)), st8(slot(i) + 0x2F, 0);
    // ours: slot 131, as the game fills a treasure point
    calls.clear();
    make_actor((129u << 20) | (0x05u << 4) | 1, 25, 12000, -3000, 1.5f);
    call_entry(0xABCD0000);
    assert(calls.empty() && ld8(kRegistry + 1) == 4 && ld8(kRegistry) == 0);
    const uint32_t s = slot(131);
    assert(int8_t(ld8(s + 0x2C)) == 25 && ld8(s + 0x2D) == 129 && ld8(s + 0x2E) == 1 && ld8(s + 0x2F) == 0 && ld8(s + 0x30) == 0x05);
    assert(ld8(s + 0x31) == 1);  // unused until proc_wait finds the chart opened
    assert(u32_as_f32(ld32(s)) == 12000 && u32_as_f32(ld32(s + 4)) == 0 && u32_as_f32(ld32(s + 8)) == -3000);
    assert(u32_as_f32(ld32(s + 0xC)) == 1.5f && u32_as_f32(ld32(s + 0x18)) == 1050.0f && u32_as_f32(ld32(s + 0x1C)) == 1500.0f);
    assert(ld32(s + 0x24) == 0xABCD0000 && ld32(s + 0x20) == 0xFF);
    // twice (the room's actors set up again): one point
    call_entry();
    assert(ld8(kRegistry + 1) == 4);
    // a finished chart: none
    make_actor((131u << 20) | 1, 25, -8000, 4000);
    call_entry();
    assert(ld8(kRegistry + 1) == 4 && int8_t(ld8(slot(132) + 0x2C)) == -1);
    // the stage's slots stay the game's
    make_actor((130u << 20) | 1, 0, 0, 0);
    call_entry();
    assert(ld8(kRegistry) == 0 && ld8(kRegistry + 1) == 4);
    // a full room: one slot is kept free (the game's own bound lets the 33rd point past the table)
    reset_registry();
    st8(kRegistry + 1, 31);
    make_actor((130u << 20) | 1, 41, 210000, 190000);
    call_entry();
    assert(ld8(kRegistry + 1) == 31 && int8_t(ld8(slot(159) + 0x2C)) == -1);
    st8(kRegistry + 1, 30);
    call_entry();
    assert(ld8(kRegistry + 1) == 31 && int8_t(ld8(slot(158) + 0x2C)) == 41 && ld8(slot(158) + 0x2D) == 130);
}

void test_active() {
    reset_registry();
    st32(kTagData, 0);
    st32(kSalvageId, 0xFFFFFFFF);
    assert(active_save_no() == 0);
    st32(kTagData, kRegistry);
    assert(active_save_no() == 0);
    st8(slot(129) + 0x2C, 25), st8(slot(129) + 0x2D, 4), st8(slot(129) + 0x2F, 0);    // a game point
    st8(slot(130) + 0x2C, 25), st8(slot(130) + 0x2D, 130), st8(slot(130) + 0x2F, 0);  // ours
    st32(kSalvageId, 129);
    assert(active_save_no() == 0);
    st32(kSalvageId, 130);
    assert(active_save_no() == 130);
    st8(slot(130) + 0x2F, 2);  // another kind
    assert(active_save_no() == 0);
    st8(slot(130) + 0x2F, 0), st8(slot(130) + 0x2C, 0xFF);  // init_one: gone
    assert(active_save_no() == 0);
    st32(kSalvageId, 160);
    assert(active_save_no() == 0);
}

// a chart chest's _delete counts it in the game's event flags; a mod chart's chest doesn't
void test_chest() {
    const uint32_t ev = kSave + 0x644;
    st8(ev + 0xA1, 1);
    Cpu c{};
    reset_registry();
    st32(kTagData, kRegistry), st32(kSalvageId, 0xFFFFFFFF);
    hooks_of(kChestDelete)(&c);  // the game's chest
    assert(ld8(ev + 0xA1) == 2);
    note_mod_chest();  // (b): a reserved point was finished
    hooks_of(kChestDelete)(&c);
    assert(ld8(ev + 0xA1) == 2);
    hooks_of(kChestDelete)(&c);  // once only: the next chest is the game's again
    assert(ld8(ev + 0xA1) == 3);
    st8(slot(140) + 0x2C, 25), st8(slot(140) + 0x2D, 131), st8(slot(140) + 0x2F, 0), st32(kSalvageId, 140);  // still being salvaged
    hooks_of(kChestDelete)(&c);
    assert(ld8(ev + 0xA1) == 3);
}

}  // namespace

int main() {
    assert(map(kGlobals, 0x10000) && map(kGlobals2, 0x20000) && map(kArena, 0x10000));
    story_store::set_memory(kArena + 0xE000);
    test_numbers();
    test_points();
    test_install();
    test_spawn();
    test_entry();
    test_active();
    test_chest();
    std::puts("story_treasure_salvage_test: ok");
    return 0;
}

namespace story_actors { void native_room_loaded(Cpu*,int) {} }

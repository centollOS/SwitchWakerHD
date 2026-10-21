// Mod treasure charts, part (a): the salvage point of a mod chart (story_treasure.h).
//
// When a sea room's actors are set up (dStage_dt_c_roomReLoader 025C2E8C: the room's object layers, the
// game's own Salvage actors among them), the port asks the game to create one more Salvage actor (process
// 0x191, fopAcM_create 025D5834) for each declared mod chart of that room Link hasn't finished, in the same
// layer and room, after the room's own: kind 0, its reserved save number, type 1 (the get-item salvage), the
// chart's item number, at the chart's place on the sea's surface, scale 1.
//
// That actor goes the game's way: its create registers it with dSalvage_control_c::entry and, unless it is
// the first Salvage actor (the registry's owner), goes away again. The game's entry() drops save numbers
// above 128, so for a kind-0 point with a reserved save number the port's entry() fills the registry slot
// as the game does for a treasure point (dSalvage_info_c; radius 700, height 1000, times the scale):
// nothing when the chart is finished, "unused" (flag 1) until daSalvage_c::proc_wait finds the chart opened
// and not complete (part b answers those checks), the room's slots (128..159) as the game's room points.
// The game's check of the save's random salvage point (home.angle.z & 3) is for the game's four variants
// of a chart; a mod chart has one place. Every other call goes to the game's entry().
//
// Like the game, every chart that isn't finished gets its point (owned or not): the ring and the crane only
// work while the chart is opened (proc_wait), so a chart Link gets while in its room works at once.
#include "story_treasure.h"
#include "story_actors.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "guest_addr.h"
#include "runtime.h"

namespace story_treasure {
namespace {

const uint32_t kRoomReLoader = GC(0x025C2E8C);  // dStage_dt_c_roomReLoader(data, stage, roomNo): a room's actors
const uint32_t kEntry = GC(0x025B44A0);         // dSalvage_control_c::entry(this, actor, emitter)
const uint32_t kCreate = GC(0x025D5834);        // fopAcM_create(proc, params, pos, room, angle, scale, argument, cb)
const uint32_t kStageName = GD(0x1046F0B0) + 0x5134;  // dStage_startStage_c: the stage's name
const uint32_t kTagData = GD(0x10475634);       // daSalvage_c::mTagData_p: the salvage registry
const uint32_t kSalvageId = GD(0x10475638);     // daSalvage_c::mSalvageId: the registry slot being salvaged
const uint32_t kChestDelete = GC(0x024683D8);   // daSTBox_c::_delete: counts a chart chest (event register 0xADFF)
const uint32_t kSavePtr = GD(0x101F84DC);       // dSv_info_c (its event flags at +0x644, 0x100 bytes)
constexpr uint32_t kSaveEvent = 0x644, kEventBytes = 0x100;

constexpr uint32_t kProcSalvage = 0x191;
constexpr uint32_t kTypeGetItem = 1;
// fopAc_ac_c (HD layout)
constexpr uint32_t kActorParams = 0xB0, kActorPos = 0x314, kActorRoom = 0x326, kActorScale = 0x330;
// dSalvage_control_c / dSalvage_info_c (HD layout, d_salvage.h)
constexpr uint32_t kRegRoomNum = 1, kRegInfo = 4, kInfoBytes = 0x38;
constexpr uint32_t kInfoPos = 0, kInfoScale = 0xC, kInfoR = 0x18, kInfoH = 0x1C, kInfoSwitch = 0x20, kInfoEmitter = 0x24,
                   kInfoRoom = 0x2C, kInfoSave = 0x2D, kInfoType = 0x2E, kInfoKind = 0x2F, kInfoItem = 0x30, kInfoFlag = 0x31;
constexpr uint32_t kMaxSea = 128, kMaxRoom = 32, kMaxTotal = 160;

void (*g_original)(Cpu*, uint32_t) = nullptr;
uint32_t (*g_call)(Cpu*, uint32_t, std::initializer_list<uint32_t>) = nullptr;
uint32_t g_scratch = 0;
bool g_installed = false;
bool g_mod_chest = false;  // the chest being salvaged is a mod chart's (note_mod_chest)

bool trace() {
    static const bool on = [] { const char* e = getenv("WWHD_TREASURE_TRACE"); return e && *e == '1'; }();
    return on;
}
bool on_sea() {
    const char* name = (const char*)mem::ptr(kStageName);
    return strncmp(name, "sea", 8) == 0;
}
uint32_t slot(uint32_t reg, uint32_t no) { return reg + kRegInfo + no * kInfoBytes; }
// a reserved save number already registered (the room's actors set up again without the registry's reset)
bool registered(uint32_t reg, uint32_t save_no) {
    for (uint32_t i = 0; i < kMaxTotal; i++)
        if (int8_t(ld8(slot(reg, i) + kInfoRoom)) != -1 && ld8(slot(reg, i) + kInfoKind) == 0 && ld8(slot(reg, i) + kInfoSave) == save_no)
            return true;
    return false;
}

}  // namespace

int sea_room(float x, float z) {
    if (!(x >= -story_chart::kSeaHalf && x <= story_chart::kSeaHalf && z >= -story_chart::kSeaHalf && z <= story_chart::kSeaHalf)) return 0;
    // (the sea's very edge, +-3.5 squares, belongs to the outer squares)
    const long column = std::clamp(std::lround(x / story_chart::kSquare), -3L, 3L), row = std::clamp(std::lround(z / story_chart::kSquare), -3L, 3L);
    return int((row + 3) * 7 + column + 3 + 1);
}

std::vector<SalvagePoint> salvage_points_for_room(int room) {
    std::vector<SalvagePoint> out;
    if (room < 1 || room > 49) return out;
    const std::vector<story_chart::ChartView> charts = story_chart::charts();
    for (size_t i = 0; i < charts.size() && i < kModSaveCount; i++) {
        const story_chart::ChartView& v = charts[i];
        if (v.state == story_chart::kResultDone || sea_room(v.x, v.z) != room) continue;
        const uint32_t save_no = mod_save_no(i);
        out.push_back({save_no, room, v.x, v.z, chest_item_for(save_no).game_item});
    }
    return out;
}

uint32_t active_save_no() {
    const uint32_t reg = ld32(kTagData);
    const int32_t id = int32_t(ld32(kSalvageId));
    if (!reg || id < 0 || uint32_t(id) >= kMaxTotal) return 0;
    const uint32_t s = slot(reg, uint32_t(id));
    if (int8_t(ld8(s + kInfoRoom)) == -1 || ld8(s + kInfoKind) != 0) return 0;
    const uint32_t save_no = ld8(s + kInfoSave);
    return is_mod_save_no(save_no) ? save_no : 0;
}

void note_mod_chest() { g_mod_chest = true; }

bool chart_for_save_no(uint32_t save_no, std::string& mod, std::string& chart) {
    const int index = mod_chart_index(save_no);
    if (index < 0) return false;
    const std::vector<story_chart::ChartView> charts = story_chart::charts();
    if (size_t(index) >= charts.size()) return false;
    mod = charts[size_t(index)].mod, chart = charts[size_t(index)].chart;
    return true;
}

namespace hooks {
// dStage_dt_c_roomReLoader(data, stage, roomNo): after the room's own actors, the mod charts' points
void room_reloader(Cpu* c) {
    const Cpu keep = *c;
    const int32_t room = int32_t(c->r[5]);
    g_original(c, kRoomReLoader);
    story_actors::native_room_loaded(c, room);
    if (!g_scratch || !on_sea()) return;
    for (const SalvagePoint& p : salvage_points_for_room(room)) {
        // position (x, 0, z), angle (0, 0, 0), scale (1, 1, 1)
        st32(g_scratch + 0x00, f32_as_u32(p.x));
        st32(g_scratch + 0x04, f32_as_u32(0.0f));
        st32(g_scratch + 0x08, f32_as_u32(p.z));
        for (uint32_t i = 0; i < 6; i += 2) st16(g_scratch + 0x0C + i, 0);
        for (uint32_t i = 0; i < 12; i += 4) st32(g_scratch + 0x14 + i, f32_as_u32(1.0f));
        const uint32_t params = (p.save_no << 20) | (uint32_t(p.item_no) << 4) | kTypeGetItem;
        Cpu call = keep;
        const uint32_t id = g_call(&call, kCreate, {kProcSalvage, params, g_scratch, uint32_t(room), g_scratch + 0x0C, g_scratch + 0x14,
                                                    0xFFFFFFFFu, 0});
        if (trace() || id == 0xFFFFFFFFu)
            LOG("[treasure] room %d: salvage point %u at %.0f,%.0f (params %08X)%s", room, p.save_no, p.x, p.z, params,
                id == 0xFFFFFFFFu ? ": not created" : "");
    }
}

// dSalvage_control_c::entry(this, actor, emitter): a mod chart's point is registered here
void entry(Cpu* c) {
    const uint32_t reg = c->r[3], actor = c->r[4], emitter = c->r[5];
    const uint32_t params = actor ? ld32(actor + kActorParams) : 0;
    const uint32_t kind = params >> 28, save_no = (params >> 20) & 0xFF;
    if (!actor || kind != 0 || !is_mod_save_no(save_no)) return g_original(c, kEntry);
    const int8_t room = int8_t(ld8(actor + kActorRoom));
    if (room <= 0) return;  // the stage's points (slots 0..127) are the game's only
    const uint32_t count = ld8(reg + kRegRoomNum);
    if (count + 1 >= kMaxRoom) {  // (the game's own bound lets the 33rd point past the table: keep one free)
        LOG("[treasure] room %d: no room for salvage point %u (%u points)", room, save_no, count);
        return;
    }
    if (chart_record_complete(save_no) || registered(reg, save_no)) return;
    const uint32_t s = slot(reg, kMaxSea + count);
    st8(s + kInfoFlag, 1);  // unused until proc_wait finds the chart opened and not complete
    st8(s + kInfoRoom, uint8_t(room));
    for (uint32_t i = 0; i < 12; i += 4) {
        st32(s + kInfoPos + i, ld32(actor + kActorPos + i));
        st32(s + kInfoScale + i, ld32(actor + kActorScale + i));
    }
    st8(s + kInfoItem, uint8_t(params >> 4));
    st8(s + kInfoType, uint8_t(params & 15));
    st32(s + kInfoEmitter, emitter);
    st8(s + kInfoKind, 0);
    st8(s + kInfoSave, uint8_t(save_no));
    st32(s + kInfoSwitch, 0xFF);  // no switch
    const float sx = u32_as_f32(ld32(actor + kActorScale)), sy = u32_as_f32(ld32(actor + kActorScale + 4));
    st32(s + kInfoH, f32_as_u32(1000.0f * sy));
    st32(s + kInfoR, f32_as_u32(700.0f * sx));
    st8(reg + kRegRoomNum, uint8_t(count + 1));
    if (trace()) LOG("[treasure] salvage point %u registered in room %d (slot %u)", save_no, room, kMaxSea + count);
}
// daSTBox_c::_delete(): a mod chart's chest leaves the game's event flags (its chest counter) as they were
void chest_delete(Cpu* c) {
    const uint32_t save = ld32(kSavePtr);
    if (!(g_mod_chest || active_save_no()) || !save) return g_original(c, kChestDelete);
    g_mod_chest = false;
    uint8_t keep[kEventBytes];
    memcpy(keep, mem::ptr(save + kSaveEvent), kEventBytes);
    g_original(c, kChestDelete);
    if (ld32(kSavePtr) == save) memcpy(mem::ptr(save + kSaveEvent), keep, kEventBytes);
    if (trace()) LOG("[treasure] a mod chart's chest went away: the game's chest counter is unchanged");
}
}  // namespace hooks

std::vector<Hook> salvage_hooks() {
    return {{kRoomReLoader, hooks::room_reloader}, {kEntry, hooks::entry}, {kChestDelete, hooks::chest_delete}};
}

void set_game_calls(void (*original_fn)(Cpu*, uint32_t), uint32_t (*call_fn)(Cpu*, uint32_t, std::initializer_list<uint32_t>)) {
    g_original = original_fn;
    g_call = call_fn;
}
void original(Cpu* c, uint32_t fn) { g_original(c, fn); }
uint32_t call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) { return g_call(c, fn, args); }
void set_scratch_memory(uint32_t guest) { g_scratch = guest; }
bool installed() { return g_installed; }

void install(const std::function<std::string(uint32_t)>& replaced_by, const std::function<void(uint32_t, void (*)(Cpu*))>& replace) {
    if (g_installed || !g_original || !g_call || !g_scratch || !story_chart::declared()) return;
    std::vector<Hook> all = salvage_hooks();
    for (const std::vector<Hook>& part : {state_hooks(), chest_hooks()}) all.insert(all.end(), part.begin(), part.end());
    // all or nothing: a point registered without the checks that answer its save number would reach the
    // game's collect-map bits
    for (const Hook& h : all) {
        const std::string other = replaced_by(h.first);
        if (!other.empty()) {
            LOG("[treasure] %s replaces %08X: mod charts have no salvage points this session", other.c_str(), h.first);
            return;
        }
    }
    for (const Hook& h : all) replace(h.first, h.second);
    g_installed = true;
    LOG("[treasure] mod charts have salvage points (%zu hooks)", all.size());
}

}  // namespace story_treasure

// Story layouts (local only, WIP): several animations in one animation set, e.g. a button's state
// animations (Normal / Select / Press) next to the layout's fade. Used by story_lyt_proto.cpp when
// WWHD_STORY_LYT_ANIMS is set; without it the prototype keeps its one-animation set.
//
//   WWHD_STORY_LYT_ANIMS="InAll@G_All_00:0,Normal@G_Btn_00:1,Select@G_Btn_00:1,Press@G_Btn_00:1"
//       animation entries in order: <animation>@<group>:<slot>. Entry i plays anim/<layout>_<animation>.bflan
//       on the group's panes; animations of one slot replace each other (the button's states share one).
//   WWHD_STORY_LYT_STATES="Select:40,Press:20,Normal:40" (testing): plays these entries one after the
//       other, each for the given number of item-screen draws, and starts over.
//   WWHD_STORY_LYT_PLAY="Grow" (testing): entries played once when the layout is first drawn (looping ones
//       keep playing).
// The calls are the animation set's own (hd_ui_02004A3C): bindGroup(set, entry, animation, group) and
// play(set, entry, slot, rate), as the game's buttons use them (hdBasicButton_c plays its Select /
// UnSelect / Decide entries the same way).
#include "runtime.h"
#include "ppc.h"
#include "guest_addr.h"
#include "story_lyt_states.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace story_lyt_states {
namespace {
const uint32_t kBindGroup = GC(0x02004E04);  // animation set: bind entry to (animation, group)
const uint32_t kAnimPlay = GC(0x020053E4);   // animation set: play(entry, slot, rate in f1)

struct Entry { std::string anim, group; uint32_t slot = 0; };
struct Step { uint32_t entry = 0, draws = 0; };
std::vector<Entry> g_entries;
std::vector<Step> g_steps;
std::vector<uint32_t> g_once;
uint32_t g_step = 0, g_left = 0;
bool g_parsed = false, g_started = false;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

void parse() {
    if (g_parsed) return;
    g_parsed = true;
    if (const char* v = getenv("WWHD_STORY_LYT_ANIMS")) {
        for (const auto& item : split(v, ',')) {
            Entry e;
            size_t at = item.find('@'), colon = item.rfind(':');
            if (at == std::string::npos) { LOG("[story-lyt] WWHD_STORY_LYT_ANIMS: '%s' needs <animation>@<group>", item.c_str()); continue; }
            e.anim = item.substr(0, at);
            e.group = item.substr(at + 1, (colon == std::string::npos || colon < at ? item.size() : colon) - at - 1);
            if (colon != std::string::npos && colon > at) e.slot = uint32_t(atoi(item.c_str() + colon + 1));
            g_entries.push_back(e);
        }
    }
    if (const char* v = getenv("WWHD_STORY_LYT_PLAY")) {
        for (const auto& name : split(v, ','))
            for (uint32_t i = 0; i < g_entries.size(); i++)
                if (g_entries[i].anim == name) g_once.push_back(i);
    }
    if (const char* v = getenv("WWHD_STORY_LYT_STATES")) {
        for (const auto& item : split(v, ',')) {
            size_t colon = item.find(':');
            const std::string name = item.substr(0, colon);
            for (uint32_t i = 0; i < g_entries.size(); i++)
                if (g_entries[i].anim == name)
                    g_steps.push_back({i, colon == std::string::npos ? 60u : uint32_t(atoi(item.c_str() + colon + 1))});
        }
    }
}

void play(Cpu* c, Call call, uint32_t set, uint32_t entry) {
    c->f[1].ps0 = 1.0;
    c->f[1].ps1 = 1.0;
    call(c, kAnimPlay, {set, entry, g_entries[entry].slot});
}
}  // namespace

bool active() { parse(); return !g_entries.empty(); }

uint32_t entry_count() { parse(); return uint32_t(g_entries.size()); }

uint32_t slot_count() {
    parse();
    uint32_t n = 1;
    for (const auto& e : g_entries) n = e.slot + 1 > n ? e.slot + 1 : n;
    return n;
}

int entry_of(const char* anim) {
    parse();
    for (uint32_t i = 0; i < g_entries.size(); i++)
        if (g_entries[i].anim == anim) return int(i);
    return -1;
}

story_lyt_screen::Anim entry(uint32_t i) {
    parse();
    return {g_entries[i].anim, g_entries[i].group, g_entries[i].slot};
}

void bind(Cpu* c, Call call, uint32_t set, uint32_t (*safe_string)(const char*)) {
    for (uint32_t i = 0; i < g_entries.size(); i++) {
        call(c, kBindGroup, {set, i, safe_string(g_entries[i].anim.c_str()), safe_string(g_entries[i].group.c_str())});
        LOG("[story-lyt] animation entry %u: %s on %s, slot %u", i, g_entries[i].anim.c_str(), g_entries[i].group.c_str(),
            g_entries[i].slot);
    }
}

void play_named(Cpu* c, Call call, uint32_t set, const char* anim) {
    const int e = entry_of(anim);
    if (e >= 0) play(c, call, set, uint32_t(e));
}

void tick(Cpu* c, Call call, uint32_t set, uint32_t draw) {
    if (!set) return;
    if (!g_started) {
        g_started = true;
        for (uint32_t e : g_once) {
            play(c, call, set, e);
            LOG("[story-lyt] plays %s", g_entries[e].anim.c_str());
        }
    }
    if (g_steps.empty()) return;
    if (g_left == 0) {
        const Step& s = g_steps[g_step % g_steps.size()];
        play(c, call, set, s.entry);
        LOG("[story-lyt] state %s (item screen draw %u)", g_entries[s.entry].anim.c_str(), draw);
        g_left = s.draws ? s.draws : 1;
        g_step++;
    }
    g_left--;
}
}  // namespace story_lyt_states

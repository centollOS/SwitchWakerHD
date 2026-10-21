// Story layouts test harness (local only): a layout archive of our own shown with the item screen through
// story_lyt_screen.h. Enabled with WWHD_STORY_LYT=<path to the archive>; the layout's name is
// WWHD_STORY_LYT_NAME (default StoryLyt_00).
//   animations: WWHD_STORY_LYT_ANIMS (story_lyt_states.cpp), else InAll on G_All_00
//   InAll plays when the item screen is drawn again after half a second, and every
//   WWHD_STORY_LYT_REPLAY item-screen draws (testing); WWHD_STORY_LYT_TEXT_DEMO=1 fills the text demo
//   layout (story_lyt_text.cpp)
#include "runtime.h"
#include "ppc.h"
#include "story_lyt_screen.h"
#include "story_lyt_states.h"
#include "story_lyt_text.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>

namespace story_lyt {
namespace {
uint32_t (*g_call)(Cpu*, uint32_t, std::initializer_list<uint32_t>) = nullptr;
std::chrono::steady_clock::time_point g_last_draw{};
uint32_t g_draws = 0, g_replay = 0;
bool g_text_demo = false;

bool frame(Cpu* c, uint32_t lyt) {
    const auto now = std::chrono::steady_clock::now();
    ++g_draws;
    const bool reopened = now - g_last_draw > std::chrono::milliseconds(500);  // the item screen opens (again)
    g_last_draw = now;
    if (reopened || (g_replay && g_draws % g_replay == 0)) {
        story_lyt_screen::play(c, "InAll");
        LOG("[story-lyt] InAll starts (item screen draw %u)", g_draws);
    }
    if (story_lyt_states::active()) story_lyt_states::tick(c, g_call, story_lyt_screen::anim_set(), g_draws);
    if (g_text_demo) story_lyt_text::demo(c, lyt, g_draws);
    return true;
}
}  // namespace

void set_game_calls(void (*original)(Cpu*, uint32_t), uint32_t (*call)(Cpu*, uint32_t, std::initializer_list<uint32_t>)) {
    (void)original;
    g_call = call;
}

// Configures the test layout as story_lyt_screen's client (before story_lyt_screen::install)
void install(const std::function<void(uint32_t, void (*)(Cpu*))>&) {
    const char* file = getenv("WWHD_STORY_LYT");
    if (!file || !*file || !g_call) return;
    if (story_lyt_screen::has_client()) {
        LOG("[story-lyt] the layout screen has a client already: WWHD_STORY_LYT ignored");
        return;
    }
    if (const char* r = getenv("WWHD_STORY_LYT_REPLAY")) g_replay = uint32_t(atoi(r));
    if (const char* t = getenv("WWHD_STORY_LYT_TEXT_DEMO")) g_text_demo = *t == '1';
    story_lyt_screen::Client client;
    const char* name = getenv("WWHD_STORY_LYT_NAME");
    client.layout = name && *name ? name : "StoryLyt_00";
    client.archive = file;
    client.owner = "the story layout test harness";
    if (story_lyt_states::active())
        for (uint32_t i = 0; i < story_lyt_states::entry_count(); i++) client.anims.push_back(story_lyt_states::entry(i));
    else
        client.anims.push_back({"InAll", "G_All_00", 0});
    client.built = [](Cpu* c, uint32_t lyt) {
        if (g_text_demo) story_lyt_text::demo(c, lyt, 0);
    };
    client.frame = frame;
    story_lyt_screen::set_client(std::move(client));
}
}  // namespace story_lyt

// Story layouts (local only, WIP): several animations in one animation set (story_lyt_states.cpp).
#pragma once
#include <cstdint>
#include <initializer_list>

#include "story_lyt_screen.h"

struct Cpu;
namespace story_lyt_states {
using Call = uint32_t (*)(Cpu*, uint32_t, std::initializer_list<uint32_t>);
bool active();               // WWHD_STORY_LYT_ANIMS is set
uint32_t entry_count();      // animation entries for hdPartsLayout_c's load
uint32_t slot_count();       // play slots for hdPartsLayout_c's load
int entry_of(const char* anim);
story_lyt_screen::Anim entry(uint32_t i);  // entry i as story_lyt_screen's animation
void bind(Cpu* c, Call call, uint32_t set, uint32_t (*safe_string)(const char*));
void play_named(Cpu* c, Call call, uint32_t set, const char* anim);
void tick(Cpu* c, Call call, uint32_t set, uint32_t draw);  // WWHD_STORY_LYT_STATES test schedule
}  // namespace story_lyt_states

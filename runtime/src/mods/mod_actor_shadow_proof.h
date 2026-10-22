#pragma once
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
struct Cpu;
namespace mod_actor_shadow_proof {
void configure(void(*original)(Cpu*,uint32_t),uint32_t(*call)(Cpu*,uint32_t,std::initializer_list<uint32_t>));
// Parent calls install only when mod actors exist. Environment gate is checked here too.
bool install(const std::function<std::string(uint32_t)>& owner,const std::function<void(uint32_t,void(*)(Cpu*))>& replace);
// Optional WWHD_MOD_ACTOR_SHADOW_DRAW_FUNCTIONS comma-separated native hex
// addresses installs callbacks at startup; a first probe logs discovered targets.
void begin_model(uint32_t model=0);
void end_model();
// Called from both GX2 draw HLE entry points with the submitted vertex/index count.
void draw_submitted(uint32_t count);
void forget_model(uint32_t model);
void state_loaded();
}

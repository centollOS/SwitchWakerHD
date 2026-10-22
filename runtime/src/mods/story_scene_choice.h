// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
struct Cpu;
namespace story_scene_choice {
inline constexpr uint32_t kScratchBytes=512;
enum class Result {Pending,Complete,Failed};
using GameCall=uint32_t(*)(Cpu*,uint32_t,std::initializer_list<uint32_t>);
using OriginalCall=void(*)(Cpu*,uint32_t);
// The caller supplies its real compulsory-event lease check, and must close
// restored choices before releasing its own restored scene ownership marker.
void configure(OriginalCall,GameCall,uint32_t scratch,std::function<bool(Cpu*)> owns_scene);
bool install(const std::function<std::string(uint32_t)>& replaced_by,
             const std::function<void(uint32_t,void(*)(Cpu*))>& replace);
bool begin(Cpu*,const std::array<std::u16string,2>& labels,unsigned default_option=0);
Result poll(Cpu*,unsigned& selected);
void close(Cpu*);
void state_loaded();
bool active();
}

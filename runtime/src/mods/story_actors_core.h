// Mod-owned actor flags use the story extension only; never a game save-bit number.
#pragma once
#include "story_mail_book.h"
#include <string>
namespace story_actors {
inline constexpr uint8_t kRecordActorFlag = story_mail::kRecordActorFlag;
inline uint16_t flag_key(const std::string& mod,const std::string& flag) {
    return story_mail::letter_key(mod,"actor-flag:"+flag);
}
inline bool flag(const story_mail::Book& b,const std::string& mod,const std::string& id) {
    return b.get(kRecordActorFlag,flag_key(mod,id))!=0;
}
inline bool set_flag(story_mail::Book b,const std::string& mod,const std::string& id,bool on=true) {
    if(id.empty()) return false;
    return b.set(kRecordActorFlag,flag_key(mod,id),on?1:0);
}
}

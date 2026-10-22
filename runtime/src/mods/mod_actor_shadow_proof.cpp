#include "mod_actor_shadow_proof.h"
#include "runtime.h"
#include "guest_addr.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <cerrno>
#include <vector>
namespace mod_actor_shadow_proof { namespace {
using Owner=std::function<std::string(uint32_t)>;
using Replace=std::function<void(uint32_t,void(*)(Cpu*))>;
void(*forward)(Cpu*,uint32_t)=nullptr;
bool active=false;
struct Packet {uint32_t model=0,target=0;};
std::mutex lock;
std::map<uint32_t,Packet> packets;
std::set<uint32_t> pending,installed;
thread_local unsigned capture_depth=0,cascade_depth=0;
thread_local uint32_t captured_model=0,cascade_index=0;
struct Submission {uint32_t packet=0,model=0,draws=0;uint64_t elements=0;};
thread_local Submission* submission=nullptr;
unsigned reports=0;
bool valid(uint32_t address,uint32_t bytes=4){return address>=mem::kMem2Start&&address<=mem::kMem2End-bytes;}
void entry(Cpu*c,uint32_t address){
    if(capture_depth){
        const uint32_t packet=c->r[4];
        if(valid(packet,0x14)){
            const uint32_t vt=ld32(packet+0xC);
            if(valid(vt,0x30)){
                const uint32_t target=ld32(vt+0x2C);
                if(target<mem::kMem2Start&&!(target&3)&&dispatch::lookup(target)){
                    std::lock_guard guard(lock);packets[packet]={captured_model,target};
                    if(!installed.contains(target)&&pending.insert(target).second)LOG("[mod_actor_shadow] discovered callback=%08X; supply startup WWHD_MOD_ACTOR_SHADOW_DRAW_FUNCTIONS for geometry proof",target);
                }
            }
        }
    }
    forward(c,address);
}
void entry_z(Cpu*c){entry(c,GC(0x027F0C14));}
void entry_non(Cpu*c){entry(c,GC(0x027F0DA8));}
void entry_imm(Cpu*c){entry(c,GC(0x027F0E04));}
void cascade(Cpu*c){
    const auto previous=cascade_index;const uint32_t info=c->r[4];
    cascade_index=valid(info)?ld32(info):0xffffffffu;++cascade_depth;
    try{forward(c,GC(0x02737D54));}catch(...){--cascade_depth;cascade_index=previous;throw;}
    --cascade_depth;cascade_index=previous;
}
void packet_draw(Cpu*c){
    const uint32_t packet=c->r[3];uint32_t target=0;Packet owned;
    if(valid(packet,0x10)){const uint32_t vt=ld32(packet+0xC);if(valid(vt,0x30))target=ld32(vt+0x2C);}
    {std::lock_guard guard(lock);const auto found=packets.find(packet);if(found!=packets.end())owned=found->second;}
    if(!target)return;
    if(!cascade_depth||owned.target!=target){forward(c,target);return;}
    Submission own{packet,owned.model};Submission* previous=submission;submission=&own;
    try{forward(c,target);}catch(...){submission=previous;throw;}
    submission=previous;
    std::lock_guard guard(lock);
    if(reports<64){++reports;LOG("[mod_actor_shadow] model=%08X packet=%08X callback=%08X cascade=%u gx2_draws=%u elements=%llu",own.model,packet,target,cascade_index,own.draws,static_cast<unsigned long long>(own.elements));}
}
}
void configure(void(*original)(Cpu*,uint32_t),uint32_t(*)(Cpu*,uint32_t,std::initializer_list<uint32_t>)){forward=original;}
bool install(const Owner&owner,const Replace&replace){
    const char* env=std::getenv("WWHD_MOD_ACTOR_SHADOW_PROOF");
    if(!env||std::strcmp(env,"1")||!forward)return false;
    if(active)return true;
    std::vector<uint32_t> callbacks;
    if(const char* list=std::getenv("WWHD_MOD_ACTOR_SHADOW_DRAW_FUNCTIONS")){
        const char* at=list;
        while(*at){
            char* end=nullptr;errno=0;const unsigned long parsed=std::strtoul(at,&end,16);
            if(errno||end==at||(*end&&*end!=',')||parsed>0xfffffffful)return false;
            const uint32_t target=uint32_t(parsed);
            if(target>=mem::kMem2Start||(target&3)||!dispatch::lookup(target)||!owner(target).empty())return false;
            if(callbacks.size()>=16||target==GC(0x027F0C14)||target==GC(0x027F0DA8)||target==GC(0x027F0E04)||target==GC(0x02737D54))return false;
            for(const auto previous:callbacks)if(previous==target)return false;
            callbacks.push_back(target);at=*end?end+1:end;
        }
    }
    for(const uint32_t address:{GC(0x027F0C14),GC(0x027F0DA8),GC(0x027F0E04),GC(0x02737D54)})if(!owner(address).empty())return false;
    replace(GC(0x027F0C14),entry_z);replace(GC(0x027F0DA8),entry_non);replace(GC(0x027F0E04),entry_imm);replace(GC(0x02737D54),cascade);
    for(const auto target:callbacks){replace(target,packet_draw);installed.insert(target);LOG("[mod_actor_shadow] installed startup callback=%08X",target);}
    active=true;return true;
}
void begin_model(uint32_t model){if(active){if(!capture_depth)captured_model=model;++capture_depth;}}
void end_model(){if(active&&capture_depth)--capture_depth;}
void draw_submitted(uint32_t count){if(submission&&count){++submission->draws;submission->elements+=count;}}
void forget_model(uint32_t model){
    std::lock_guard guard(lock);
    for(auto it=packets.begin();it!=packets.end();)if(it->second.model==model)it=packets.erase(it);else ++it;
}
void state_loaded(){std::lock_guard guard(lock);packets.clear();pending.clear();reports=0;}
}

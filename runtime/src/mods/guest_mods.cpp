// Guest mods (Mod SDK v2 prototype, docs/mod-sdk-v2.md): PowerPC mods translated to C on install and
// compiled into a module by the player's own compiler (tools/guestmod/). They hook or replace game
// functions at runtime through the per-function flag check that recomp.py --mod-hooks puts at the
// start of every game function body (PPC_MOD_HOOK in ppc.h).
//
// The mod manager builds and loads its frozen, trusted guest set before guest code runs.
#include "../exception_report.h"
#include "guest_mods.h"
#include "guest_validation.h"
#include "guest_build.h"
#include "packages.h"
#include "guest_heap.h"
#include "guest_files.h"
#include "guest_settings.h"
#include "guest_hud.h"
#include <fstream>
#include <filesystem>
#include "guest_audio.h"
#include "story_notebook_game.h"
#include "guest_png.h"
#include "story_mail.h"
#include "story_mail_book.h"
#include "story_chart.h"
#include "story_notebook.h"
#include "story_manifest.h"
#include "story_store.h"
#include "story_takeover.h"
#include "mod_json.h"
#include "input.h"
#include "true60.h"
#include "guest_addr.h"
#include <atomic>
#include <limits>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// (this fork) native guest modules are built against upstream's ppc.h (g_core_preempt, its Cpu layout);
// this fork's generated code differs (Cpu::preempt, registers kept in host locals), so they are refused
#define WWHD_FORK_NO_NATIVE_GUEST_MODS 1
#if WWHD_FORK_NO_NATIVE_GUEST_MODS
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "recomp_table.h"
#include "runtime.h"
#include "../platform/host.h"
#include "wwhd_guest_abi.h"

extern "C" {
unsigned g_mod_hook_count = 0;  // initialized only by a hook-enabled game-code build
uint8_t* g_mod_hook_flags = nullptr;
const PpcFunc* g_mod_bodies = nullptr;
void ppc_mod_register(unsigned count, uint8_t* flags, const PpcFunc* bodies) {
    g_mod_hook_count = count; g_mod_hook_flags = flags; g_mod_bodies = bodies;
}
#if !WWHD_FORK_NO_NATIVE_GUEST_MODS
extern volatile int g_core_preempt[3];
#endif
double ppc_fres(double);
double ppc_frsqrte(double);
}

// Cpu::mod_skip uses former padding: save states (which store sizeof(Cpu) bytes) stay compatible
static_assert(sizeof(void*) != 8 || sizeof(Cpu) == 752, "Cpu layout changed");

namespace guestmods {
namespace {

struct Chain {
    uint32_t addr = 0, ordinal = 0;
    PpcFunc replace = nullptr;
    std::string replace_mod;
    std::vector<PpcFunc> entry, ret;
};
std::unordered_map<uint32_t, Chain> g_chains;  // built before the game starts, read-only afterwards
struct Loaded {
    std::string path,id,version,package_path,data_path,name;
    uint32_t hud_callback=0,hud_screen=0;
    uint64_t hud_step=0;
    bool hud_called=false;
    const WWHDGuestModuleV1* m=nullptr;
    uint32_t region_size=0;
    mods::json::Value options;
    std::unique_ptr<Heap> heap;
    std::unique_ptr<Files> files;
};
std::vector<Loaded> g_loaded;
std::atomic<uint64_t> g_logic_step{0};
std::atomic<bool> g_hud_callbacks{false};
std::mutex g_hud_mutex;
Loaded& owner(Cpu* c) {
    for(auto& mod:g_loaded)
        if(c->pc>=mod.m->mem_base&&c->pc-mod.m->mem_base<mod.m->mem_size)return mod;
    fatal("[guestmods] host service called outside a loaded mod: %08X",c->pc);
}

int ordinal_of(uint32_t addr) {
    const RecompEntry* b = g_recomp_funcs;
    const RecompEntry* e = g_recomp_funcs + g_recomp_func_count;
    const RecompEntry* it = std::lower_bound(b, e, addr, [](const RecompEntry& r, uint32_t a) { return r.addr < a; });
    return it != e && it->addr == addr ? (int)(it - b) : -1;
}

void call_original(Cpu* c, uint32_t func) {
    int i = ordinal_of(func);
    if (i < 0) fatal("[guestmods] call_original: %08X is not a game function", func);
    if (g_mod_hook_flags[i]) c->mod_skip = func;  // consumed by the check at the start of the body
    g_mod_bodies[i](c);
}

// ---- host services (imports of guest mods by name; arguments in r3..r10 / f1..f8, result in r3 / f1)
std::string cstr(uint32_t a) { return a ? mem::read_cstr(a) : std::string("(null)"); }
void svc_log(Cpu* c) { LOG("[guestmod:%s] %s",owner(c).id.c_str(),cstr(c->r[3]).c_str()); }
void svc_log_int(Cpu* c) { LOG("[guestmod:%s] %s %d",owner(c).id.c_str(),cstr(c->r[3]).c_str(),(int32_t)c->r[4]); }
void svc_log_hex(Cpu* c) { LOG("[guestmod:%s] %s %08X",owner(c).id.c_str(),cstr(c->r[3]).c_str(),c->r[4]); }
void svc_log_float(Cpu* c) { LOG("[guestmod:%s] %s %g",owner(c).id.c_str(),cstr(c->r[3]).c_str(),c->f[1].ps0); }
void svc_memcpy(Cpu* c) { memmove(mem::ptr(c->r[3]), mem::ptr(c->r[4]), c->r[5]); }
void svc_memset(Cpu* c) { memset(mem::ptr(c->r[3]), (int)(c->r[4] & 0xFF), c->r[5]); }
const mods::json::Value& option(Cpu* c) {return owner(c).options.get(cstr(c->r[3]));}
void svc_config_int(Cpu* c) {
    const auto& v=option(c);
    if(v.type==mods::json::Value::Number&&v.number>=INT32_MIN&&v.number<=INT32_MAX&&std::floor(v.number)==v.number)c->r[3]=uint32_t(int32_t(v.number));
    else c->r[3]=c->r[4];
}
void svc_config_bool(Cpu* c) {const auto& v=option(c);c->r[3]=v.type==mods::json::Value::Bool?v.boolean:!!c->r[4];}
void svc_config_float(Cpu* c) {const auto& v=option(c);if(v.type==mods::json::Value::Number)c->f[1].ps0=v.number;}
void svc_config_string(Cpu* c) {
    const auto& v=option(c);uint32_t dst=c->r[4],size=c->r[5];
    if(v.type!=mods::json::Value::String||!dst||!size||size>1024*1024){c->r[3]=0;return;}
    uint32_t n=uint32_t(std::min<size_t>(v.text.size(),size-1));
    memcpy(mem::ptr(dst),v.text.data(),n);mem::ptr(dst)[n]=0;c->r[3]=n;
}
void svc_malloc(Cpu* c) {c->r[3]=owner(c).heap->allocate(c->r[3]);}
void svc_free(Cpu* c) {if(!owner(c).heap->release(c->r[3]))LOG("[guestmod:%s] invalid heap free",owner(c).id.c_str());}
void svc_file_read(Cpu* c) {if((!c->r[4]&&c->r[5])||uint64_t(c->r[4])+c->r[5]>0x100000000ull){c->r[3]=UINT32_MAX;return;}c->r[3]=uint32_t(owner(c).files->read(cstr(c->r[3]),mem::ptr(c->r[4]),c->r[5]));}
void svc_file_write(Cpu* c) {if((!c->r[4]&&c->r[5])||uint64_t(c->r[4])+c->r[5]>0x100000000ull){c->r[3]=UINT32_MAX;return;}c->r[3]=uint32_t(owner(c).files->write(cstr(c->r[3]),mem::ptr(c->r[4]),c->r[5]));}
void svc_input(Cpu* c) {
    if(!c->r[3])return;
    const auto p=input::read();uint32_t a=c->r[3];
    st32(a,p.buttons);stf32(a+4,p.lx);stf32(a+8,p.ly);stf32(a+12,p.rx);stf32(a+16,p.ry);
    st32(a+20,p.touch);stf32(a+24,p.tx);stf32(a+28,p.ty);
}
void svc_logic_dt(Cpu* c) {c->f[1].ps0=double(true60::dt())/30.0;}
void svc_logic_step(Cpu* c) {uint64_t step=g_logic_step.load(std::memory_order_relaxed);c->r[3]=uint32_t(step>>32);c->r[4]=uint32_t(step);}
bool setting_buffer(const Loaded& mod,uint32_t address,uint32_t bytes) {
    return address>=mod.m->mem_base&&uint64_t(address)+bytes<=uint64_t(mod.m->mem_base)+mod.region_size;
}
std::string setting_key(const Loaded& mod,uint32_t address) {
    std::string key;
    for(uint32_t i=0;i<64;++i) {
        if(!setting_buffer(mod,address+i,1))return {};
        char byte=char(ld8(address+i));if(!byte)return key;
        if(byte<32||byte>126)return {};key+=byte;
    }
    return {};
}
void svc_setting_get(Cpu* c) {
    auto& mod=owner(c);auto key=setting_key(mod,c->r[3]);
    auto value=settings::read(key);uint32_t expected=c->r[4],destination=c->r[5],capacity=c->r[6];c->r[3]=0;
    if(!value||value->type()!=expected)return;
    uint32_t bytes=value->type()==settings::String?uint32_t(std::get<std::string>(value->data).size()+1):value->type()==settings::Number?8:4;
    if(!destination&&!capacity){c->r[3]=bytes;return;}
    if(capacity<bytes||!setting_buffer(mod,destination,bytes))return;
    switch(value->type()) {
        case settings::String:memcpy(mem::ptr(destination),std::get<std::string>(value->data).c_str(),bytes);break;
        case settings::Boolean:st32(destination,std::get<bool>(value->data));break;
        case settings::Unsigned:st32(destination,std::get<uint32_t>(value->data));break;
        case settings::Number:stf64(destination,std::get<double>(value->data));break;
        default:return;
    }
    c->r[3]=bytes;
}
void svc_setting_changed(Cpu* c) {
    auto& mod=owner(c);auto value=settings::read(setting_key(mod,c->r[3]));
    uint64_t revision=value?value->revision:0;c->r[3]=uint32_t(revision>>32);c->r[4]=uint32_t(revision);
}
// PCM is signed 16-bit big-endian in the guest, copied before returning.
void svc_audio_epoch(Cpu* c) {auto v=pcm::store().epoch();c->r[3]=uint32_t(v>>32);c->r[4]=uint32_t(v);}
void svc_audio_open(Cpu* c) {
    auto& mod=owner(c);c->r[3]=uint32_t(pcm::store().open(mod.id,c->r[3],c->r[4]));
    if(getenv("WWHD_AUDIO_STREAM_TRACE"))LOG("[guestpcm:%s] open %d epoch %llu",mod.id.c_str(),int32_t(c->r[3]),(unsigned long long)pcm::store().epoch());
}
void svc_audio_available(Cpu* c) {c->r[3]=uint32_t(pcm::store().available(owner(c).id,c->r[3]));}
void svc_audio_close(Cpu* c) {
    auto& mod=owner(c);uint32_t handle=c->r[3];c->r[3]=uint32_t(pcm::store().close(mod.id,handle));
    if(getenv("WWHD_AUDIO_STREAM_TRACE"))LOG("[guestpcm:%s] close %u result %d",mod.id.c_str(),handle,int32_t(c->r[3]));
}
void svc_audio_submit(Cpu* c) {
    auto& mod=owner(c);uint32_t h=c->r[3],a=c->r[4],frames=c->r[5],channels=c->r[6];
    c->r[3]=uint32_t(pcm::kInvalid);
    if(frames>pcm::kMaxSubmit||(channels!=1&&channels!=2)||!setting_buffer(mod,a,frames*channels*2))return;
    int16_t samples[pcm::kMaxSubmit*2];
    for(uint32_t i=0;i<frames*channels;++i)samples[i]=int16_t(ld16(a+i*2));
    c->r[3]=uint32_t(pcm::store().submit(mod.id,h,samples,frames,channels));
    if(getenv("WWHD_AUDIO_STREAM_TRACE"))LOG("[guestpcm:%s] submit %u requested %u accepted %d",mod.id.c_str(),h,frames,int32_t(c->r[3]));
}
// HUD services use packed big-endian guest structures, never host struct casts.
std::string hud_path(const Loaded& mod,uint32_t address) {
    std::string path;
    for(uint32_t i=0;i<512;++i){
        if(!setting_buffer(mod,address+i,1))return {};
        char ch=char(ld8(address+i));if(!ch)return path;path+=ch;
    }
    return {};
}
void svc_hud_register(Cpu* c) {
    auto& mod=owner(c);uint32_t callback=c->r[3],screen=c->r[4];c->r[3]=0;
    std::lock_guard lock(g_hud_mutex);
    if(screen>2){hud::store().note(mod.id,"HUD registration failed: invalid screen");return;}
    if(callback) {
        bool found=false;for(uint32_t i=0;i<mod.m->func_count;++i)found|=mod.m->funcs[i].addr==callback;
        if(!found){hud::store().note(mod.id,"HUD registration failed: callback must belong to this mod");return;}
    }
    if(mod.hud_callback!=callback||mod.hud_screen!=screen)mod.hud_called=false;
    mod.hud_callback=callback;mod.hud_screen=screen;
    if(!callback)hud::store().drop(mod.id);
    bool any=false;for(const auto& loaded:g_loaded)any|=loaded.hud_callback!=0;
    g_hud_callbacks.store(any,std::memory_order_release);c->r[3]=1;
}
void svc_hud_texture(Cpu* c) {
    auto& mod=owner(c);uint32_t source=c->r[3];auto path=hud_path(mod,c->r[4]);c->r[3]=0;
    if(source>1||path.empty()){hud::store().note(mod.id,"HUD PNG load failed: invalid source or path");return;}
    // Package textures are deliberately restricted to the two artwork folders.
    if(source==0&&!path.starts_with("assets/")&&!path.starts_with("textures/")){hud::store().note(mod.id,"HUD PNG load failed: package images must be in assets/ or textures/");return;}
    try {
        auto pixels=hud::load_png(source?mod.data_path:mod.package_path,path);
        c->r[3]=hud::store().create_image(mod.id,pixels.width,pixels.height,pixels.width*4,pixels.rgba.data(),pixels.rgba.size());
        if(!c->r[3])hud::store().note(mod.id,"HUD PNG load failed: per-mod image quota exceeded");
    }catch(const std::exception&){hud::store().note(mod.id,"HUD PNG load failed: missing, invalid or oversized texture");}
}
void svc_hud_release(Cpu* c) {c->r[3]=hud::store().release(owner(c).id,c->r[3]);}
void svc_hud_epoch(Cpu* c) {auto epoch=hud::store().state_generation();c->r[3]=uint32_t(epoch>>32);c->r[4]=uint32_t(epoch);}
void svc_hud_record(Cpu* c,bool clip_only) {
    auto& mod=owner(c);uint32_t list=c->r[3],a=c->r[4];c->r[3]=0;
    if(!setting_buffer(mod,a,72)){hud::store().fail(mod.id,list,"HUD draw list dropped: invalid element buffer");return;}
    hud::Command command;
    command.kind=hud::Command::Kind(ld32(a));
    bool clip=command.kind==hud::Command::ClipPush||command.kind==hud::Command::ClipPop;
    if(clip!=clip_only){hud::store().fail(mod.id,list,"HUD draw list dropped: wrong recording service");return;}
    command.anchor=hud::Anchor(ld32(a+4));command.blend=hud::Blend(ld32(a+8));
    command.x=ldf32(a+12);command.y=ldf32(a+16);command.w=ldf32(a+20);command.h=ldf32(a+24);
    command.size=ldf32(a+28);command.thickness=ldf32(a+32);command.rotation=ldf32(a+36);
    command.u0=ldf32(a+40);command.v0=ldf32(a+44);command.u1=ldf32(a+48);command.v1=ldf32(a+52);
    command.rgba=ld32(a+56);uint32_t image=ld32(a+60),text=ld32(a+64),bytes=ld32(a+68);
    if(command.kind==hud::Command::Text) {
        if(bytes>hud::kMaxText||!setting_buffer(mod,text,bytes)){hud::store().fail(mod.id,list,"HUD draw list dropped: invalid text buffer");return;}
        command.text.assign(reinterpret_cast<const char*>(mem::ptr(text)),bytes);
    }
    c->r[3]=command.kind==hud::Command::Picture?hud::store().picture(mod.id,list,image,std::move(command)):
              hud::store().append(mod.id,list,std::move(command));
}
void svc_hud_emit(Cpu* c) {svc_hud_record(c,false);}
void svc_hud_clip(Cpu* c) {svc_hud_record(c,true);}
// Letters (story_mail.h). Strings must lie in the calling mod's memory; too long or unterminated
// strings make the call fail with WWHD_MAIL_INVALID.
bool mod_string(const Loaded& mod,uint32_t address,uint32_t limit,std::string& out) {
    out.clear();
    for(uint32_t i=0;i<=limit;++i){
        if(!setting_buffer(mod,address+i,1))return false;
        char ch=char(ld8(address+i));if(!ch)return true;out+=ch;
    }
    return false;
}
void svc_mail_send(Cpu* c) {
    auto& mod=owner(c);std::string id,sender,text;
    if(!mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)||!mod_string(mod,c->r[4],story_mail::kMaxSenderBytes,sender)||
       !mod_string(mod,c->r[5],story_mail::kMaxTextBytes,text)){c->r[3]=uint32_t(story_mail::kResultInvalid);return;}
    c->r[3]=uint32_t(story_mail::send(mod.id,id,sender,text));
}
void svc_mail_state(Cpu* c) {
    auto& mod=owner(c);std::string id;
    c->r[3]=uint32_t(mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_mail::state(mod.id,id):story_mail::kResultInvalid);
}
// Mod charts (story_chart.h). IDs and labels must lie in the calling mod's memory.
void svc_chart_give(Cpu* c) {
    auto& mod=owner(c);std::string id;
    c->r[3]=uint32_t(mod_string(mod,c->r[3],story_chart::kMaxIdBytes,id)?story_chart::give(mod.id,id):story_chart::kResultInvalid);
}
void svc_chart_finish(Cpu* c) {
    auto& mod=owner(c);std::string id;
    c->r[3]=uint32_t(mod_string(mod,c->r[3],story_chart::kMaxIdBytes,id)?story_chart::finish(mod.id,id):story_chart::kResultInvalid);
}
void svc_chart_state(Cpu* c) {
    auto& mod=owner(c);std::string id;
    c->r[3]=uint32_t(mod_string(mod,c->r[3],story_chart::kMaxIdBytes,id)?story_chart::state(mod.id,id):story_chart::kResultInvalid);
}
// wwhd_chart_mark(chart, marker, x, z, icon, label): r3, r4, f1, f2, r5, r6; a null label is empty
void svc_chart_mark(Cpu* c) {
    auto& mod=owner(c);std::string chart,marker,label;
    if(!mod_string(mod,c->r[3],story_chart::kMaxIdBytes,chart)||!mod_string(mod,c->r[4],story_chart::kMaxIdBytes,marker)||
       (c->r[6]&&!mod_string(mod,c->r[6],story_chart::kMaxLabelBytes,label))){c->r[3]=uint32_t(story_chart::kResultInvalid);return;}
    c->r[3]=uint32_t(story_chart::mark(mod.id,chart,marker,float(c->f[1].ps0),float(c->f[2].ps0),c->r[5],label));
}
// wwhd_chart_unmark(chart, marker): a null marker removes all markers of the chart
void svc_chart_unmark(Cpu* c) {
    auto& mod=owner(c);std::string chart,marker;
    if(!mod_string(mod,c->r[3],story_chart::kMaxIdBytes,chart)||(c->r[4]&&!mod_string(mod,c->r[4],story_chart::kMaxIdBytes,marker))){
        c->r[3]=uint32_t(story_chart::kResultInvalid);return;}
    c->r[3]=uint32_t(story_chart::unmark(mod.id,chart,marker));
}
// The story notebook (story_notebook.h). Texts must lie in the calling mod's memory; null texts are empty.
bool quest_text(const Loaded& mod,uint32_t address,uint32_t limit,std::string& out) {
    out.clear();return !address||mod_string(mod,address,limit,out);
}
void quest_result(Cpu* c,int32_t r) {c->r[3]=uint32_t(r);}
// wwhd_quest_define(quest, title, reward, condition)
void svc_quest_define(Cpu* c) {
    auto& mod=owner(c);std::string id,title,reward,condition;
    if(!mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)||!mod_string(mod,c->r[4],story_notebook::kMaxTitleBytes,title)||
       !quest_text(mod,c->r[5],story_notebook::kMaxRewardBytes,reward)||!quest_text(mod,c->r[6],story_notebook::kMaxConditionBytes,condition))
        return quest_result(c,story_notebook::kResultInvalid);
    quest_result(c,story_notebook::define(mod.id,mod.name,id,title,reward,condition));
}
// wwhd_quest_step(quest, step, next, where)
void svc_quest_step(Cpu* c) {
    auto& mod=owner(c);std::string id,next,where;
    if(!mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)||!quest_text(mod,c->r[5],story_notebook::kMaxNextBytes,next)||
       !quest_text(mod,c->r[6],story_notebook::kMaxWhereBytes,where))
        return quest_result(c,story_notebook::kResultInvalid);
    quest_result(c,story_notebook::step(mod.id,id,c->r[4],next,where));
}
// wwhd_quest_place(quest, x, z): r3, f1, f2
void svc_quest_place(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::place(mod.id,id,float(c->f[1].ps0),float(c->f[2].ps0))
                                                                     :story_notebook::kResultInvalid);
}
void svc_quest_no_place(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::no_place(mod.id,id):story_notebook::kResultInvalid);
}
void svc_quest_known(Cpu* c) {
    auto& mod=owner(c);std::string id,text;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)&&quest_text(mod,c->r[4],story_notebook::kMaxKnownBytes,text)
                       ?story_notebook::known(mod.id,id,text):story_notebook::kResultInvalid);
}
// wwhd_quest_portrait(quest, path): a PNG of the package (assets/ or textures/)
void svc_quest_portrait(Cpu* c) {
    auto& mod=owner(c);std::string id,path;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)&&mod_string(mod,c->r[4],story_notebook::kMaxPortraitBytes,path)
                       ?story_notebook::portrait(mod.id,id,path,mod.package_path):story_notebook::kResultInvalid);
}
void svc_quest_done(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::done(mod.id,id):story_notebook::kResultInvalid);
}
// wwhd_quest_carry(quest, carried)
void svc_quest_carry(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::carry(mod.id,id,c->r[4]!=0):story_notebook::kResultInvalid);
}
// wwhd_quest_talk(quest, step, resident, lines, reward)
void svc_quest_talk(Cpu* c) {
    auto& mod=owner(c);std::string id,resident,lines;
    if(!mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)||!mod_string(mod,c->r[5],48,resident)||!mod_string(mod,c->r[6],story_notebook::kMaxLinesBytes,lines))
        return quest_result(c,story_notebook::kResultInvalid);
    quest_result(c,story_notebook::talk(mod.id,id,c->r[4],resident,lines,c->r[7]));
}
void svc_quest_state(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::state(mod.id,id):story_notebook::kResultInvalid);
}
// wwhd_quest_set_flag(quest, n, on), wwhd_quest_flag(quest, n)
void svc_quest_set_flag(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::set_flag(mod.id,id,c->r[4],c->r[5]!=0):story_notebook::kResultInvalid);
}
void svc_quest_flag(Cpu* c) {
    auto& mod=owner(c);std::string id;
    quest_result(c,mod_string(mod,c->r[3],story_mail::kMaxIdBytes,id)?story_notebook::flag(mod.id,id,c->r[4]):story_notebook::kResultInvalid);
}
const std::unordered_map<std::string, PpcFunc> kServices = {
    {"wwhd_audio_epoch",svc_audio_epoch},{"wwhd_audio_open",svc_audio_open},
    {"wwhd_audio_available",svc_audio_available},{"wwhd_audio_submit",svc_audio_submit},{"wwhd_audio_close",svc_audio_close},
    {"wwhd_log", svc_log},       {"wwhd_log_int", svc_log_int}, {"wwhd_log_hex", svc_log_hex},
    {"wwhd_log_float", svc_log_float}, {"wwhd_config_int", svc_config_int},
    {"wwhd_config_bool",svc_config_bool},{"wwhd_config_float",svc_config_float},{"wwhd_config_string",svc_config_string},
    {"wwhd_malloc",svc_malloc},{"wwhd_free",svc_free},{"wwhd_input_read",svc_input},
    {"wwhd_file_read",svc_file_read},{"wwhd_file_write",svc_file_write},
    {"wwhd_hud_register",svc_hud_register},{"wwhd_hud_texture",svc_hud_texture},
    {"wwhd_hud_clip",svc_hud_clip},{"wwhd_hud_release",svc_hud_release},{"wwhd_hud_epoch",svc_hud_epoch},{"wwhd_hud_emit",svc_hud_emit},
    {"wwhd_setting_get",svc_setting_get},{"wwhd_setting_changed",svc_setting_changed},
    {"wwhd_logic_dt",svc_logic_dt},{"wwhd_logic_step",svc_logic_step},
    {"wwhd_mail_send",svc_mail_send},{"wwhd_mail_state",svc_mail_state},
    {"wwhd_chart_give",svc_chart_give},{"wwhd_chart_finish",svc_chart_finish},{"wwhd_chart_state",svc_chart_state},
    {"wwhd_chart_mark",svc_chart_mark},{"wwhd_chart_unmark",svc_chart_unmark},
    {"wwhd_quest_define",svc_quest_define},{"wwhd_quest_step",svc_quest_step},{"wwhd_quest_place",svc_quest_place},
    {"wwhd_quest_no_place",svc_quest_no_place},{"wwhd_quest_known",svc_quest_known},{"wwhd_quest_portrait",svc_quest_portrait},
    {"wwhd_quest_done",svc_quest_done},{"wwhd_quest_state",svc_quest_state},
    {"wwhd_quest_carry",svc_quest_carry},{"wwhd_quest_talk",svc_quest_talk},
    {"wwhd_quest_set_flag",svc_quest_set_flag},{"wwhd_quest_flag",svc_quest_flag},
    {"memcpy", svc_memcpy},      {"memmove", svc_memcpy},       {"memset", svc_memset},
};
PpcFunc service(const char* name) {
    auto it = kServices.find(name);
    if (it == kServices.end()) return nullptr;
    if (!strncmp(name, "wwhd_mail_", 10)) story_mail::note_import();  // the postbox hooks are needed
    if (!strncmp(name, "wwhd_chart_", 11)) {  // the sea chart hooks are needed, and the chart list's captions
        story_chart::note_import();
        story_mail::note_import();
    }
    if (!strncmp(name, "wwhd_quest_", 11)) {  // the notebook's item menu entry, and its marker on the sea chart
        story_notebook::note_import();
        story_chart::note_import();
        story_mail::note_import();  // residents' lines go through the message hooks
    }
    return it->second;
}

bool g_story_package=false;  // (this fork: outside the native-mods block below)

#if !WWHD_FORK_NO_NATIVE_GUEST_MODS
const WWHDGuestHostV1 kHost = {
    sizeof(WWHDGuestHostV1), WWHD_GUEST_ABI_VERSION,
    ppc_dispatch, ppc_unimplemented, ppc_trap, ppc_timebase, ppc_fres, ppc_frsqrte, ppc_preempt,
    g_core_preempt, call_original, service,
};

// The NPC catalogue's names and spots come from the player's own game files: generated once by
// tools/story_mods/npc_catalogue.py (next to the guest builder) into the port's data folder, for the
// compiled-in recipes (story_npc_catalogue.h); generated again when the recipes change.
void prepare_npc_catalogue() {
    static bool done=false;
    if(done)return;
    done=true;
    std::string recipes;for(const auto& r:story_notebook::npc_recipes())recipes+=r.second;
    char tag[17];snprintf(tag,sizeof tag,"%016llx",(unsigned long long)std::hash<std::string>{}(recipes));
    const char* forced=getenv("WWHD_NPC_CATALOGUE");
    const std::filesystem::path dir=std::filesystem::path(host::config_dir())/"story";
    const std::filesystem::path file=forced&&*forced?std::filesystem::path(forced):dir/("npc_catalogue-"+std::string(tag)+".json");
    std::error_code ec;
    if(!std::filesystem::is_regular_file(file,ec)) {
        try {
            const auto bridge=BuildBridge::installed(std::string());
            const auto tool=std::filesystem::path(bridge.builder).parent_path().parent_path()/"story_mods"/"npc_catalogue.py";
            std::filesystem::create_directories(dir/"recipes",ec);
            auto command=bridge.python;command.push_back(tool.string());command.insert(command.end(),{"--game",config::game_dir,"--out",file.string(),"--json"});
            for(const auto& [place,text]:story_notebook::npc_recipes()) {
                const auto recipe=dir/"recipes"/(place+".recipe.json");
                std::ofstream(recipe,std::ios::binary)<<text;
                command.insert(command.end(),{"--recipe",recipe.string()});
            }
            const auto process=host::run_process(command);
            LOG("[notebook] NPC catalogue generated from the game files: %s",process.code==0?"ok":(process.output+process.error).c_str());
        } catch(const std::exception& e) {LOG("[notebook] NPC catalogue not generated: %s",e.what());}
    }
    if(!story_notebook::load_local_npc_catalogue(file.string()))
        LOG("[notebook] NPC catalogue: no names from the game yet (residents show their description and have no spot)");
}

// The story notebook's game model, built from the Story Notebook package's .glb with the player's own
// stand-in item archive (tools/story_mods/notebook_model.py, next to the guest builder) into the
// package's data folder; built again when the .glb changes. Empty when it can't be built (the game's
// stand-in model shows).
std::string notebook_model(const std::filesystem::path& glb,const std::string& data_dir) {
    std::error_code ec;
    if(data_dir.empty()||!std::filesystem::is_regular_file(glb,ec))return {};
    std::ifstream in(glb,std::ios::binary);std::string bytes{std::istreambuf_iterator<char>(in),{}};
    char tag[17];snprintf(tag,sizeof tag,"%016llx",(unsigned long long)std::hash<std::string>{}(bytes));
    const auto out=std::filesystem::path(data_dir)/("notebook_model-"+std::string(tag)+".szs");
    if(std::filesystem::is_regular_file(out,ec))return out.string();
    try {
        const auto bridge=BuildBridge::installed(std::string());
        const auto tool=std::filesystem::path(bridge.builder).parent_path().parent_path()/"story_mods"/"notebook_model.py";
        std::filesystem::create_directories(data_dir,ec);
        auto command=bridge.python;command.push_back(tool.string());
        command.insert(command.end(),{"--game",config::game_dir,"--glb",glb.string(),"--out",out.string()});
        const auto process=host::run_process(command);
        LOG("[notebook] the notebook's model built from %s: %s",glb.filename().string().c_str(),process.code==0?"ok":(process.output+process.error).c_str());
    } catch(const std::exception& e) {LOG("[notebook] the notebook's model not built: %s",e.what());}
    return std::filesystem::is_regular_file(out,ec)?out.string():std::string();
}
#endif

bool load_one(const std::string& path, std::string& err,const mods::packages::GuestPackage& pkg,uint32_t base,uint32_t reserved) {
#if WWHD_FORK_NO_NATIVE_GUEST_MODS
    (void)path, (void)pkg, (void)base, (void)reserved;
    err = "native guest mods are not supported by this port (they are built for upstream's game code)";
    return false;
#else
#ifdef _WIN32
    void* lib = (void*)LoadLibraryA(path.c_str());
    auto init = lib ? (WWHDGuestInitV1)(void*)GetProcAddress((HMODULE)lib, WWHD_GUEST_INIT_SYMBOL) : nullptr;
#else
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto init = lib ? (WWHDGuestInitV1)dlsym(lib, WWHD_GUEST_INIT_SYMBOL) : nullptr;
#endif
    if (!lib) { err = "cannot load the module"; return false; }
    struct LibraryGuard {
        void* handle;
        ~LibraryGuard() {
            if (!handle) return;
#ifdef _WIN32
            FreeLibrary((HMODULE)handle);
#else
            dlclose(handle);
#endif
        }
    } guard{lib};
    if (!init) { err = "not a guest mod module (no " WWHD_GUEST_INIT_SYMBOL ")"; return false; }
    const WWHDGuestModuleV1* m = init(&kHost);
    if (!m || m->size < sizeof(WWHDGuestModuleV1) || m->abi_version != WWHD_GUEST_ABI_VERSION) {
        err = "module ABI does not match this game version: rebuild the mod";
        return false;
    }
    if(base<0x7F000000||base>=0x80000000||reserved>0x80000000-base||m->mem_base!=base||m->mem_size>reserved||pkg.heap_size>reserved-m->mem_size){err="module does not fit its assigned guest region";return false;}
    if (!validate_module(*m, pkg.id,
            [](uint32_t addr) { return ordinal_of(addr) >= 0; },
            [](uint32_t addr) {
                auto it = g_chains.find(addr);
                return it == g_chains.end() ? std::string() : it->second.replace_mod;
            }, err)) return false;
    for (auto& o : g_loaded)
        if (m->mem_base < o.m->mem_base + o.region_size && o.m->mem_base < m->mem_base + reserved) {
            err = "module memory overlaps " + o.path;
            return false;
        }
    Loaded loaded;
    loaded.path=path;loaded.package_path=pkg.path;loaded.data_path=pkg.data_path;loaded.id=pkg.id;loaded.name=pkg.name;loaded.version=pkg.version;loaded.m=m;loaded.region_size=reserved;loaded.options=pkg.options;
    loaded.files=std::make_unique<Files>(pkg.data_path);
    uint32_t heap_base=(m->mem_base+m->mem_size+15)&~15u;
    loaded.heap=std::make_unique<Heap>(mem::ptr(heap_base),heap_base,pkg.heap_size);
    memset(mem::ptr(m->mem_base), 0, reserved);
    loaded.heap->initialize();
    if (m->image_size) memcpy(mem::ptr(m->mem_base), m->image, m->image_size);
    for (uint32_t i = 0; i < m->func_count; i++) dispatch::set(m->funcs[i].addr, m->funcs[i].fn);
    for (uint32_t i = 0; i < m->hook_count; i++) {
        const WWHDGuestHook& h = m->hooks[i];
        Chain& ch = g_chains[h.target];
        ch.addr = h.target;
        ch.ordinal = (uint32_t)ordinal_of(h.target);
        if (h.kind == WWHD_GUEST_REPLACE) { ch.replace = h.fn; ch.replace_mod = pkg.id; }
        else if (h.kind == WWHD_GUEST_HOOK_ENTRY) ch.entry.push_back(h.fn);
        else ch.ret.insert(ch.ret.begin(), h.fn);  // return hooks run in reverse load order
        g_mod_hook_flags[ch.ordinal] = 1;
        LOG("[guestmods] %s %08X", h.kind == WWHD_GUEST_REPLACE ? "replace" : h.kind == WWHD_GUEST_HOOK_ENTRY ? "entry hook" : "return hook",
            h.target);
    }
    // story quests declared in the manifest (story_notebook.h): known without running the mod's code.
    // The story system runs only with its required package, the Story Notebook (it brings the notebook
    // and its look); without it no mod's quests are declared.
    try {
        std::ifstream manifest_file(std::filesystem::path(pkg.path)/"manifest.json",std::ios::binary);
        std::string manifest_text{std::istreambuf_iterator<char>(manifest_file),{}};
        const auto manifest=mods::json::parse(manifest_text);
        if(loaded.id==story_notebook::kNotebookPackage) {
            g_story_package=true;
            const auto& look=manifest.get("notebook");
            const std::string icon=look.get("icon").string(),model=look.get("model").string();
            story_notebook::set_look(icon.empty()?std::string():(std::filesystem::path(pkg.path)/icon).string(),
                                     model.empty()?std::string():notebook_model(std::filesystem::path(pkg.path)/model,pkg.data_path));
        }
        if(manifest_text.find("\"quests\"")!=std::string::npos||manifest_text.find("\"charts\"")!=std::string::npos) {
            prepare_npc_catalogue();
            story_notebook::declare(loaded.id,loaded.name,manifest);
        }
    } catch(const std::exception& e) {LOG("[guestmods] %s: manifest quests not read: %s",loaded.id.c_str(),e.what());}
    g_loaded.push_back(std::move(loaded));
    guard.handle = nullptr; // module functions remain resident for the process lifetime
    LOG("[guestmods] loaded %s (translator %s): %u functions, %u hooks, guest memory %08X-%08X", path.c_str(),
        m->translator, m->func_count, m->hook_count, m->mem_base, m->mem_base + m->mem_size);
    return true;
#endif
}

struct Args {
    uint32_t r[8];
    double f[8][2];
    void save(const Cpu* c) {
        for (int i = 0; i < 8; i++) { r[i] = c->r[3 + i]; f[i][0] = c->f[1 + i].ps0; f[i][1] = c->f[1 + i].ps1; }
    }
    void load(Cpu* c) const {
        for (int i = 0; i < 8; i++) { c->r[3 + i] = r[i]; c->f[1 + i].ps0 = f[i][0]; c->f[1 + i].ps1 = f[i][1]; }
    }
};

}  // namespace

void init() {
    namespace packages=mods::packages;
    // start_guests invokes callbacks under the package-manager mutex. Resolve its
    // directory before entering those callbacks to avoid recursively locking it.
    const auto cache=(std::filesystem::path(packages::directory()).parent_path()/"GuestBuild").string();
    auto inspect=[cache](const packages::GuestPackage& pkg) {
        if(!g_mod_hook_count)exception_report::raise("Guest mods require code-mod support; rebuild and restart first");
        auto result=BuildBridge::installed(cache).run(pkg.path,0x7F000000,true,g_guest_build_name);
        if(result.get("elf_sha256").string()!=pkg.fingerprint)exception_report::raise("Guest ELF changed; review its trust confirmation again");
        const auto& size=result.get("allocation_size");
        if(size.type!=mods::json::Value::Number||size.number<=0||size.number>0x1000000||std::floor(size.number)!=size.number)
            exception_report::raise("Invalid guest module memory requirement");
        uint32_t module_bytes=uint32_t(size.number);
        if(pkg.heap_size>0x1000000-module_bytes)exception_report::raise("Guest module and heap exceed the mod region");
        return (module_bytes+pkg.heap_size+0xFFFF)&~0xFFFFu;
    };
    auto build=[cache](const packages::GuestPackage& pkg,uint32_t base) {
        if(!g_mod_hook_count)exception_report::raise("Guest mods require code-mod support; rebuild and restart first");
        auto result=BuildBridge::installed(cache).run(pkg.path,base,false,g_guest_build_name);
        if(result.get("elf_sha256").string()!=pkg.fingerprint)exception_report::raise("Guest ELF changed; review its trust confirmation again");
        const auto& memory=result.get("allocation_size");
        if(memory.type!=mods::json::Value::Number||memory.number<=0||memory.number>0x1000000||std::floor(memory.number)!=memory.number||pkg.heap_size>0x1000000-uint32_t(memory.number))
            exception_report::raise("Invalid guest module allocation size");
        uint32_t reserved=(uint32_t(memory.number)+pkg.heap_size+0xFFFF)&~0xFFFFu;
        return packages::GuestBuilt{result.get("module").string(),reserved};
    };
    packages::set_guest_builder(inspect,build,[cache](const mods::json::Value& requests) {
        auto result=BuildBridge::installed(cache).check_cached(requests,g_guest_build_name);
        const auto& valid=result.get("valid");
        if(valid.type!=mods::json::Value::Array)exception_report::raise("Invalid guest cache check result");
        std::vector<std::string> ids;for(const auto& id:valid.array) {
            if(id.type!=mods::json::Value::String)exception_report::raise("Invalid guest cache check ID");
            ids.push_back(id.text);
        }
        return ids;
    });
    packages::start_guests(inspect,[build](const packages::GuestPackage& pkg,uint32_t base) {
        auto result=build(pkg,base);std::string error;
        if(result.module.empty()||!load_one(result.module,error,pkg,base,result.allocation_size))
            exception_report::raise(error.empty()?"Guest builder returned no module":error);
    });
    // The story system runs only with its required package, the Story Notebook (packages.cpp makes every
    // quest mod depend on it); without it: no letters, takeovers, chart marks or notebook, the game as it is.
    if((story_mail::imported()||story_chart::imported()||story_notebook::imported())&&!g_story_package) {
        LOG("[guestmods] the story system is inactive: the Story Notebook package (%s) isn't installed",story_notebook::kNotebookPackage.c_str());
        return;
    }
    // Letters from mods: host replacements of the postbox and message functions, only when a loaded
    // mod uses the letter services (story_mail.h).
    if(story_mail::imported())story_mail::set_text_buffers(mem::host_alloc(8192,16),8192);
    // Story progress (story_store.h): its areas in guest memory, so full save states carry them; saved
    // with the game's saves in the story block of cking.sav. Without a story mod the file's block is
    // still kept as it is.
    const bool story=story_mail::imported()||story_chart::imported()||story_notebook::imported();
    if(story){uint32_t areas=mem::host_alloc(story_store::kMemoryBytes,16);memset(mem::ptr(areas),0,story_store::kMemoryBytes);story_store::set_memory(areas);}
    // developer override of the notebook's model archive (the Story Notebook package's setup step builds it)
    if(const char* model=getenv("WWHD_NOTEBOOK_MODEL");model&&*model)story_mail::set_notebook_model(model);
    story_mail::set_game_calls(call_original,[](Cpu* c,uint32_t fn,std::initializer_list<uint32_t> args){return guest_call(c,fn,args);});
    story_mail::install([](uint32_t addr) {
        auto it=g_chains.find(addr);return it==g_chains.end()?std::string():it->second.replace_mod;
    },[](uint32_t addr,PpcFunc fn) {
        int ordinal=ordinal_of(addr);
        if(ordinal<0)fatal("[guestmods] letters: %08X is not a game function",addr);
        Chain& ch=g_chains[addr];
        ch.addr=addr;ch.ordinal=uint32_t(ordinal);ch.replace=fn;ch.replace_mod="the port's letter service";
        g_mod_hook_flags[ordinal]=1;
    });
    // Mod charts: host replacements of the sea chart menu's story marks, only when a loaded mod uses
    // the chart services (story_chart.h).
    story_chart::set_game_calls(call_original,[](Cpu* c,uint32_t fn,std::initializer_list<uint32_t> args){return guest_call(c,fn,args);});
    if(story_chart::imported()){
        story_chart::set_entry_memory(mem::host_alloc(story_chart::kMaxCharts*0x28,16),story_chart::kMaxCharts*0x28);
        story_chart::set_caption_text([](uint32_t number,const std::u16string* text){if(text)story_mail::caption(number,text);else story_mail::uncaption();});
    }
    story_chart::install([](uint32_t addr) {
        auto it=g_chains.find(addr);return it==g_chains.end()?std::string():it->second.replace_mod;
    },[](uint32_t addr,PpcFunc fn) {
        int ordinal=ordinal_of(addr);
        if(ordinal<0)fatal("[guestmods] charts: %08X is not a game function",addr);
        Chain& ch=g_chains[addr];
        ch.addr=addr;ch.ordinal=uint32_t(ordinal);ch.replace=fn;ch.replace_mod="the port's chart service";
        g_mod_hook_flags[ordinal]=1;
    });
    // The story notebook: the item menu's Delivery Bag shows it and opens its page, only when a loaded
    // mod uses the quest services (story_notebook.h).
    story_notebook::set_game_ui(&story_notebook::game_ui());
    story_notebook::set_game_calls(call_original,[](Cpu* c,uint32_t fn,std::initializer_list<uint32_t> args){return guest_call(c,fn,args);});
    story_notebook::install([](uint32_t addr) {
        auto it=g_chains.find(addr);return it==g_chains.end()?std::string():it->second.replace_mod;
    },[](uint32_t addr,PpcFunc fn) {
        int ordinal=ordinal_of(addr);
        if(ordinal<0)fatal("[guestmods] notebook: %08X is not a game function",addr);
        Chain& ch=g_chains[addr];
        ch.addr=addr;ch.ordinal=uint32_t(ordinal);ch.replace=fn;ch.replace_mod="the port's story notebook";
        g_mod_hook_flags[ordinal]=1;
    });
    // The King of Red Lions' one-time line for the tracked quest (story_sail.h): the boat's execute,
    // checkForceMessage and procTalk_init, only when a declared quest step has a sail_line.
    story_notebook::install_sail([](uint32_t addr) {
        auto it=g_chains.find(addr);return it==g_chains.end()?std::string():it->second.replace_mod;
    },[](uint32_t addr,PpcFunc fn) {
        int ordinal=ordinal_of(addr);
        if(ordinal<0)fatal("[guestmods] notebook: %08X is not a game function",addr);
        Chain& ch=g_chains[addr];
        ch.addr=addr;ch.ordinal=uint32_t(ordinal);ch.replace=fn;ch.replace_mod="the port's story notebook (King of Red Lions)";
        g_mod_hook_flags[ordinal]=1;
    });
    story_store::set_game_calls(call_original);
    story_store::install([](uint32_t addr) {
        auto it=g_chains.find(addr);return it==g_chains.end()?std::string():it->second.replace_mod;
    },[](uint32_t addr,PpcFunc fn) {
        int ordinal=ordinal_of(addr);
        if(ordinal<0)fatal("[guestmods] story store: %08X is not a game function",addr);
        Chain& ch=g_chains[addr];
        ch.addr=addr;ch.ordinal=uint32_t(ordinal);ch.replace=fn;ch.replace_mod="the port's story progress";
        g_mod_hook_flags[ordinal]=1;
    });
    if(getenv("WWHD_GUEST_MODS"))LOG("[guestmods] WWHD_GUEST_MODS is retired; install and trust guest packages in the mod manager");
}

std::vector<ModIdentity> enabled_mods() {
    std::vector<ModIdentity> mods;for(const auto& mod:g_loaded)mods.push_back({mod.id,mod.version});return mods;
}

bool hooks_built() { return g_mod_hook_count != 0 && g_mod_hook_flags && g_mod_bodies; }

void frame(uint64_t step) {g_logic_step.store(step,std::memory_order_relaxed);story_notebook::logic_step(step);}
void draw_frame(Cpu* c,uint64_t step) {
    if(!c||!g_hud_callbacks.load(std::memory_order_acquire))return;
    for(auto& mod:g_loaded) {
        uint32_t callback,screen;
        {
            std::lock_guard lock(g_hud_mutex);
            if(!mod.hud_callback||(mod.hud_called&&mod.hud_step==step))continue;
            mod.hud_step=step;mod.hud_called=true;callback=mod.hud_callback;screen=mod.hud_screen;
        }
        auto list=hud::store().begin(mod.id,screen);
        if(!list){hud::store().drop(mod.id);continue;}
        Cpu saved=*c;
        try {guest_call(c,callback,{list});hud::store().commit(mod.id,list);}
        catch(...) {hud::store().fail(mod.id,list,"HUD callback failed; draw list dropped");hud::store().commit(mod.id,list);}
        *c=saved;
    }
}
void state_loaded() {
    std::lock_guard lock(g_hud_mutex);
    hud::store().reset();
    pcm::store().reset();
    story_mail::state_loaded();
    story_chart::state_loaded();
    story_notebook::state_loaded();
    for(auto& mod:g_loaded)mod.hud_called=false;
}

}  // namespace guestmods

using namespace guestmods;

// A hooked or replaced game function. The calling convention is the game's: hooks get the function's
// arguments (r3..r10, f1..f8 are restored for every hook), a return hook also leaves the return value
// (r3, r4, f1) as the function produced it. r1/r2/r13 are preserved by every callee (ABI).
extern "C" void ppc_mod_run(Cpu* c) {
    uint32_t addr = c->pc;
    auto it = g_chains.find(addr);
    if (it == g_chains.end()) fatal("[guestmods] %08X flagged without hooks", addr);
    const Chain& ch = it->second;
    Args a;
    a.save(c);
    for (PpcFunc h : ch.entry) {
        h(c);
        a.load(c);
    }
    if (ch.replace) ch.replace(c);
    else { c->mod_skip = addr; g_mod_bodies[ch.ordinal](c); }
    if (!ch.ret.empty()) {
        uint32_t r3 = c->r[3], r4 = c->r[4];
        double f1a = c->f[1].ps0, f1b = c->f[1].ps1;
        for (PpcFunc h : ch.ret) {
            a.load(c);
            h(c);
        }
        c->r[3] = r3; c->r[4] = r4; c->f[1].ps0 = f1a; c->f[1].ps1 = f1b;
    }
}

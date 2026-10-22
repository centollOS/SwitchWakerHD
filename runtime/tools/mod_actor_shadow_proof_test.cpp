#include "mods/mod_actor_shadow_proof.h"
#include "runtime.h"
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
namespace {
constexpr uint32_t Arena=0x20000000,Packet=Arena+0x100,Vtable=Arena+0x200,Info=Arena+0x300,Target=0x027F2000;
std::map<uint32_t,PpcFunc> hooks;unsigned geometry_reports=0,forwards=0;bool test_cascade=false;
void env(const char* key,const char* value){
#ifdef _WIN32
_putenv_s(key,value);
#else
setenv(key,value,1);
#endif
}
void original(Cpu*c,uint32_t address){++forwards;
if(address==0x02737D54&&test_cascade){Cpu nested=*c;nested.r[3]=Packet;hooks.at(Target)(&nested);}
if(address==Target){mod_actor_shadow_proof::draw_submitted(36);mod_actor_shadow_proof::draw_submitted(0);c->r[3]=42;}
}
uint32_t call(Cpu*,uint32_t,std::initializer_list<uint32_t>){return 0;}
void dummy(Cpu*){}
}
void log_msg(const char* format,...){if(std::string(format).find("gx2_draws=")!=std::string::npos){va_list args;va_start(args,format);char text[512];vsnprintf(text,sizeof(text),format,args);va_end(args);assert(std::string(text).find("gx2_draws=1 elements=36")!=std::string::npos);++geometry_reports;}}
namespace dispatch {PpcFunc lookup(uint32_t address){return address==Target?dummy:nullptr;}}
int main(){
#ifdef _WIN32
assert(VirtualAlloc(mem::ptr(Arena),0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
#else
assert(mmap(mem::ptr(Arena),0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=MAP_FAILED);
#endif
st32(Packet+0xC,Vtable);st32(Vtable+0x2C,Target);st32(Info,2);
mod_actor_shadow_proof::configure(original,call);auto owner=[](uint32_t){return std::string();};auto replace=[](uint32_t address,PpcFunc function){hooks[address]=function;};
env("WWHD_MOD_ACTOR_SHADOW_PROOF","0");assert(!mod_actor_shadow_proof::install(owner,replace)&&hooks.empty());
env("WWHD_MOD_ACTOR_SHADOW_PROOF","1");env("WWHD_MOD_ACTOR_SHADOW_DRAW_FUNCTIONS","ffffffff");assert(!mod_actor_shadow_proof::install(owner,replace)&&hooks.empty());
env("WWHD_MOD_ACTOR_SHADOW_DRAW_FUNCTIONS","027f2000");assert(mod_actor_shadow_proof::install(owner,replace)&&hooks.size()==5);
Cpu cpu{};cpu.r[4]=Packet;mod_actor_shadow_proof::begin_model(123);hooks.at(0x027F0C14)(&cpu);mod_actor_shadow_proof::end_model();
cpu.r[3]=Packet;hooks.at(Target)(&cpu);assert(cpu.r[3]==42&&geometry_reports==0);
test_cascade=true;cpu.r[4]=Info;hooks.at(0x02737D54)(&cpu);assert(geometry_reports==1);
mod_actor_shadow_proof::forget_model(456);cpu.r[4]=Info;hooks.at(0x02737D54)(&cpu);assert(geometry_reports==2);
mod_actor_shadow_proof::forget_model(123);cpu.r[4]=Info;hooks.at(0x02737D54)(&cpu);assert(geometry_reports==2);
cpu.r[4]=Packet;mod_actor_shadow_proof::begin_model(123);hooks.at(0x027F0C14)(&cpu);mod_actor_shadow_proof::end_model();
mod_actor_shadow_proof::state_loaded();cpu.r[4]=Info;hooks.at(0x02737D54)(&cpu);assert(geometry_reports==2&&forwards>=4);
}

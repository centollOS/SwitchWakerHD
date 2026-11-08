// Exercise production Link hooks using synthetic state only.
#include "runtime.h"
#include "mods/mods.h"
#include <cassert>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
constexpr uint32_t actor=0x16000000;
static float step=1, factor=1;
static bool flying=false;
namespace true60 {
float dt(){return step;} uint32_t link(){return actor;} uint32_t exec_proc(){return actor;}
bool half_pass(){return step<1;} bool enabled(){return step<1;} bool preview(){return step<1;}
uint64_t pass(){return 1;}
}
namespace dispatch { uint32_t register_host(PpcFunc,const char*){return 0;} }
namespace mods {
float link_move_factor(uint32_t){return factor;}
void move_start_effect(Cpu*,uint32_t){} void move_trail_effect(Cpu*,uint32_t){} void move_swim_effect(Cpu*,uint32_t){}
SidleScope sidle_scope_begin(Cpu*,bool){return {};} void sidle_scope_end(const SidleScope&){}
bool moon_jump_move(uint32_t link,float& x,float& z){if(!flying || link!=actor)return false;x=40;z=-10;return true;}
}
extern "C" void site_023FD39C(Cpu*);
extern "C" void f_02055B64_orig(Cpu*){}
extern "C" void f_0207A9A0_orig(Cpu*){}
extern "C" void f_0211D2F8_orig(Cpu*){}
extern "C" void f_022ED850_orig(Cpu*){}
extern "C" void f_023FB230_orig(Cpu*){}
extern "C" void f_023FBCEC_orig(Cpu*){}
extern "C" void f_02416230_orig(Cpu*){}
extern "C" void f_025AAE08_orig(Cpu*){}
static unsigned demo_commands = 0;
extern "C" void f_023F22FC_orig(Cpu*){++demo_commands;}
extern "C" void hook_023F22FC(Cpu*);
extern "C" void f_023F695C_orig(Cpu*){}
extern "C" void f_023F7820_orig(Cpu*){}
extern "C" void f_023F81A4_orig(Cpu*){}
extern "C" void f_023F8F80_orig(Cpu*){}
extern "C" void f_023FA578_orig(Cpu*){}
extern "C" void f_023FB020_orig(Cpu*){}
extern "C" void f_023DBDD0_orig(Cpu*){}
extern "C" void f_023FD4E4_orig(Cpu*){}
extern "C" void f_023DC7AC_orig(Cpu*){}
extern "C" void f_025E14A8_orig(Cpu*){}
extern "C" void f_025DFAB8_orig(Cpu*){}
extern "C" void f_02821448_orig(Cpu*){}
extern "C" void f_0200E240_orig(Cpu*){}
extern "C" void f_0253EC0C_orig(Cpu*){}
extern "C" void f_0253ED80_orig(Cpu*){}
extern "C" void f_025F0658_orig(Cpu*){}
extern "C" void f_025B8AF4_orig(Cpu*){}
extern "C" void f_025B51DC_orig(Cpu*){}
static void map(uint32_t address,size_t size){
#ifdef _WIN32
 auto p=VirtualAlloc(mem::ptr(address),size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
 auto p=mmap(mem::ptr(address),size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
 assert(p==mem::ptr(address));
}
int main(int argc,char** argv){
 // Event commands can destroy model heaps; the actual preview fence must keep
 // them on full steps while leaving the number of half-step previews intact.
 if (argc==2 && !std::strcmp(argv[1],"--event-fence")) {
  Cpu event_cpu{};
  for (unsigned pass=0;pass<60;++pass) {
   step=(pass&1)?.5f:1.0f;
   hook_023F22FC(&event_cpu);
  }
  assert(demo_commands==30);
  return 0;
 }
 map(actor,0x10000);map(mem::kFixedStart,0x10000);
 Cpu cpu{};cpu.r[4]=actor+0x33C;cpu.f[31].ps0=10;
 stf32(cpu.r[4],3);stf32(cpu.r[4]+4,12);stf32(cpu.r[4]+8,4);
 site_023FD39C(&cpu);assert(cpu.r[4]==actor+0x33C); // exact stock argument retained
 flying=true;site_023FD39C(&cpu);
 assert(ldf32(cpu.r[4])==40 && ldf32(cpu.r[4]+4)==12 && ldf32(cpu.r[4]+8)==-10);
 cpu.r[4]=actor+0x33C;step=.5f;site_023FD39C(&cpu);
 assert(ldf32(cpu.r[4])==20 && ldf32(cpu.r[4]+4)==6.5 && ldf32(cpu.r[4]+8)==-5);
 // Stock true60 gravity correction and run boost remain intact with moon jump off.
 cpu.r[4]=actor+0x33C;flying=false;factor=2;site_023FD39C(&cpu);
 assert(ldf32(cpu.r[4])==3 && ldf32(cpu.r[4]+4)==6.5 && ldf32(cpu.r[4]+8)==4);
}

#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__SWITCH__)
#include <switch.h>
#include <pthread.h>
#include <unistd.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <mach-o/ldsyms.h>
#include <mach-o/dyld.h>
#include <climits>
#include <pthread/qos.h>
#elif !defined(_WIN32) && !defined(__SWITCH__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
namespace host {
#if defined(__APPLE__) && defined(WWHD_HAS_VULKAN)
// Render command batches can create Objective-C temporaries inside MoltenVK.
void with_autorelease_pool(void (*fn)());
void with_autorelease_pool(void (*fn)(void*), void* context);
#else
inline void with_autorelease_pool(void (*fn)()) { fn(); }
inline void with_autorelease_pool(void (*fn)(void*), void* context) { fn(context); }
#endif
// The native call is synchronous: captured references remain valid and no heap
// allocation or callable lifetime extension is needed. Exceptions propagate.
template<class Fn> void with_autorelease_pool(Fn&& fn) {
 auto call = [&] { fn(); };
 with_autorelease_pool([](void* context) {
  (*static_cast<decltype(call)*>(context))();
 }, &call);
}
inline thread_local std::string thread_label;
inline void set_thread_name(const char* name) {
 thread_label=name;
#ifdef __APPLE__
 pthread_setname_np(name);
#elif defined(_WIN32)
 using SetDescription=HRESULT(WINAPI*)(HANDLE,PCWSTR);
 auto f=(SetDescription)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"),"SetThreadDescription");
 if(f) { std::wstring text; for(unsigned char c:thread_label)text.push_back(c); f(GetCurrentThread(),text.c_str()); }
#elif defined(__SWITCH__)
 (void)name;
#else
 pthread_setname_np(pthread_self(),thread_label.substr(0,15).c_str());
#endif
}
// Game and render threads: keep them on fast cores and ahead of background work. macOS: QoS
// user-interactive (the default QoS let macOS park them on efficiency cores). Windows: above-normal
// priority and no power throttling (hybrid P/E-core CPUs otherwise move busy threads to E-cores).
// Linux: a small nice boost where the process may raise priority (needs CAP_SYS_NICE; otherwise a
// no-op). WWHD_NO_QOS=1 leaves the thread untouched on every platform.
inline void boost_thread_priority() {
 if(getenv("WWHD_NO_QOS")) return;
#ifdef __APPLE__
 pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#elif defined(_WIN32)
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
 // SetThreadInformation(ThreadPowerThrottling) exists from Windows 10 1709; looked up at run time
 struct PowerThrottling { ULONG Version, ControlMask, StateMask; };
 using SetInfo=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
 static const auto set_info=(SetInfo)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"),"SetThreadInformation");
 if(set_info) {
  PowerThrottling state{1 /* THREAD_POWER_THROTTLING_CURRENT_VERSION */,1 /* EXECUTION_SPEED */,0 /* off */};
  set_info(GetCurrentThread(),3 /* ThreadPowerThrottling */,&state,sizeof state);
 }
#elif defined(__SWITCH__)
 // nothing: Horizon priorities and cores are set by raise_thread_priority / place_thread
#else
 setpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid),-5);  // EPERM without CAP_SYS_NICE: ignored
#endif
}
inline void get_thread_name(char* out,size_t size) {
 if(!size)return;
#ifdef __APPLE__
 pthread_getname_np(pthread_self(),out,size);
#else
 snprintf(out,size,"%s",thread_label.empty()?"host":thread_label.c_str());
#endif
}
inline uintptr_t executable_base() {
#ifdef __APPLE__
 return (uintptr_t)&_mh_execute_header;
#elif defined(_WIN32)
 return (uintptr_t)GetModuleHandleW(nullptr);
#elif defined(__SWITCH__)
 // the NRO's code segment: the mapping containing this function
 MemoryInfo info{}; u32 page=0;
 return R_SUCCEEDED(svcQueryMemory(&info,&page,(u64)(uintptr_t)&executable_base))?(uintptr_t)info.addr:0;
#else
 static int anchor;
 Dl_info info{}; return dladdr(&anchor,&info)?(uintptr_t)info.dli_fbase:0;
#endif
}
inline std::string executable_path() {
#ifdef _WIN32
 char path[32768]; DWORD n=GetModuleFileNameA(nullptr,path,sizeof path);return std::string(path,n);
#elif !defined(__APPLE__) && !defined(__SWITCH__)
 char path[4096];ssize_t n=readlink("/proc/self/exe",path,sizeof path);return n>0?std::string(path,n):std::string();
#else
 return {};
#endif
}
inline size_t page_size() {
#ifdef _WIN32
 SYSTEM_INFO info;GetSystemInfo(&info);return info.dwPageSize;
#elif defined(__SWITCH__)
 return 0x1000;
#else
 return (size_t)getpagesize();
#endif
}
inline bool memory_touched(void* p,size_t bytes) {
#ifdef _WIN32
 // MEM_WRITE_WATCH records written pages even after paging them out. Residency alone
 // cannot distinguish untouched zero pages from modified pages in the swap file.
 size_t n=(bytes+page_size()-1)/page_size(); std::vector<void*> pages(n);
 ULONG_PTR count=n; DWORD granularity=0;
 if(GetWriteWatch(0,p,bytes,pages.data(),&count,&granularity)!=0)return true;
 return count!=0;
#elif defined(__APPLE__)
 std::vector<char> pages((bytes+page_size()-1)/page_size());
 if(mincore(p,bytes,pages.data()))return true;
 for(auto page:pages)if(page!=0)return true;
 return false;
#else
 // Linux mincore reports residency, not whether a swapped-out page contains data.
 // Scan the mapped region to preserve every byte; this is slower than macOS/Windows.
 (void)p;(void)bytes;return true;
#endif
}
inline bool replace_file(const std::string& from,const std::string& to) {
#ifdef _WIN32
 return MoveFileExA(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
#ifdef __SWITCH__
 remove(to.c_str());  // the SD card file system does not replace an existing target
#endif
 return rename(from.c_str(),to.c_str())==0;
#endif
}
// Horizon starts every thread on the process's default core; let the scheduler use the three
// application cores, preferring `core` (the emulated Espresso core, or any host thread's choice).
inline void set_thread_core(uint32_t core) {
#ifdef __SWITCH__
 svcSetThreadCoreMask(threadGetCurHandle(),(s32)(core%3),0x7);
#else
 (void)core;
#endif
}
// WWHD_CORE_LAYOUT (on by default on the Switch, =0 for the old placement): the game's main thread
// (emulated core 1) gets host core 1 to itself; the render thread, Mesa's GL thread and the guest
// threads of emulated cores 0 and 2 share host cores 0 and 2. Before, the higher-priority host
// threads kept landing on the main thread's core and preempting it (it held its emulated core 70%
// of the time but ran only 63%), and the main thread is what limits the frame rate in busy views.
inline bool core_layout_on() {
#ifdef __SWITCH__
    static const bool on = [] {
        const char* e = getenv("WWHD_CORE_LAYOUT");
        return !(e && *e == '0');
    }();
    return on;
#else
    return false;
#endif
}
// a thread's preferred host core and the set it may run on (bit n: core n); Switch only
inline void set_thread_cores(uint32_t preferred, uint32_t mask) {
#ifdef __SWITCH__
 svcSetThreadCoreMask(threadGetCurHandle(), (s32)preferred, mask);
#else
 (void)preferred; (void)mask;
#endif
}
// placement of a thread of emulated core `core` (guest threads, the render and GL threads)
inline void place_thread(uint32_t core) {
    if (!core_layout_on()) return set_thread_core(core);
    if (core % 3 == 1) set_thread_cores(1, 0x2);
    else set_thread_cores(core % 3, 0x5);
}
// libnx creates every pthread at priority 59, the only one Horizon time-slices (10 ms) on cores 0-2.
// Host service threads (GX2 render, audio, alarms) move above the guest threads so they run as soon
// as they have work instead of waiting for a busy guest thread's slice to end.
inline void raise_thread_priority() {
#ifdef __SWITCH__
 svcSetThreadPriority(threadGetCurHandle(),0x2C);
 // these run above the guest threads: off the main thread's core (WWHD_CORE_LAYOUT)
 if (core_layout_on()) set_thread_cores(0, 0x5);
#endif
}
// A detached host thread. Horizon commits a small default stack for std::thread, so threads that run
// guest code or the shader compilers ask for `stack` bytes there.
inline void start_thread(void (*fn)(), size_t stack) {
#ifdef __SWITCH__
 pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,stack);
 pthread_t t;
 if(pthread_create(&t,&attr,[](void* f)->void* { raise_thread_priority(); ((void(*)())f)(); return nullptr; },(void*)fn)==0) pthread_detach(t);
 pthread_attr_destroy(&attr);
#else
 (void)stack; std::thread(fn).detach();
#endif
}
// Portable mode (release packages): a file "portable.txt" next to the executable keeps every
// per-user file (settings, controls, save states, shader caches) in "user" next to the executable's
// folder (<folder>/bin/wwhd -> <folder>/user) instead of the user's Library / AppData / .config.
// Without the marker (source builds) nothing changes.
inline std::string exe_dir() {
 static const std::string dir=[]{
  std::string p;
#if defined(__APPLE__)
  char buf[4096]; uint32_t n=sizeof buf;
  if(_NSGetExecutablePath(buf,&n)==0){ char real[PATH_MAX]; p=realpath(buf,real)?real:buf; }
#elif defined(_WIN32)
  char buf[MAX_PATH*4]; DWORD n=GetModuleFileNameA(nullptr,buf,sizeof buf); if(n>0&&n<sizeof buf) p.assign(buf,n);
#elif !defined(__SWITCH__)  // the Switch has no portable mode: everything is in sdmc:/switch/wwhd
  char buf[4096]; ssize_t n=readlink("/proc/self/exe",buf,sizeof buf-1); if(n>0) p.assign(buf,(size_t)n);
#endif
  size_t s=p.find_last_of("/\\");
  return s==std::string::npos?std::string():p.substr(0,s);
 }();
 return dir;
}
inline const std::string& portable_user_dir() {
 static const std::string dir=[]{
  std::string e=exe_dir();
  if(e.empty()) return std::string();
  FILE* f=fopen((e+"/portable.txt").c_str(),"rb");
  if(!f) return std::string();
  fclose(f);
  size_t s=e.find_last_of("/\\");
  return (s==std::string::npos?e:e.substr(0,s))+"/user";
 }();
 return dir;
}
inline bool portable() { return !portable_user_dir().empty(); }
inline std::string config_dir() {
 if(portable()) return portable_user_dir();
#ifdef __APPLE__
 const char* home=getenv("HOME");return std::string(home?home:".")+"/Library/Application Support/WWHD";
#elif defined(_WIN32)
 const char* root=getenv("APPDATA");return std::string(root?root:".")+"/WWHD";
#elif defined(__SWITCH__)
 return "sdmc:/switch/wwhd";
#else
 if(const char* xdg=getenv("XDG_CONFIG_HOME"))return std::string(xdg)+"/wwhd";
 const char* home=getenv("HOME");return std::string(home?home:".")+"/.config/wwhd";
#endif
}
}

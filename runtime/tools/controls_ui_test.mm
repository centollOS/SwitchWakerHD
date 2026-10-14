// Run the native Controls self-test and synthetic mouse captures without showing a window/game.
#include "../src/gfx/controls_ui.mm"
#include <cassert>
#include <cstdarg>
#include <filesystem>
#include <unistd.h>
namespace gfx { void show_drc_window(bool){} }
namespace input {
bool pro_controller(){return false;}
void set_pro_controller(bool){}
void release_keys(){}
void held_keys(bool* v){std::fill(v,v+256,false);}
void controller_values(float* v){std::fill(v,v+input_map::kPadCount,0.f);}
}
void log_msg(const char* fmt,...) {
    va_list args;va_start(args,fmt);vfprintf(stderr,fmt,args);va_end(args);fputc('\n',stderr);
}
int main() {
    @autoreleasepool {
        const auto path = std::filesystem::temp_directory_path()/ ("wwhd-controls-165-"+std::to_string(getpid())+".json");
        setenv("WWHD_CONTROLS",path.c_str(),1);setenv("WWHD_NO_HOST_INPUT","1",1);
        [NSApplication sharedApplication];
        input_map::set_current(input_map::Mapping::defaults(),false);
        g_controls = [[WWControls alloc] init];
        assert(self_test()==0);
        for (int i=0;i<5;++i) {
            [g_controls beginCapture:kA mode:kCapKey1];
            CGEventRef cg = CGEventCreateMouseEvent(nullptr, i==0?kCGEventLeftMouseDown:i==1?kCGEventRightMouseDown:kCGEventOtherMouseDown,CGPointZero,static_cast<CGMouseButton>(i));
            NSEvent* e = [NSEvent eventWithCGEvent:cg];
            assert(e.buttonNumber==i);
            [g_controls.window sendEvent:e];
            assert(current().keys[kA][1]==kMouse1+i);
            CFRelease(cg);
        }
        for (int delta : {1,-1}) {
            [g_controls beginCapture:kB mode:kCapAny];
            CGEventRef cg = CGEventCreateScrollWheelEvent(nullptr,kCGScrollEventUnitLine,1,delta);
            [g_controls.window sendEvent:[NSEvent eventWithCGEvent:cg]];
            assert(current().keys[kB][0]==(delta>0?kWheelUp:kWheelDown));CFRelease(cg);
        }
        [g_controls beginCapture:kR mode:kCapPad];
        CGEventRef cg=CGEventCreateMouseEvent(nullptr,kCGEventLeftMouseDown,CGPointZero,kCGMouseButtonLeft);
        const auto before=current();[g_controls handleMouseEvent:[NSEvent eventWithCGEvent:cg]];
        assert(current()==before);CFRelease(cg);[g_controls cancelCapture];
        std::filesystem::remove(path);
        fprintf(stderr,"controls_ui_test: passed\n");
    }
}

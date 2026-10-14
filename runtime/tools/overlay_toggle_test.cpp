#include "overlay/toggle_guard.h"
#include "overlay/open_reason.h"
#include "overlay/settings_shortcut.h"
#include "overlay/host_focus.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstdio>
int main(){
 using namespace overlay;
 ImGui::CreateContext();auto& io=ImGui::GetIO();
 io.KeysData[ImGuiKey_A-ImGuiKey_NamedKey_BEGIN].Down=true;
 io.KeyShift=true;io.MouseDown[0]=true;
 io.AddKeyEvent(ImGuiKey_B,true);
 assert(ImGui::GetCurrentContext()->InputEventsQueue.Size>0);
 release_focus_inputs(io);
 assert(!io.KeysData[ImGuiKey_A-ImGuiKey_NamedKey_BEGIN].Down&&!io.KeyShift&&!io.MouseDown[0]);
 assert(ImGui::GetCurrentContext()->InputEventsQueue.Size==0);
 io.AddKeyEvent(ImGuiKey_C,true); // the new window's events still arrive
 assert(ImGui::GetCurrentContext()->InputEventsQueue.Size==1);
 ImGui::DestroyContext();
 ToggleGuard g;
 assert(g.press(true,false,1));
 assert(!g.press(true,false,2)); // same press, even well beyond debounce
 assert(!g.press(true,true,2));
 assert(!g.press(false,false,2));
 assert(g.press(true,false,2.1));
 assert(!g.press(false,false,2.11));
 assert(!g.press(true,false,2.12)); // release/repress noise
 assert(!g.press(false,false,2.13));
 assert(g.press(true,false,2.26)); // deliberate next press
 for(auto reason:{OpenReason::API,OpenReason::F1,OpenReason::Esc,OpenReason::Home,OpenReason::View,OpenReason::B,OpenReason::CloseButton,OpenReason::LoadSlot,OpenReason::Focus}){
  auto line=open_message(false,reason,{7,1234,"GamePad"},2);
  assert(line.find(reason_name(reason))!=std::string::npos);
  assert(line.find("window GamePad id 7, timestamp 1234, frame 2")!=std::string::npos);
  assert(line.starts_with("[overlay] closed ("));
 }
 assert(open_message(true,OpenReason::F1,{1,99,"TV"},42)=="[overlay] opened (F1 key, window TV id 1, timestamp 99, frame 42)");
 SettingsShortcut s;
 assert(s.poll(0,false,true,false)==SettingsShortcut::Open&&s.reason()==OpenReason::Home);
 assert(s.poll(.1,false,false,true)==SettingsShortcut::None);
 assert(s.poll(.2,true,false,true)==SettingsShortcut::Close&&s.reason()==OpenReason::View);
 puts("overlay_toggle: focus release, debounce and all transition reasons passed");
}

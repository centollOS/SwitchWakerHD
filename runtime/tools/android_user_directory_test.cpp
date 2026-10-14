#include "platform/host.h"
#include <cassert>
#include <fstream>
#include <chrono>

int main() {
 namespace fs=std::filesystem;
 const auto root=fs::temp_directory_path()/
     ("wwhd-android-user-directory-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 const auto external=root/"external";
 const auto existing=external/"config"/"wwhd";
 fs::create_directories(existing);
 std::ofstream(existing/"settings.ini")<<"existing-settings";
 std::ofstream(existing/"controls.json")<<"existing-controls";
 // This is the exact XDG root supplied by SDL's Android startup, using the
 // production Linux/Android resolver rather than a test-only path calculation.
 const auto xdg=(external/"config").string();
 const auto selected=fs::path(host::xdg_config_dir(xdg.c_str(),"unused-home"));
 assert(selected==existing);
 for(const auto* name:{"settings.ini","controls.json"}) {
  std::ifstream input(selected/name);
  std::string contents;
  std::getline(input,contents);
  assert(contents==(std::string(name)=="settings.ini"?"existing-settings":"existing-controls"));
 }
 assert(selected/"shader-prebuild"==existing/"shader-prebuild");
 assert(!fs::exists(root/"user"));
 assert(host::xdg_config_dir(nullptr,"example-home")=="example-home/.config/wwhd");
 fs::remove_all(root);
}

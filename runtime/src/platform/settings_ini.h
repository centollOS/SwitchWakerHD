// The Switch's settings.ini (sdmc:/switch/wwhd/settings.ini) as text: no file access and no Switch headers,
// so tools/switch/settings_ini_host_test.cpp checks it on a computer.
//
//   # Wind Waker HD settings
//   switchCpuClock=1224            the menu's settings: KEY=VALUE lines before any [section] header
//   mod.fast-scenes.enabled=1
//
//   [dev]                          developer variables: each KEY=VALUE line goes into the environment
//   WWHD_DK_TRACE_FRAMES=2500      before the static initialisers run (main.cpp)
//
// Everything from the first [section] header on is kept as written: the menu rewrites only its own lines.
// A [dev] variable that one of the menu's settings stands for (WWHD_CPU_CLOCK, WWHD_DEBUG_SERVER,
// WWHD_MOD_*...) is ignored; env.txt (the old file of variables) is converted once (migrate_env_txt).
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace settings_ini {

struct File {
    std::map<std::string, std::string> menu;  // the menu's settings
    std::vector<std::string> sections;        // from the first [section] header on, line by line, verbatim
};
File parse(const std::string& text);
// the file's text: a comment, the menu's settings (sorted), then the sections (after a blank line)
std::string format(const std::map<std::string, std::string>& menu, const std::vector<std::string>& sections);
std::string format(const File& f);

// the KEY=VALUE lines of the [dev] section, in order (comments and blank lines skipped)
std::vector<std::pair<std::string, std::string>> dev_variables(const File& f);
// adds KEY=VALUE to the [dev] section (made at the end when there is none)
void add_dev_variable(File& f, const std::string& name, const std::string& value);

// the variable is one of the menu's settings (its [dev] line is ignored)
bool menu_backed(const std::string& name);

// mods: the WWHD_MOD_* (and WWHD_CLIMB) start-up variable of each built-in mod; as mods/manager.cpp's
// catalogue (main.cpp checks the two agree)
struct ModVariable {
    const char* id;
    const char* env;
};
extern const ModVariable kModVariables[6];

// env.txt's text into f: a variable with a menu setting becomes that setting (one that cannot be converted is
// dropped), any other KEY=VALUE line goes into [dev]; comments, blank lines and --option lines are dropped.
// log: a line per variable; mods: the mods it turned on or off (main.cpp tells the mod manager too)
struct Migration {
    std::vector<std::string> log;
    std::vector<std::pair<std::string, bool>> mods;
};
Migration migrate_env_txt(const std::string& envText, File& f);

}  // namespace settings_ini

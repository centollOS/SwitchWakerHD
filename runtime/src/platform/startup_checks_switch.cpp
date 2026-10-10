// Start-up checks on the Switch (startup_checks_switch.h). They run from main() once the log is open and the
// working directory is sdmc:/switch/wwhd, before the game's code is loaded and before the renderer starts: the
// message is a text screen (libnx's console on the default window, which deko3d takes over afterwards), not the
// system's error applet, which showed only "an unexpected error" for this forwarder's title and froze the network.
#include "startup_checks_switch.h"

#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../runtime.h"

namespace startup_checks {
namespace {

bool is_file(const std::string& p, bool nonEmpty = false) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (!nonEmpty || st.st_size > 0);
}
bool is_dir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// `text` word-wrapped to the console's 80 columns (blank lines kept)
void print_wrapped(std::string_view text) {
    constexpr size_t kCols = 78;
    size_t col = 0;
    while (!text.empty()) {
        if (text[0] == '\n') { printf("\n"); col = 0; text.remove_prefix(1); continue; }
        size_t n = text.find_first_of(" \n");
        std::string_view word = text.substr(0, n);
        if (col && col + 1 + word.size() > kCols) { printf("\n"); col = 0; }
        else if (col) { printf(" "); col++; }
        printf("%.*s", int(word.size()), word.data());
        col += word.size();
        text.remove_prefix(word.size());
        if (!text.empty() && text[0] == ' ') text.remove_prefix(1);
    }
    printf("\n");
}

// A text screen with `title` (red for an error, yellow for a warning) and `body`, until `button` is pressed (or
// the app is asked to close).
void show(bool error, const char* title, const std::string& body, u64 button, const char* prompt) {
    PrintConsole* con = consoleInit(nullptr);
    printf("\n %s%s\x1b[0m\n\n", error ? "\x1b[31;1m" : "\x1b[33;1m", title);
    print_wrapped(body);
    printf("\n\x1b[36;1m%s\x1b[0m\n", prompt);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & button) break;
        consoleUpdate(con);
        svcSleepThread(16'000'000);
    }
    consoleExit(con);
}

}  // namespace

void game_files(const std::string& game_dir) {
    // a relative folder is under sdmc:/switch/wwhd (main() changed into it)
    const std::string shown = game_dir.rfind("sdmc:", 0) == 0 ? game_dir : "sdmc:/switch/wwhd/" + game_dir;
    const char* missing = nullptr;
    if (!is_file(game_dir + "/code/cking.rpx", true)) missing = "code/cking.rpx";
    else if (!is_dir(game_dir + "/content")) missing = "content/";
    else if (!is_file(game_dir + "/meta/meta.xml")) missing = "meta/meta.xml";
    if (!missing) return;
    LOG("[startup] the game's files are missing: %s/%s; closing", shown.c_str(), missing);
    log_flush();
    show(true, "SwitchWakerHD: the game's files were not found",
         "SwitchWakerHD needs your own copy of the game in " + shown +
             "/ (the folders code/, content/ and meta/), as tools/switch/make_sd.py lays it out from your dump "
             "(INSTALL.md).\n\nMissing: " + shown + "/" + missing +
             "\n\nCopy the build/sd/ folder made by make_sd.py to the root of the SD card, then start the game "
             "again.",
         HidNpadButton_Plus, "Press + to close.");
    exit(1);
}

void shader_cache() {
    // shaders_dk.cpp's cache files (in WWHD_DK_SHADER_CACHE_DIR when set): the one built on a computer and the one
    // the console fills as it compiles; with neither, every shader is compiled while the game is played
    const char* e = getenv("WWHD_DK_SHADER_CACHE_DIR");
    const std::string dir = e && *e ? std::string(e) + "/" : std::string();
    if (is_file(dir + "shadercache_dksh.bin", true) || is_file(dir + "shadercache_dksh_local.bin", true)) return;
    LOG("[startup] no shadercache_dksh.bin and no shaders compiled on this console yet: first-start notice shown");
    log_flush();
    show(false, "SwitchWakerHD: first start",
         "No graphics have been compiled on this console yet. The console compiles each of the game's graphics "
         "effects the first time it is drawn, so on this first start some textures may look black and some objects "
         "may appear a moment late. It gets better as you play: what is compiled is kept for the next starts.\n\n"
         "If this bothers you, the console can prepare the whole game at once: start your game, hold Minus (-) for "
         "the settings menu, Switch tab, Prepare graphics. It visits every place by itself (30-40 minutes, best "
         "docked), then restarts; your Quest Log is left as it was.",
         HidNpadButton_A, "Press A to continue.");
}

}  // namespace startup_checks

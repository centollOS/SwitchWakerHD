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
#include <vector>

#include "../runtime.h"
#include "prepare_graphics_switch.h"

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

// libnx's console: 80 columns, ANSI colours (bold = bright)
constexpr size_t kCols = 80, kMargin = 4;
#define C_RESET "\x1b[0m"
#define C_DIM "\x1b[37m"
#define C_TEXT "\x1b[37;1m"
#define C_CYAN "\x1b[36;1m"
#define C_GREEN "\x1b[32;1m"

// `text` word-wrapped between column `indent` and the right margin (blank lines kept), in `colour`
void print_wrapped(std::string_view text, size_t indent = kMargin, const char* colour = C_DIM) {
    const size_t width = kCols - kMargin - indent;
    printf("%s%*s", colour, int(indent), "");
    size_t col = 0;
    while (!text.empty()) {
        if (text[0] == '\n') { printf("\n%*s", int(indent), ""); col = 0; text.remove_prefix(1); continue; }
        size_t n = text.find_first_of(" \n");
        std::string_view word = text.substr(0, n);
        if (col && col + 1 + word.size() > width) { printf("\n%*s", int(indent), ""); col = 0; }
        else if (col) { printf(" "); col++; }
        printf("%.*s", int(word.size()), word.data());
        col += word.size();
        text.remove_prefix(word.size());
        if (!text.empty() && text[0] == ' ') text.remove_prefix(1);
    }
    printf(C_RESET "\n");
}

// a full-width bar: the title on a coloured background (red: an error, yellow: a notice)
void title_bar(bool error, const char* title) {
    const char* bar = error ? "\x1b[41;37;1m" : "\x1b[43;30m";
    printf("\n%s%*s%s\n", bar, int(kCols), "", C_RESET);
    printf("%s%*s%-*s%s\n", bar, int(kMargin), "", int(kCols - kMargin), title, C_RESET);
    printf("%s%*s%s\n\n", bar, int(kCols), "", C_RESET);
}

void rule() { printf(C_DIM "%*s%s" C_RESET "\n", int(kMargin), "", std::string(kCols - 2 * kMargin, '-').c_str()); }

struct Choice {
    const char* button;  // "A"
    const char* label;
    const char* detail;
};

// waits for one of `buttons` (0: the app is asked to close); the button pressed
u64 wait_for(PrintConsole* con, u64 buttons) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    u64 pressed = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((pressed = padGetButtonsDown(&pad) & buttons)) break;
        consoleUpdate(con);
        svcSleepThread(16'000'000);
    }
    consoleExit(con);
    return pressed;
}

// A text screen: a title bar, paragraphs, then either choices (each a coloured button, its label and a line of
// detail) or a one-line prompt; until one of `buttons` is pressed. The button pressed.
u64 show(bool error, const char* title, const std::vector<std::string>& paragraphs, const std::vector<Choice>& choices,
         u64 buttons, const char* prompt) {
    PrintConsole* con = consoleInit(nullptr);
    title_bar(error, title);
    for (const std::string& p : paragraphs) {
        print_wrapped(p, kMargin, C_TEXT);
        printf("\n");
    }
    if (!choices.empty()) {
        rule();
        printf("\n");
        for (const Choice& c : choices) {
            printf("%*s" C_GREEN "(%s)" C_RESET "  " C_TEXT "%s" C_RESET "\n", int(kMargin), "", c.button, c.label);
            print_wrapped(c.detail, kMargin + 5, C_DIM);
            printf("\n");
        }
        rule();
    }
    if (prompt) printf("\n%*s" C_CYAN "%s" C_RESET "\n", int(kMargin), "", prompt);
    return wait_for(con, buttons);
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
         {"SwitchWakerHD needs your own copy of the game in " + shown + "/ (the folders code/, content/ and meta/), "
          "as tools/switch/make_sd.py lays it out from your dump (INSTALL.md).",
          "Missing: " + shown + "/" + missing,
          "Copy the build/sd/ folder made by make_sd.py to the root of the SD card, then start the game again."},
         {}, HidNpadButton_Plus, "Press + to close.");
    exit(1);
}

bool shader_cache() {
    // shaders_dk.cpp's cache files (in WWHD_DK_SHADER_CACHE_DIR when set): the one built on a computer and the one
    // the console fills as it compiles; with neither, every shader is compiled while the game is played
    const char* e = getenv("WWHD_DK_SHADER_CACHE_DIR");
    const std::string dir = e && *e ? std::string(e) + "/" : std::string();
    if (is_file(dir + "shadercache_dksh.bin", true) || is_file(dir + "shadercache_dksh_local.bin", true)) {
        // graphics already compiled here: a player updating from a version without Prepare graphics, offered once
        if (!prepare_graphics::offer_to_update()) return false;
        prepare_graphics::mark_offered();
        LOG("[startup] Prepare graphics offered once (an update)");
        log_flush();
        const u64 b = show(false, "SwitchWakerHD: new in this version",
             {"Prepare graphics: the console can now prepare the graphics of the whole game at once, so that places "
              "you have not visited yet are not black or late the first time."},
             {{"A", "Prepare graphics now",
               "The game visits every place by itself behind a loading screen (about 30-40 minutes, best docked), "
               "then restarts at the title screen, ready. Your saves are not touched. What is already prepared on "
               "this console goes faster."},
              {"B", "Later",
               "This message does not show again. Prepare graphics stays in the settings menu: hold Minus (-) in "
               "the game, Switch tab."}},
             HidNpadButton_A | HidNpadButton_B, nullptr);
        LOG("[startup] update notice: %s", b & HidNpadButton_A ? "prepare graphics now" : "later");
        return b & HidNpadButton_A;
    }
    LOG("[startup] no shadercache_dksh.bin and no shaders compiled on this console yet: first-start notice shown");
    log_flush();
    const u64 b = show(false, "SwitchWakerHD: first start",
         {"The graphics have not been prepared on this console yet.",
          "On this first start some textures may look black and some objects may appear a moment late: the console "
          "prepares each graphics effect the first time it is drawn. It gets better as you play, and what is "
          "prepared is kept for the next starts."},
         {{"A", "Prepare graphics now",
           "The game visits every place by itself behind a loading screen (about 30-40 minutes, best docked), then "
           "restarts at the title screen, ready. Your saves are not touched."},
          {"B", "Play now",
           "You can prepare the graphics later: hold Minus (-) in the game for the settings menu, Switch tab."}},
         HidNpadButton_A | HidNpadButton_B, nullptr);
    prepare_graphics::mark_offered();  // (the update notice is for players who never saw this one)
    const bool prepare = b & HidNpadButton_A;
    LOG("[startup] first-start notice: %s", prepare ? "prepare graphics now" : "play now");
    return prepare;
}

}  // namespace startup_checks

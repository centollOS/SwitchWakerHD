#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
// (this fork) The PT-BR fan translation (fan_translation.h, docs/ptbr-translation-plan.md) on a SYNTHETIC game
// folder: dummy packs (a "SARC" tag and made-up bytes; no game data). Checks when Portuguese (Brazil) is offered,
// the language the game is told, and that the real guest file system HLE reads the translation's pack in place of
// the English one only once it is chosen, while everything else stays as it was.
#include "fan_translation.h"
#include "game_languages.h"
#include "runtime.h"
#include "savestate.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
// hle/fs.cpp reports opened language packs to the right-to-left text support (not under test)
namespace rtl_text { void language_pack_opened(const std::string&) {} }
// hle/fs.cpp reports opened 2D layout / 3D pose packs to the HUD mods (not under test; the same stubs as
// mod_content_fs_test.cpp and language_source_test.cpp)
namespace game_layouts { void note_open(const std::string&, const std::string&) {} }
namespace link_poses { void note_open(const std::string&, const std::string&) {} }
namespace fs = std::filesystem;
namespace {
std::map<std::string, PpcFunc>& functions() {
    static std::map<std::string, PpcFunc> f;
    return f;
}
constexpr uint32_t base = 0x10000000;
}  // namespace
HleReg::HleReg(const char* lib, const char* name, PpcFunc fn) { functions()[std::string(lib) + ":" + name] = fn; }
bool g_trace_hle = false;
void log_msg(const char*, ...) {}
namespace config { std::string game_dir, save_dir; }
namespace mem {
std::string read_cstr(uint32_t ea) { return reinterpret_cast<const char*>(ptr(ea)); }
void write_cstr(uint32_t ea, const std::string& s, uint32_t max) {
    if (max) {
        auto n = std::min<size_t>(max - 1, s.size());
        memcpy(ptr(ea), s.data(), n);
        ptr(ea)[n] = 0;
    }
}
}  // namespace mem
namespace threads { void block_begin() {} void block_end() {} }
namespace wwatch { void host_write_begin(uint32_t, uint32_t) {} void host_write_end(uint32_t, uint32_t) {} }

static int32_t call(const char* name, std::initializer_list<uint32_t> args) {
    Cpu c{};
    size_t i = 3;
    for (auto a : args) c.r[i++] = a;
    functions().at(name)(&c);
    return int32_t(c.r[3]);
}
static int32_t try_open(const std::string& path, const std::string& mode, uint32_t* handle) {
    mem::write_cstr(base, path, 1024);
    mem::write_cstr(base + 1024, mode, 32);
    int32_t r = call("coreinit:FSOpenFile", {0, 0, base, base + 1024, base + 1100, 0});
    *handle = ld32(base + 1100);
    return r;
}
static std::string read_all(const std::string& path) {
    uint32_t h = 0;
    assert(try_open(path, "rb", &h) == 0);
    auto n = call("coreinit:FSReadFile", {0, 0, base + 8192, 1, 4096, h, 0});
    assert(n >= 0);
    std::string s(reinterpret_cast<char*>(mem::ptr(base + 8192)), size_t(n));
    assert(call("coreinit:FSGetStatFile", {0, 0, h, base + 2048, 0}) == 0);
    assert(ld32(base + 2048 + 0x10) == s.size());
    assert(call("coreinit:FSCloseFile", {0, 0, h}) == 0);
    mem::write_cstr(base, path, 1024);
    assert(call("coreinit:FSGetStat", {0, 0, base, base + 2048, 0}) == 0);
    assert(ld32(base + 2048 + 0x10) == s.size());
    return s;
}
static void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

int main() {
    auto address = mem::ptr(base);
    constexpr size_t memory_size = 65536;
#ifdef _WIN32
    auto memory = VirtualAlloc(address, memory_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    auto memory = mmap(address, memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
    assert(memory == address);
    const fs::path root = fs::temp_directory_path() /
                          ("wwhd-ptbr-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    // sdmc:/switch/wwhd/game and sdmc:/switch/wwhd/translation/pt-BR
    const fs::path game = root / "wwhd" / "game", pack = root / "wwhd" / "translation" / "pt-BR" / "permanent_2d_ptBR.pack";
    put(game / "content" / "Common" / "Pack" / "permanent_2d_UsEnglish.pack", "SARC synthetic us-en");
    put(game / "content" / "Common" / "Pack" / "permanent_2d_UsFrench.pack", "SARC synthetic us-fr");
    put(game / "content" / "Common" / "Pack" / "permanent_2d_UsSpanish.pack", "SARC synthetic us-es");
    put(game / "code" / "app.xml", "synthetic");
    config::game_dir = (game / "").string();  // as the Switch build has it, with the slash
    config::save_dir = (root / "save").string();
    fs::create_directories(root / "save");
#ifdef _WIN32
    _putenv_s("WWHD_PTBR_PACK", "");
#else
    unsetenv("WWHD_PTBR_PACK");
#endif
    const std::string en = "/vol/content/Common/Pack/permanent_2d_UsEnglish.pack";

    // no translation: Portuguese is the disc's (absent) language, as before
    assert(!fan_translation::present());
    assert(std::string(game_lang::name(9)) == "Portuguese" && !game_lang::is_available(9));
    assert(game_lang::usable(9) == 1 && fan_translation::default_language(1) == 1);
    assert(fan_translation::game_language(9) == 9 && !fan_translation::active());
    assert(fan_translation::redirect(en).empty());

    // a file that is not a pack is not offered
    put(pack, "PK not a pack");
    fan_translation::reset_for_test();
    assert(!fan_translation::present() && !game_lang::is_available(9));

    // the translation installed
    put(pack, "SARC synthetic pt-br");
    fan_translation::reset_for_test();
    assert(fan_translation::present() && fs::path(fan_translation::pack_path()) == pack);
    assert(std::string(game_lang::name(9)) == "Portugu\xC3\xAAs (Brasil)" && game_lang::is_available(9));
    assert(game_lang::is_available(1) && !game_lang::is_available(3));  // the others as they were
    assert(fan_translation::default_language(1) == 9);
    game_lang::Start s = game_lang::choose(9, game_lang::kNoRegion);
    assert(!s.pack && s.language == 9);
    // before the game reads the console language, nothing is redirected
    assert(fan_translation::redirect(en).empty() && read_all(en) == "SARC synthetic us-en");
    // another language chosen: no translation
    assert(fan_translation::game_language(2) == 2 && !fan_translation::active());
    assert(fan_translation::shown_language(2) == 2);

    // Portuguese chosen: the game runs in English, its English pack read from the translation
    assert(fan_translation::game_language(9) == 1 && fan_translation::active());
    game_lang::set_started(1);
    assert(fan_translation::shown_language(game_lang::started()) == 9);
    assert(read_all(en) == "SARC synthetic pt-br");
    assert(read_all("/vol/content/common/pack/PERMANENT_2D_USENGLISH.PACK") == "SARC synthetic pt-br");
    assert(read_all("Common/Pack/permanent_2d_UsEnglish.pack") == "SARC synthetic pt-br");
    assert(fs::path(fan_translation::redirect("/vol/content/Cafe/JP/Pack/permanent_2d_EuEnglish.pack")) == pack);
    assert(fan_translation::redirect("/vol/content/Common/Pack/xpermanent_2d_UsEnglish.pack").empty());
    // the other packs are untouched, writers never reach the translation
    assert(read_all("/vol/content/Common/Pack/permanent_2d_UsFrench.pack") == "SARC synthetic us-fr");
    uint32_t h = 0;
    assert(try_open(en, "r+b", &h) == 0);
    assert(call("coreinit:FSCloseFile", {0, 0, h}) == 0);
    {
        std::ifstream in(pack, std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(in), {}};
        assert(bytes == "SARC synthetic pt-br");
    }

    fs::remove_all(root);
#ifdef _WIN32
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, memory_size);
#endif
    std::cout << "fan_translation_test: Portuguese (Brazil) offered, chosen and read through the guest FS\n";
}

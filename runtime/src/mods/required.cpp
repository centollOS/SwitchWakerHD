// Required packages (required.h).
#include "required.h"
#include "packages.h"
#include "mods.h"
#include <cstdio>

#include <mutex>
#include <thread>

namespace mods::required {
namespace {
std::mutex g_mu;
Status g_status;
bool g_started = false;  // check_local ran: the start is over, an install takes effect at the next one
void set(const Status& s) {
    std::lock_guard lock(g_mu);
    g_status = s;
}
const packages::View* find(const std::vector<packages::View>& list, const std::string& id) {
    for (const auto& v : list)
        if (v.id == id) return &v;
    return nullptr;
}
// installed: confirm it as the port's own and keep it enabled
bool keep_enabled(std::string& why) {
    auto list = packages::list();
    const auto* v = find(list, kStoryNotebook);
    if (!v) return why = "not installed", false;
    std::string error;
    if (!v->native_confirmed && !packages::confirm_native(kStoryNotebook, error)) return why = error, false;
    if (!v->enabled) {
        if (!packages::unacknowledged_experimental(kStoryNotebook).empty() && !packages::acknowledge_experimental(kStoryNotebook, error))
            return why = error, false;
        if (!packages::enable(kStoryNotebook, true, error)) return why = error, false;
        fprintf(stderr, "[mods] %s enabled: the story system's required package\n", kStoryNotebook.c_str());
    }
    return true;
}
const char* kGetIt = "Story notebook: not installed. Install it from the Mods menu (it needs the internet once).";
}  // namespace

std::string catalogue_url(const std::string& build_version) {
    if (const char* forced = getenv("WWHD_STORY_CATALOGUE"); forced && *forced) return forced;
    const bool development = build_version == "dev" || build_version.find_first_of("+-") != std::string::npos;
    return std::string("https://raw.githubusercontent.com/ZeldaWWHDRecomp/ZeldaWWHDMods/") + (development ? "devel" : "main") + "/index.json";
}

bool check_local() {
    {
        std::lock_guard lock(g_mu);
        g_started = true;
    }
    Status s;
    s.needed = mods::story_system();
    if (!s.needed) return set(s), false;
    std::string why;
    s.installed = keep_enabled(why);
    if (!s.installed) s.text = why == "not installed" ? kGetIt : "Story notebook: " + why;
    if (!s.installed) fprintf(stderr, "[mods] %s The story system is inactive.\n", s.text.c_str());
    set(s);
    return s.installed;
}

bool install(const Context& c) {
    Status s;
    s.needed = mods::story_system();
    s.working = true;
    s.text = "Story notebook: downloading...";
    set(s);
    std::string why;
    bool ok = false;
    try {
        if (!find(packages::list(), kStoryNotebook)) {
            const catalogue::Loaded loaded = catalogue::refresh_cached(c.catalogue, c.fetch, c.work);
            const catalogue::Entry* entry = nullptr;
            for (const auto& e : loaded.index.entries)
                if (e.id == kStoryNotebook) entry = &e;
            catalogue::require(entry != nullptr, "the mod catalogue has no " + kStoryNotebook);
            catalogue::StagedPackage package(*entry, c.port, c.build, packages::platform_key(), c.work, loaded.fixture_root, c.fetch);
            std::string error, id;
            catalogue::require(packages::install(package.path().string(), error, &id), error);
            fprintf(stderr, "[mods] %s %s installed from the catalogue: the story system's required package\n", kStoryNotebook.c_str(), entry->version.c_str());
        }
        ok = keep_enabled(why);
    } catch (const std::exception& e) {
        why = e.what();
    }
    s.working = false;
    s.installed = ok;
    {
        std::lock_guard lock(g_mu);
        s.restart = ok && g_started;
    }
    s.text = ok ? (s.restart ? "Story notebook installed: the story system starts with the next start." : "")
                : "Story notebook: not installed (" + why + "). Try again from the Mods menu.";
    fprintf(stderr, "[mods] %s\n", ok ? "the Story Notebook is installed and enabled" : s.text.c_str());
    set(s);
    return ok;
}

void install_in_background(const Context& c) {
    {
        std::lock_guard lock(g_mu);
        if (g_status.working) return;
        g_status.working = true;
    }
    std::thread([c] { install(c); }).detach();
}

Status status() {
    std::lock_guard lock(g_mu);
    return g_status;
}
}  // namespace mods::required

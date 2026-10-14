// Required packages: the story system's Story Notebook (the mod repository's "story-notebook": the
// letter that brings the story notebook, the notebook's look, the first small quest). Every quest mod
// depends on it; while the story system is on (the "story-system" built-in, on by default) it is
// confirmed like the port's own code, kept enabled and can't be switched off or removed on its own
// (packages.cpp).
//
// The game's start never uses the network: it only looks at what is installed (check_local). The
// package is downloaded from the mod catalogue by setup (wwhd --install-story-notebook, a setup step
// when the story system is on) or from the Mods menu (switching the story system on, Install on its
// line, or switching on a quest mod while it is missing). Without it: the story system is inactive
// (no letters, takeovers, chart marks or notebook), quest mods wait with "Needs the Story Notebook
// quest", the plain game is untouched and no save is changed.
#pragma once
#include "catalogue_client.h"
#include <functional>
#include <string>

namespace mods::required {
inline const std::string kStoryNotebook = "story-notebook";
// The catalogue the package comes from: WWHD_STORY_CATALOGUE, else the mod repository's main channel
// for releases and its devel channel for development builds ("v0.2.13+210", "dev").
std::string catalogue_url(const std::string& build_version);
struct Context {
    std::string catalogue;
    catalogue::Fetch fetch;
    catalogue::Version port;
    std::string build = "USA";
    std::filesystem::path work;  // the catalogue cache folder
};
// At the start, after packages::initialize() and before the guest mods load: no network. Installed:
// confirmed and enabled (true: the caller refreshes the guest startup list). Not installed: false and
// the status says how to get it.
bool check_local();
// Download, install, confirm and enable it (setup, the Mods menu; blocking, on a background thread in
// the menu). False with the reason in status().
bool install(const Context& context);
// The Mods menu: install() on a background thread (nothing if one is running)
void install_in_background(const Context& context);
struct Status {
    bool needed = false;     // the story system is on
    bool installed = false;  // the package is installed and enabled
    bool working = false;    // an install is running
    bool restart = false;    // installed after the start: the story system starts with the next start
    std::string text;        // for the Mods menu and setup
};
Status status();
}  // namespace mods::required

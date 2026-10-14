// "Restart required" logic of the Mods tab (overlay/restart_notice.h): which mod changes count as
// pending, and when the popup opens. No UI, game or mod files.
#include "overlay/restart_notice.h"
#include <cassert>
#include <cstdio>

using overlay::restart::Item;
using overlay::restart::Notice;
using View = mods::packages::View;

static View pack(const char* id, const char* name, const char* kind, bool enabled, bool active) {
    View v;
    v.id = id; v.name = name; v.kind = kind; v.enabled = enabled; v.active = active;
    // as packages::list(): only next-launch kinds wait for a restart
    v.restart_required = v.kind == "content" || v.kind == "cemu" || v.kind == "guest";
    v.pending_restart = v.restart_required && enabled != active;
    return v;
}

int main() {
    using overlay::restart::pending;
    using overlay::restart::summary;
    const double s = Notice::kSettle + 0.01;  // just past the settle time
    mods::code::Status idle;

    // A save restore staged in Save backups is applied at the next start: it asks for a restart too.
    {
        auto items = pending({}, idle, "2026-10-11 10:00:00 (Manual backup)");
        assert(items.size() == 1 && items[0].key.starts_with("restore:") && summary(items) == "save restore");
        assert(items[0].text.find("2026-10-11 10:00:00") != std::string::npos);
        assert(pending({}, idle, "").empty());
    }
    // Live changes never count: native and settings packages, built-in mods (not packages at all).
    {
        auto items = pending({pack("fps", "FPS counter", "native", true, false), pack("preset", "Preset", "settings", false, true)}, idle);
        assert(items.empty());
        assert(summary(items).empty());
    }
    // Sentences and labels.
    {
        auto on = pack("gc-sea-minimap", "Sea minimap", "guest", true, false);
        auto off = pack("hd", "HD textures", "content", false, true);
        auto cfg = pack("res", "Resolution pack", "cemu", true, true);
        cfg.pending_restart = true;  // startup config differs from the saved one
        auto items = pending({on, off, cfg}, idle);
        assert(items.size() == 3);
        assert(items[0].text == "Sea minimap will be turned on after a restart.");
        assert(items[1].text == "HD textures will be turned off after a restart.");
        assert(items[2].text == "Resolution pack settings will change after a restart.");
        assert(summary(items) == "Sea minimap on, HD textures off, Resolution pack settings");
        mods::code::Status code;
        code.ready = true; code.target = true; code.mod = "gc-sea-minimap";
        auto with_code = pending({pack("gc-sea-minimap", "Sea minimap", "guest", false, false)}, code);
        assert(with_code.size() == 1 && with_code[0].text == "Sea minimap and code-mod support will be turned on after a restart.");
        code.mod.clear(); code.target = false;
        assert(pending({}, code)[0].text == "Code-mod support will be turned off after a restart.");
    }
    // State transitions.
    {
        Notice n;
        // A pack that failed to load at start is pending from the outset: known, no popup.
        auto broken = pack("broken", "Broken pack", "content", true, false);
        std::vector<View> views = {broken};
        assert(!n.update(pending(views, idle), 0, false));
        assert(!n.update(pending(views, idle), 10, false));
        // Change: enable the minimap. Nothing before the batch settles, then exactly once.
        views.push_back(pack("gc-sea-minimap", "Sea minimap", "guest", true, false));
        assert(!n.update(pending(views, idle), 20, false));
        assert(!n.update(pending(views, idle), 20 + Notice::kSettle / 2, false));
        assert(n.update(pending(views, idle), 20 + s, false));
        assert(n.showing() && n.fresh().size() == 1 && n.fresh()[0].key == "pkg:gc-sea-minimap:on");
        assert(!n.update(pending(views, idle), 21, false));  // open: not again
        // Later: the same pending state never asks again.
        n.dismiss();
        for (double t = 22; t < 40; t += 1) assert(!n.update(pending(views, idle), t, false));
        // A further change adds to the pending set: asked again, naming only the new change.
        views.push_back(pack("hd", "HD textures", "content", false, true));
        assert(!n.update(pending(views, idle), 50, false));
        assert(n.update(pending(views, idle), 50 + s, false));
        assert(n.fresh().size() == 1 && n.fresh()[0].text == "HD textures will be turned off after a restart.");
        n.dismiss();
        // Several changes in a row form one batch: one popup naming both.
        views.push_back(pack("a", "Pack A", "cemu", true, false));
        assert(!n.update(pending(views, idle), 60, false));
        views.push_back(pack("b", "Pack B", "cemu", true, false));
        assert(!n.update(pending(views, idle), 60.3, false));
        assert(!n.update(pending(views, idle), 60 + s, false));  // settles from the last change
        assert(n.update(pending(views, idle), 60.3 + s, false));
        assert(n.fresh().size() == 2);
        n.dismiss();
        // Another dialog, a running setup or a dragged slider defers the popup.
        views.push_back(pack("c", "Pack C", "content", true, false));
        assert(!n.update(pending(views, idle), 70, true));
        assert(!n.update(pending(views, idle), 75, true));
        assert(n.update(pending(views, idle), 75.1, false));
        // Undone while the popup is up: it closes without the player answering.
        views.pop_back();
        assert(!n.update(pending(views, idle), 76, false) && !n.showing());
        // Undone and redone after Later: a new change, asked again.
        views.pop_back();  // Pack B no longer pending
        assert(!n.update(pending(views, idle), 80, false));
        views.push_back(pack("b", "Pack B", "cemu", true, false));
        assert(!n.update(pending(views, idle), 81, false));
        assert(n.update(pending(views, idle), 81 + s, false) && n.fresh()[0].key == "pkg:b:on");
        n.dismiss();
        // A settings change of an active pack is its own pending state.
        views.push_back(pack("res", "Resolution pack", "cemu", true, true));
        assert(!n.update(pending(views, idle), 90, false) && !n.update(pending(views, idle), 90 + s, false));
        views.back().pending_restart = true;
        assert(!n.update(pending(views, idle), 91, false));
        assert(n.update(pending(views, idle), 91 + s, false) && n.fresh()[0].key == "pkg:res:settings");
        n.dismiss();
        // Live changes alongside never ask.
        views.push_back(pack("fps", "FPS counter", "native", true, false));
        assert(!n.update(pending(views, idle), 100, false) && !n.update(pending(views, idle), 101, false));
    }
    // Code-mod rebuild: its own dialog already offered the restart (acknowledged), so no second popup.
    {
        Notice n;
        assert(!n.update(pending({}, idle), 0, false));
        mods::code::Status code;
        code.requested = true; code.ready = true; code.target = true;
        auto items = pending({}, code);
        for (const auto& item : items) n.acknowledge(item.key);
        assert(!n.update(items, 1, true));
        code.requested = false;  // Later in the rebuild dialog
        assert(!n.update(pending({}, code), 2, false) && !n.update(pending({}, code), 5, false));
        // The hint still lists it.
        assert(summary(pending({}, code)) == "code-mod support on");
    }
    // Restart: the new process starts with nothing pending (wanted == active): no popup, no hint.
    {
        Notice n;
        std::vector<View> after = {pack("gc-sea-minimap", "Sea minimap", "guest", true, true), pack("hd", "HD textures", "content", false, false)};
        assert(pending(after, idle).empty());
        for (double t = 0; t < 5; t += 1) assert(!n.update(pending(after, idle), t, false));
        assert(!n.showing());
    }
    std::puts("restart notice: pending set, popup once per batch, Later, undo, blocked, code mods and restart passed");
}

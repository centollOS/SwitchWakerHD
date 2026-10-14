// "Restart required" notification for the Mods tab: what is waiting for a restart, and when to
// tell the player. The pending set is derived from the mod manager's own state (package wanted vs
// active, code-mod rebuild status) every frame; nothing here changes what a mod does.
#pragma once
#include "../mods/code_mods.h"
#include "../mods/packages.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace overlay::restart {
struct Item {
    std::string key;    // identifies the pending state ("pkg:<id>:on", "code:on", ...)
    std::string text;   // one sentence for the popup
    std::string label;  // short form for the "Restart to apply" line
};

// Everything a restart would change. Live changes (native and settings packages, built-in mods)
// never appear: the manager marks only next-launch kinds (content, Cemu, guest) pending_restart.
// restore: the save backup staged for the next start (mods/save_backup.h), described for the player.
inline std::vector<Item> pending(const std::vector<mods::packages::View>& packages, const mods::code::Status& code,
                                 const std::string& restore = {}) {
    std::vector<Item> out;
    if (!restore.empty())
        out.push_back({"restore:" + restore, "Your saves will be restored from the backup of " + restore + " at the next start.",
                       "save restore"});
    if (code.ready) {
        std::string name;
        for (const auto& p : packages) if (!code.mod.empty() && p.id == code.mod) name = p.name;
        if (code.target && !name.empty())
            out.push_back({"code:on:" + code.mod, name + " and code-mod support will be turned on after a restart.",
                           name + " and code-mod support on"});
        else
            out.push_back({code.target ? "code:on" : "code:off",
                           std::string("Code-mod support will be turned ") + (code.target ? "on" : "off") + " after a restart.",
                           std::string("code-mod support ") + (code.target ? "on" : "off")});
    }
    for (const auto& p : packages) {
        if (!p.pending_restart) continue;
        if (p.enabled && !p.active)
            out.push_back({"pkg:" + p.id + ":on", p.name + " will be turned on after a restart.", p.name + " on"});
        else if (!p.enabled && p.active)
            out.push_back({"pkg:" + p.id + ":off", p.name + " will be turned off after a restart.", p.name + " off"});
        else
            out.push_back({"pkg:" + p.id + ":settings", p.name + " settings will change after a restart.", p.name + " settings"});
    }
    return out;
}

inline std::string summary(const std::vector<Item>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); i++) out += (i ? ", " : "") + items[i].label;
    return out;
}

// When to show the popup. Items pending when the notice first looks (e.g. a pack that failed to
// load at start) count as already known. A popup opens once the pending set has gained an item the
// player has not been told about and has then stayed unchanged for `settle` seconds (one popup per
// batch of clicks, after the posted changes have been applied). Later/OK marks everything pending
// as known; an item that stops being pending (the change was undone) is forgotten, so making the
// change again tells the player again.
class Notice {
public:
    static constexpr double kSettle = 0.6;

    // Once per frame. blocked: another dialog, a running setup, or a control being dragged.
    // Returns true on the frame the popup should open.
    bool update(const std::vector<Item>& items, double now, bool blocked) {
        std::vector<std::string> keys;
        for (const auto& item : items) keys.push_back(item.key);
        std::sort(keys.begin(), keys.end());
        if (!started_) {
            started_ = true;
            known_.insert(keys.begin(), keys.end());
            keys_ = keys;
            changed_ = now;
            return false;
        }
        std::erase_if(known_, [&](const std::string& k) { return !std::binary_search(keys.begin(), keys.end(), k); });
        if (keys != keys_) { keys_ = keys; changed_ = now; }
        fresh_.clear();
        for (const auto& item : items) if (!known_.contains(item.key)) fresh_.push_back(item);
        if (showing_) {
            if (fresh_.empty()) showing_ = false;  // undone while the popup was up
            return false;
        }
        if (fresh_.empty() || blocked || now - changed_ < kSettle) return false;
        showing_ = true;
        return true;
    }
    // Another dialog already told the player about this item (the code-mod rebuild dialog).
    void acknowledge(const std::string& key) { known_.insert(key); }
    // Later / OK, and Restart now (if the restart fails, it is not asked again).
    void dismiss() {
        known_.insert(keys_.begin(), keys_.end());
        showing_ = false;
        fresh_.clear();
    }
    bool showing() const { return showing_; }
    bool started() const { return started_; }
    // The items the popup names: those not told yet.
    const std::vector<Item>& fresh() const { return fresh_; }

private:
    bool started_ = false, showing_ = false;
    double changed_ = 0;
    std::vector<std::string> keys_;
    std::set<std::string> known_;
    std::vector<Item> fresh_;
};
}  // namespace overlay::restart

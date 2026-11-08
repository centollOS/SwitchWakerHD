#pragma once
// macOS menu bar key equivalents (menu.mm), kept apart so controls_ui_test can check them without the game.
// Cmd+, belongs to the app menu's Settings... item, always, as in every Mac app. It is not a remappable
// binding: the Shortcuts menu never shows it, so a player's own Cmd+, binding cannot take it or fire twice.
// Key events with Cmd go from gfx/input.mm's key monitor straight to the menu (the overlay never sees them).
#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <string>
#include "../input_map.h"

namespace menu_shortcuts {

inline bool settings_menu_key(input_map::ShortcutKey key) {
    return key.code == kVK_ANSI_Comma && key.mods == input_map::kModSuper;
}

// AppKit can display one key equivalent per item. Alternate keys get adjacent items.
inline NSString* shortcut_equivalent(input_map::ShortcutKey key) {
    if (key.code < 0 || settings_menu_key(key)) return @"";
    const int functions[] = {kVK_F1,kVK_F2,kVK_F3,kVK_F4,kVK_F5,kVK_F6,kVK_F7,kVK_F8,
                            kVK_F9,kVK_F10,kVK_F11,kVK_F12,kVK_F13,kVK_F14,kVK_F15,kVK_F16};
    for (int i = 0; i < 16; ++i) if (key.code == functions[i])
        return [NSString stringWithFormat:@"%C", (unichar)(NSF1FunctionKey + i)];
    std::string label = input_map::key_label(key.code);
    if (label.size() == 1) return [[NSString stringWithUTF8String:label.c_str()] lowercaseString];
    if (key.code == kVK_Return) return @"\r";
    if (key.code == kVK_Tab) return @"\t";
    if (key.code == kVK_Space) return @" ";
    return @""; // unsupported physical keys still work in the event path
}
inline void apply_shortcut_equivalent(NSMenuItem* item, input_map::ShortcutKey key) {
    item.keyEquivalent = shortcut_equivalent(key);
    item.keyEquivalentModifierMask = (key.mods & input_map::kModShift ? NSEventModifierFlagShift : 0) |
        (key.mods & input_map::kModCtrl ? NSEventModifierFlagControl : 0) |
        (key.mods & input_map::kModAlt ? NSEventModifierFlagOption : 0) |
        (key.mods & input_map::kModSuper ? NSEventModifierFlagCommand : 0);
}

// The Shortcuts menu: one item per bound key of every remappable action, tag = action.
inline void fill_shortcut_menu(NSMenu* menu, const input_map::Mapping& m, id target, SEL action) {
    [menu removeAllItems];
    for (int a = 0; a < input_map::kShortcutCount; ++a) {
        const auto& info = input_map::shortcut_info(a);
        for (int slot = 0; slot < 2; ++slot) {
            auto key = m.shortcuts[a].keys[slot];
            if (slot && key.code == input_map::kNoKey) continue;
            NSString* title = [NSString stringWithUTF8String:info.name];
            if (slot) title = [title stringByAppendingString:@" (alternate)"];
            NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:@""];
            item.tag = a; item.target = target;
            apply_shortcut_equivalent(item, key);
        }
    }
}

// Settings... in the app menu, with its fixed Cmd+, (toggles the overlay).
inline NSMenuItem* add_settings_item(NSMenu* app, id target, SEL action) {
    NSMenuItem* settings = [app addItemWithTitle:@"Settings…" action:action keyEquivalent:@","];
    settings.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    settings.target = target;
    settings.toolTip = @"In-game settings overlay over the picture (⌘, here, or Esc or F1 in the game window)";
    return settings;
}

// Remappable equivalents live together in the Shortcuts menu, so an old native item cannot keep a
// removed key alive. Settings... keeps its fixed Cmd+,.
inline void clear_host_keys(NSMenu* menu) {
    for (NSMenuItem* item in menu.itemArray) {
        NSString* selector = NSStringFromSelector(item.action);
        if ([selector isEqualToString:@"fullScreen:"] ||
            [selector isEqualToString:@"screenshot:"] || [selector isEqualToString:@"toggleAniso:"] ||
            [selector isEqualToString:@"toggleHires:"] || [selector isEqualToString:@"toggleFxaa:"] ||
            [selector isEqualToString:@"toggleInterp:"] || [selector isEqualToString:@"toggleInterpFps:"] ||
            [selector isEqualToString:@"setRes:"] || [selector isEqualToString:@"setAO:"]) item.keyEquivalent = @"";
        if (item.submenu) clear_host_keys(item.submenu);
    }
}

}  // namespace menu_shortcuts

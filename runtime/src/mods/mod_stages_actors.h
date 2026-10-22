// SPDX-License-Identifier: MPL-2.0
// Mod stages: the game's actors a stage's rooms may hold (docs/mod-stages.md, "Actors"). They are written into
// the room's own data (room.dzr), so the game creates them as it creates its own: no story system and no
// package dependency. A manifest names a kind and a variant, as SDK v3 actors do (the enemies' variants are
// story_enemy_variants.h's); the stage actor name and parameter word the game reads are the port's.
// Pure code, unit-tested in runtime/tools/mod_stages_test.cpp.
#pragma once
#include <cstdint>
#include <string>

#include "story_enemy_variants.h"

namespace mod_stages {

// one entry of a room's actor tables: ACTR (actors) or TRES (chests); the three rotation words as the table
// stores them (x, y, z; y is the heading, the builder writes it from the actor's angle)
struct NativeActor {
    std::string tag, name;
    uint32_t params = 0;
    uint16_t angle_x = 0, angle_z = 0x00FF;
    bool scalable = false;  // the game's scaled-actor table (SCOB) can hold it with a size: the large tree
};

// The game's trees: the variants of the stage actor "tree" (their models, collision, sway and cutting are the
// game's own). "small" and its groups are the islands' small trees, "bushy" a round bushy one, "large" the big
// leafy tree (it can be scaled 0.5 to 1.5), "palm" the palm tree.
struct TreeKind {
    const char* variant;
    const char* name;
    uint32_t params;
    bool scalable;
};
inline constexpr TreeKind kTrees[] = {
    {"small", "swood", 0x00000010u, false},    // one small tree
    {"small_3", "swood", 0x00000013u, false},  // a group of three
    {"small_5", "swood", 0x00000017u, false},  // a group of five
    {"bushy", "woodb", 0x00000000u, false},
    {"large", "lwood", 0xFFFFFFFFu, true},
    {"palm", "Oyashi", 0xFFFFFFFFu, false},
};
inline constexpr double kTreeScaleMin = 0.5, kTreeScaleMax = 1.5;

// The chest items: the game's item number and the chest it comes in (the game's small chest for a small key,
// the larger one for the map and compass, the big one for the boss key).
struct ChestItem {
    const char* item;
    uint8_t number;
    const char* chest;
    uint8_t shape;
};
inline constexpr ChestItem kChestItems[] = {
    {"small_key", 0x15, "takara2", 0},
    {"map", 0x4C, "takara", 1},
    {"compass", 0x4D, "takara", 1},
    {"boss_key", 0x4E, "takara4", 3},
    {"heart_container", 0x08, "takara4", 3},  // raises Link's hearts for good, in the player's own save
};
inline const ChestItem* chest_item(const std::string& item) {
    for (const auto& c : kChestItems)
        if (item == c.item) return &c;
    return nullptr;
}

// the kinds, for messages
inline constexpr const char* kKinds = "chuchu, bokoblin, keese, darknut, pot, big_pot, tree or chest";

// `actor`/`variant` -> the native entry; a chest's `flag` is its chest bit (0..31). False with a reason.
inline bool native_actor(const std::string& actor, const std::string& variant, const std::string& item, unsigned flag, NativeActor& out,
                         std::string& why) {
    out = NativeActor{};
    out.tag = "ACTR";
    if (actor == "chest") {
        const ChestItem* c = chest_item(item);
        if (!c) return why = "a chest holds small_key, map, compass, boss_key or heart_container", false;
        if (flag > 31) return why = "a stage has at most 32 chests", false;
        out.tag = "TRES";
        out.name = c->chest;
        out.params = 0xFF000000u | uint32_t(c->shape) << 20 | uint32_t(flag) << 7;  // no switch, the chest's flag
        out.angle_z = uint16_t(c->number << 8 | 0xFF);                                // the item, no opening switch
        return true;
    }
    if (actor == "tree") {
        const std::string v = variant.empty() ? "small" : variant;
        for (const auto& t : kTrees)
            if (v == t.variant) return out.name = t.name, out.params = t.params, out.scalable = t.scalable, true;
        return why = "tree: unknown variant \"" + variant + "\" (small, small_3, small_5, bushy, large, palm)", false;
    }
    if (!variant.empty() && (actor == "pot" || actor == "big_pot")) return why = actor + " has no variants", false;
    if (actor == "pot") return out.name = "kotubo", out.params = 0x707FFF3Fu, true;
    if (actor == "big_pot") return out.name = "ootubo1", out.params = 0x717FFF3Fu, true;
    if (actor != "chuchu" && actor != "bokoblin" && actor != "keese" && actor != "darknut")
        return why = "\"" + actor + "\" is not an actor a stage's room can hold (" + kKinds + ")", false;
    const story_enemy_variants::Variant* v = story_enemy_variants::find(actor, variant);
    if (!v) return why = actor + ": unknown variant \"" + variant + "\" (" + story_enemy_variants::names(actor) + ")", false;
    out.params = v->params;
    out.angle_x = v->angle_x;
    const std::string name = v->name;
    if (actor == "chuchu")
        out.name = name == "red" ? "c_red" : name == "blue" ? "c_blue" : name == "yellow" ? "c_kiiro" : name == "black" ? "c_black" : "c_green";
    else if (actor == "bokoblin")
        out.name = "Bk";
    else if (actor == "keese")
        out.name = name == "fire" ? "Fkeeth" : "keeth";
    else
        out.name = "Tn";
    return true;
}

}  // namespace mod_stages

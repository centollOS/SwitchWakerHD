// Mod items: the extended item icon archive. The game draws its bag and button icons from the layout
// archive BtnItemIcon_00 (in the 2D language pack, cached into the resident layout heap at start): per
// icon frame a 128 x 128 icon (Icon128_nn^l) and its glow (Float128_nn^t), chosen by the animation
// IconChange. The port serves the pack with that archive extended by one frame per mod item, built on the
// player's machine from the player's own pack (only this code ships, never the game's file or a copy of
// it) and kept in a cache next to the other built archives.
//
// Frames: the game's own are 0-54; 55 is a dummy (the game's "no item" value: a real icon there would
// never show); the mod items follow from 56 in the order set_items() gives them. The Python twin is
// tools/story_mods/lyt/item_icons.py (same bytes; test_lyt.py runs item_icons_test against it).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace item_icons {

using Bytes = std::vector<uint8_t>;
inline constexpr int kGameFrames = 55, kDummyFrame = 55, kFirstFrame = 56, kIconSize = 128;

// ---- pure parts (unit-tested) ----
// The glow picture (RGBA, white with the softened outline as alpha) of a 128 x 128 RGBA icon.
Bytes glow(const Bytes& rgba);
// IconChange with `count` more frames (keys and texture names appended; the game's keys unchanged).
Bytes extend_animation(const Bytes& flan, unsigned count);
// The archive's files with the dummy frame and the icons (RGBA 128 x 128, frames 56...) added.
std::map<std::string, Bytes> extend(const std::map<std::string, Bytes>& files, const std::vector<Bytes>& icons);
// A SARC as the game's layout archives are laid out: each texture aligned as its .bflim asks, other files
// to 4 bytes (keeps the archive in memory about as small as the game's own).
Bytes archive(const std::map<std::string, Bytes>& files);
// SARC reading and Yaz0 decoding (throw std::runtime_error on bad data).
std::map<std::string, Bytes> read_sarc(const Bytes& sarc);
Bytes yaz0_decode(const Bytes& data);
// Yaz0 with a greedy LZ search (item_icons.py yaz0_compress: the same bytes).
Bytes yaz0_compress(const Bytes& data);
// The 2D language pack with BtnItemIcon_00.szs extended (Yaz0); the other members as they were.
Bytes extend_pack(const Bytes& pack, const std::vector<Bytes>& icons);

// ---- the port ----
struct Item {
    uint16_t key = 0;       // the item (story_items key; 0 the notebook)
    std::string icon_png;   // its picture (any size; scaled to 128 x 128)
};
// The mod items with an icon, in frame order (frame 56 + index). Before the game opens its 2D pack.
void set_items(const std::vector<Item>& items, const std::filesystem::path& cache_dir);
// For the game's read of a 2D language pack (host path of the file it would read): the extended pack's
// path (built once and cached), or "" to read the original (no items, or the build failed: logged).
std::string pack_for(const std::string& guest, const std::string& host_path);
// The icon frame of an item in the served pack, or -1 (not served: the caller falls back).
int frame(uint16_t key);

}  // namespace item_icons

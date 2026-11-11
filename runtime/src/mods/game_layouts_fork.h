// SwitchWakerHD (this fork, issue #121): faster parts of game_layouts.cpp (upstream's) for the Switch's main thread,
// kept here so upstream's file changes only by a few marked calls. Timed on the console (EU, stock clocks): reading
// the whole 12 MB 2D pack at the first use took 2313 ms (now its index: 1.3 ms); an island map's 3x enlargement
// 238 ms (36 ms); showing a map again redid the enlargement (now kept: 0.2 ms). docs/guest-mods-switch-plan.md.
// Every function is called under game_layouts.cpp's lock.
#pragma once
#include <cstdint>
#include <string>

#include "game_layouts.h"

namespace game_layouts::fork {
// game_layouts::upscale's result (Lanczos-3 by 2-4, the same sharpening) within one level of rounding, with the
// taps computed once per output phase and the blur as running sums
Image upscale(const Image& image, uint32_t factor);
// a member of the 2D pack (a SARC) at pack_path: the pack's index is read once (its header and tables), then the
// member from the file. False with a short reason.
bool pack_member(const std::string& pack_path, const std::string& name, Bytes& out, std::string& error);
// the finished pictures texture() made, by their spec (at most 4 MiB, most recently used kept)
bool cached(const std::string& spec, Image& out);
void remember(const std::string& spec, const Image& image);
// another pack (note_open): the index and the pictures are dropped
void clear();
}  // namespace game_layouts::fork

// (this fork, issue #67) Lockstep frame hashes: two builds, or one build with an option on and then off, play
// the same frames from the same portable state, and the picture the window shows (the GamePad's in GamePad play) is hashed each frame; a host tool compares the
// two lists and names the first frame that differs (docs/lockstep.md, tools/switch/lockstep.py).
//
// A run is armed by a debug-server command (platform/bench_switch.cpp "bench hash") and does nothing before:
// nothing here runs in a player's game unless that command is sent (one relaxed atomic load per swap and per
// present). While armed:
// - the next portable load gets the fixed seeds and frame counter of WWHD_TEST_FIXED_SEED (fixed_seed.h): at
//   its loading frames and at the new stage's first gameplay frame (savestate.cpp), and g_Counter.mTimer is set
//   to kTimer at its loading frames;
// - the renderer's dynamic resolution is held at the profile's top factor (gfx/deko/backend.cpp);
// - the game thread tags the swaps of logic steps skip+1 .. skip+n after that first frame (gx2_core.cpp, the
//   host-only command OP_LOCKSTEP before the swap's OP_SWAP: the tag travels in the render queue with its frame);
// - the render thread hashes the shown picture of each tagged present (gfx/deko/capture.cpp: copied back from the
//   GPU, the stored bytes hashed) and records it with the tag.
// Each record also has the game state's small digest at the swap (cM_rnd's seeds, g_Counter.mTimer) and the
// frame's draw count, so a difference can be told apart: logic (the game went another way) or picture only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lockstep {

constexpr uint32_t kTagWords = 6;  // step offset, mTimer, cM_rnd seeds 0..2, the run's number
// g_Counter.mTimer written at the seeded load's loading frames (savestate.cpp): the stages' looping animations
// take their phase from it, and it otherwise goes on counting from the boot across loads
constexpr uint32_t kTimer = 0x1000;
constexpr uint32_t kCounter1 = 0x2000;  // g_Counter.mCounter1 (101FF55C), the same: at the loading frames and arrival

struct Record {
    uint32_t step = 0;            // logic steps after the seeded arrival's frame boundary
    uint32_t timer = 0;           // g_Counter.mTimer at the swap
    uint32_t rnd[3] = {0, 0, 0};  // cM_rnd's seeds at the swap
    uint64_t hash = 0;            // the shown picture's stored bytes (0: no picture)
    uint32_t w = 0, h = 0, format = 0;  // the picture's pixels and deko3d format
    uint32_t draws = 0;           // draws executed in the frame
    bool held = false;            // shown late (#49: shaders compiled in that frame; not presented)
    uint32_t run = 0;             // the run that tagged it (a tag of a run given up is dropped)
    uint16_t tiles[64] = {};      // (diagnostic, listed by "bench hash ... mem") the picture's 8x8 tiles' small hashes
};

// debug-server thread: arms a run (n frames after skipping `skip` logic steps); false when one is armed
bool arm(uint32_t n, uint32_t skip);
// debug-server thread: ends the run (done or given up); the records stay until the next arm
void disarm();
// any thread: armed and not finished (one relaxed load)
bool armed();
// savestate.cpp: the fixed seed for the next portable load (armed, no seeded arrival yet)
bool seed_wanted();
// savestate.cpp, game thread: the seeded portable load's first gameplay frame (interp::logic_steps there)
void seeded_arrival(uint64_t step);
// any thread: the seeded arrival was seen; all n records are in
bool arrived();
bool done();

// gx2_core.cpp, game thread, at a swap that ran a logic step: true when it is hashed (tag: OP_LOCKSTEP's payload)
bool game_swap(uint64_t steps, uint32_t tag[kTagWords]);
// gx2_core.cpp, render thread: OP_LOCKSTEP; the next present is hashed
void render_tag(const uint32_t* p, uint32_t n);
// render thread, at present: the tag of this frame (false: not hashed); taken once
bool take_tag(Record& r);
// render thread: the hashed frame's record
void record(const Record& r);
// after done(): the records, in order
std::vector<Record> records();

// diagnostic (bench hash ... mem): the next run's first hashed frame also snapshots the game's memory (static data
// word by word, the rest in 4 KiB block hashes) on the game thread; memdiff lists what differs from the snapshot of
// the run before
void memsnap_next();
bool dump_wanted();  // render thread: once per run after memsnap_next, the hashed picture saved as a PNG
std::string memdiff();

// the picture hash: 64 bits over the bytes (four lanes of 64-bit words; the tail byte by byte)
uint64_t hash_bytes(const void* data, size_t size, uint64_t seed = 0);

}  // namespace lockstep

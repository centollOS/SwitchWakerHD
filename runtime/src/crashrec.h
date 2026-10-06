// Crash recovery: while it is on, the game is saved into rotating automatic save states every few
// minutes, and the controller input since the latest one is recorded next to it. After a crash the
// crash log names both; WWHD_REPLAY=<n> loads automatic state n and plays the recorded input back,
// so the crash can be reproduced (reliably for crashes caused by what the game does; timing-dependent
// crashes between threads may not repeat). See crashrec.cpp.
#pragma once
#include <string>

#include "input.h"

namespace crashrec {

constexpr int kAutoSlots = 3;  // automatic states auto1..auto3 (save state slots 101..103)
constexpr int kAutoBase = 100;

bool enabled();                 // any thread
void set_enabled(bool on);      // any thread; remembered across starts
int interval_seconds();

// game thread, once per frame (from ss::service)
void service();
// game thread: automatic state `n` (1..kAutoSlots) was captured; recording restarts for it
void on_auto_saved(int n);

// pad reads (VPAD = 0, KPAD = 1) on logic passes: returns the live input (recorded while crash
// recovery is on) or, during a replay, the recorded input
input::PadState read(int pad);

struct AutoInfo {
    bool used = false;
    std::string when, area;
};
AutoInfo auto_info(int n);      // menu: automatic state n
void request_load(int n);       // menu: load automatic state n (no replay)

// crash handlers: writes which automatic state and input recording belong to this session
// (async-signal-safe: only preformatted text and write())
void crash_note(int fd, void (*out)(int, const char*, size_t));

}  // namespace crashrec

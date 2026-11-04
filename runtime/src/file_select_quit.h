// Host-only file-select Quit UI. No guest state is changed.
#pragma once
#include "input.h"
#include <cstdint>
struct Cpu;
namespace file_select_quit {
struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};
inline constexpr Rect kQuitBox{520, 605, 240, 100};

// Logical overlay coordinates, after HiDPI conversion. Rendering and pointer
// routing must use the same TV rectangle, including letterboxing and dual-screen layouts.
struct View {
    float width = 1280, height = 720;
    Rect tv{0, 0, 1280, 720};
    float scale() const { return tv.h / 720.f; }
    float origin_x() const { return tv.x + (tv.w - 1280 * scale()) * .5f; }
    float origin_y() const { return tv.y; }
    bool contains(float nx, float ny) const {
        if (width <= 0 || height <= 0 || tv.w <= 0 || tv.h <= 0)
            return false;
        const float px = nx * width, py = ny * height;
        return tv.contains(px, py) && kQuitBox.contains((px - origin_x()) / scale(), (py - origin_y()) / scale());
    }
};

// Input routing is kept independent of the renderer for regression tests.
//
// Quit is part of the game's own selection (issue #315): Down from Quest Log 3 (or Up from Quest Log 1)
// moves the selector onto Quit, Down from Quit goes on to Quest Log 1 and Up to Quest Log 3; A on Quit
// asks, as X does. `focus` is the slot the game's selector is on (0..2), or -1 for none (the native
// focus cleared while Quit has it). Event::focus asks the guest side to move the game's selector:
// kClear takes it off the slots (Quit selected), 0..2 puts it on that slot.
struct Routing {
    static constexpr int kKeep = -2, kClear = -1, kUnknown = -3;  // kUnknown: no selector (the X / mouse Quit only)
    struct Event {
        bool open = false, dismiss = false;
        uint32_t pressed = 0;
        int focus = kKeep;
        bool moved = false;  // the selector moved onto or off Quit (the cursor sound)
    };
    bool x_held = false;
    bool filter(input::PadState &pad, bool visible) {
        const bool down = (pad.buttons & input::kX) != 0;
        const bool pressed = visible && down && !x_held;
        x_held = down;
        if (visible)
            pad.buttons &= ~input::kX;
        return pressed;
    }
    bool quit_selected() const { return quit; }
    Event route(input::PadState &pad, bool eligible, bool shown, bool confirming, int focus = kUnknown) {
        const uint32_t raw = pad.buttons | (pad.lx < -.5f ? input::kLeft : 0) | (pad.lx > .5f ? input::kRight : 0);
        const uint32_t dirs = (pad.buttons & (input::kUp | input::kDown)) | (pad.ly > .5f ? input::kUp : 0) |
                              (pad.ly < -.5f ? input::kDown : 0);
        const uint32_t edges = dirs & ~previous_dirs;
        previous_dirs = dirs;
        if (!eligible || !shown) {
            previous = raw;
            x_held = (raw & input::kX) != 0;
            release = 0;
            gate = 0;
            Event event{false, !eligible, 0, kKeep, false};
            if (!eligible) {
                // the native context or the setting went (the Settings window opened...): the game's
                // selector comes back to the last Quest Log, so it never stays without one
                if (quit)
                    event.focus = 2;
                quit = awaiting = false;
            } else if (quit) {
                // A presentation stall hides the UI temporarily: Quit keeps the selector, and the
                // game, whose own selector is off, gets none of its inputs meanwhile
                pad.buttons &= ~(input::kUp | input::kDown | input::kLeft | input::kRight | input::kA);
                pad.lx = pad.ly = 0;
            }
            // only loss of the native context or the setting dismisses its pending confirmation
            return event;
        }
        Event event;
        // the game's selector took a slot itself (a touch or click on a Quest Log): Quit lets go
        if (quit && !awaiting && focus >= 0)
            quit = false;
        if (awaiting && focus == kClear)
            awaiting = false;
        bool open = filter(pad, true) && !confirming;
        if (!confirming && !open && focus != kUnknown) {
            if (quit) {
                if (edges & (input::kUp | input::kDown)) {
                    event.focus = (edges & input::kDown) ? 0 : 2;
                    event.moved = true;
                    quit = awaiting = false;
                    gate |= dirs;  // the move's press stays from the game until released
                } else if ((pad.buttons & input::kA) && !(previous & input::kA)) {
                    open = true;
                }
            } else if ((focus == 2 && (edges & input::kDown)) || (focus == 0 && (edges & input::kUp))) {
                quit = awaiting = true;
                event.focus = kClear;
                event.moved = true;
                gate |= dirs;
            }
        }
        uint32_t pressed = 0;
        if (confirming || open) {
            pressed = raw & ~previous;
            release |= raw;
            pad = input::PadState{};
        } else {
            release &= raw;
            pad.buttons &= ~release;
            gate &= dirs;
            // Quit selected: the selector's inputs are Quit's; the slots' last one does not wrap past Quit
            uint32_t keep_off = gate;
            if (quit)
                keep_off |= dirs | input::kLeft | input::kRight;
            else if (focus == 2)
                keep_off |= dirs & input::kDown;
            else if (focus == 0)
                keep_off |= dirs & input::kUp;
            pad.buttons &= ~keep_off;
            if (quit) {
                pad.buttons &= ~input::kA;
                pad.lx = 0;
            }
            if ((keep_off & input::kUp) && pad.ly > 0)
                pad.ly = 0;
            if ((keep_off & input::kDown) && pad.ly < 0)
                pad.ly = 0;
        }
        previous = raw;
        event.open = open;
        event.pressed = pressed;
        return event;
    }

  private:
    uint32_t previous = 0, release = 0, previous_dirs = 0, gate = 0;
    bool quit = false, awaiting = false;
};
void observe(Cpu *cpu); // guest draw thread, at the title background's draw
void filter(input::PadState &pad);
bool visible();
bool confirming();
bool quit_selected(); // the game's selector is on Quit
bool pointer(float x, float y); // normalized full-overlay coordinates
void set_tv_region(float x, float y, float width, float height, bool visible); // physical presentation pixels
void frame(float width, float height);
void backend_destroyed();
bool image_texture(uint64_t id); // native layout images encoded as sRGB
} // namespace file_select_quit

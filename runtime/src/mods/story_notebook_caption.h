// What the Delivery Bag says about a quest's sealed letter (A or a touch on it): the item menu's Info
// box, as the game shows it for its own items on the TV (A: "Info"): the cream box with the wind
// swirls, the item's name in the bordered menu font and its description in the message font, both
// centred; the letter's name and description come from the quest's manifest. Pure code: input in,
// draw commands out; unit-tested in runtime/tools/story_notebook_test.cpp.
//
// The port draws the box (with the menu's own Info box art and fonts) rather than opening the game's:
// in the GamePad's item menu the game has no Info for its bag items (a touch picks the item up for
// X, Y or R), and the TV menu fills its Info box from the game's item tables by item number, which
// have no entry for a port item.
#pragma once
#include "story_notebook_page.h"

namespace story_notebook {

class Caption {
public:
    // the window (1280 x 720 page units)
    // the window (1280 x 720 page units): where and how large the game's Info box is (W_TextBase_00)
    static constexpr float kW = 740, kH = 276, kX = (1280 - kW) / 2, kY = 230;
    static constexpr uint32_t kTitle = 0xFFFFFFFF, kBody = 0x2E2E2AFF;

    bool open() const { return open_; }
    const std::string& title() const { return title_; }
    void show(std::string title, std::string text) {
        title_ = std::move(title), text_ = std::move(text);
        open_ = true;
        last_ = ~0u;  // the A (or touch) that opened it does nothing
        touched_ = true;
        opening_ = 0, press_b_ = 1;
    }
    void hide() { open_ = false; }
    void tick(float dt) {
        dt = std::clamp(dt, 0.0f, 0.1f);
        opening_ = std::min(1.0f, opening_ + dt / 0.18f);
        press_b_ = std::min(1.0f, press_b_ + dt / 0.16f);
        ticked_ = true;
    }
    // B, A or a tap closes it (the game's caption closes the same way)
    Sound input(const PadInput& pad) {
        if (!open_) return Sound::None;
        if (last_ == ~0u) last_ = pad.buttons;
        const uint32_t pressed = pad.buttons & ~last_;
        last_ = pad.buttons;
        const bool tap = pad.touch && !touched_;
        touched_ = pad.touch;
        if (pressed & kPadB) press_b_ = 0;
        if ((pressed & (kPadA | kPadB)) || tap) {
            open_ = false;
            return Sound::Close;
        }
        return Sound::None;
    }
    std::vector<Command> draw() const {
        std::vector<Command> out;
        if (!open_) return out;
        const float t = ticked_ ? opening_ : 1.0f, fade = std::min(1.0f, t * 1.6f);
        Command shade;
        shade.kind = Command::Rect, shade.x = -400, shade.y = -200, shade.w = 2080, shade.h = 1120;
        shade.rgba = (kShade & 0xFFFFFF00u) | uint32_t(float(kShade & 0xFF) * 0.6f * fade);
        out.push_back(shade);
        const size_t from = out.size();
        out.push_back(box("nb:info", kX, kY, kW, kH));
        const std::string title = fit(title_, 40, kW - 80);
        out.push_back(text(kX + (kW - text_width(bold(title), 40)) / 2, kY + 26, 40, kTitle, bold(title)));
        const std::vector<std::string> lines = wrap(text_, 30, kW - 90, 3, kMessage);
        float y = kY + 92 + (3 - float(lines.size())) * 19;
        for (const std::string& l : lines) {
            out.push_back(text(kX + (kW - text_width(message(l), 30)) / 2, y, 30, kBody, message(l)));
            y += 38;
        }
        // the prompt: B (or A, or a tap) closes it
        const float size = 32, w = size + 6 + text_width("Back", size * 0.8f);
        const float px = kX + kW - 30 - w, py = kY + kH + 12;
        const size_t prompt_from = out.size();
        out.push_back(text(px, py, size, kWhite, "\xEE\x80\x81"));
        out.push_back(text(px + size + 6, py + size * 0.1f, size * 0.8f, kWhite, bold("Back")));
        if (ticked_ && press_b_ < 1.0f) transform(out, prompt_from, px + size / 2, py + size / 2, 1.0f - 0.1f * std::sin(press_b_ * 3.14159265f), 0, 0, 1);
        const float s = 0.9f + 0.1f * (1.0f - (1.0f - t) * (1.0f - t));
        transform(out, from, kX + kW / 2, kY + kH / 2, s, 0, 0, fade);
        return out;
    }

private:
    static Command text(float x, float y, float size, uint32_t rgba, std::string s) {
        Command c;
        c.kind = Command::Text, c.x = x, c.y = y, c.size = size, c.rgba = rgba, c.text = std::move(s);
        return c;
    }
    static Command box(const char* name, float x, float y, float w, float h) {
        Command c;
        c.kind = Command::Picture, c.x = x, c.y = y, c.w = w, c.h = h, c.rgba = 0xFFFFFFFF;
        c.text = std::string(name) + "@" + std::to_string(int(std::lround(w))) + "x" + std::to_string(int(std::lround(h)));
        return c;
    }
    bool open_ = false, touched_ = false, ticked_ = false;
    uint32_t last_ = 0;
    float opening_ = 1, press_b_ = 1;
    std::string title_, text_;
};

}  // namespace story_notebook

// The story notebook's page: what it shows and how the player moves through it (docs/mod-sdk-v2.md,
// "The story notebook"). Pure code: input in, draw commands out (the port's HUD layer draws them on
// the TV and the GamePad); unit-tested in runtime/tools/story_notebook_test.cpp.
//
// The page looks and feels like the game's own menu windows (the item menu's mini windows, such as
// "Would you like to save?"): a window with a paper panel, text in the game's menu font, button
// plates with the game's A/B button pictures, the game's cursor (four corner brackets that breathe
// around what A acts on), and the menu's sounds. Coordinates are those of a 1280 x 720 screen,
// centered on wider screens.
//
//   List:   one row per quest (story_notebook_list.h order): the mod's emblem or the quest's portrait,
//           title and mod name, the next step, where, and the status (New, In progress, Not yet
//           available: condition, Done with a stamp). The tracked quest carries the chart ring.
//           Up/Down choose, A opens the quest, Left/Right the tabs (Quests, Archive), B closes.
//   Detail: the quest: next step and where (or "no place on the chart"), what is known so far, the
//           reward, and "Show on chart" (A): the one tracked quest; choosing it again untracks it.
//           B goes back to the list.
// Touch (and the mouse on the GamePad picture) taps rows, tabs and plates; the keyboard acts as the
// controller through the port's controls.
//
// Boxes are drawn as style markers (a Picture command without an image whose text names the box:
// "nb:window", "nb:panel", "nb:plate", "nb:plate-on", "nb:cursor", "nb:tab", "nb:tab-on", then
// "@<w>x<h>", the size it was laid out at):
// story_notebook_style.h turns them into the game's window art or into plain shapes. Text commands
// are drawn in the game's font (story_notebook_font.h); the button pictures are the Text of the
// game's icon characters (U+E000 A, U+E001 B).
//
// Feel (tick(dt), seconds): the window opens with a quick scale-and-fade, switching tabs or opening a
// quest turns the page (the content slides in from the side it comes from), the cursor glides to the
// new row and breathes, A/B press their plates and prompts down. Action::sound names the menu sound of
// what the input did (story_notebook.cpp plays the game's own). Without ticks the page is at rest.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "guest_hud.h"
#include "story_notebook_list.h"
#include "story_notebook_art.h"

namespace story_notebook {

using guestmods::hud::Command;

// the controller as the page reads it (VPAD bits as input.h; touch in page coordinates)
struct PadInput {
    uint32_t buttons = 0;
    float ly = 0;
    bool touch = false;
    float x = 0, y = 0;
};
inline constexpr uint32_t kPadX = 0x2000;
inline constexpr uint32_t kPadA = 0x8000, kPadB = 0x4000, kPadUp = 0x0200, kPadDown = 0x0100, kPadLeft = 0x0800, kPadRight = 0x0400;

// page geometry (1280 x 720)
inline constexpr float kPageX = 150, kPageY = 44, kPageW = 980, kPageH = 632;
inline constexpr float kListX = kPageX + 70, kListY = kPageY + 108, kRowH = 88, kListW = kPageW - 130;
inline constexpr unsigned kRowsShown = 5;
// plates of the detail view and the close mark
inline constexpr float kTrackX = kPageX + kPageW - 380, kTrackY = kPageY + kPageH - 92, kTrackW = 320, kTrackH = 58;
inline constexpr float kBackX = kPageX + 60, kBackY = kTrackY, kBackW = 190, kBackH = 58;
inline constexpr float kCloseX = kPageX + kPageW - 46, kCloseY = kPageY + 46, kCloseR = 22;
// the two tabs: the quests (active, new, not yet available) and the archive (done)
inline constexpr float kTabY = kPageY + 34, kTabH = 48, kTabW = 180, kTab0X = kPageX + kPageW - 490, kTab1X = kTab0X + kTabW + 12;

// colours (RGBA): the game's menu window (green paper, dark green ink) and the status marks
inline constexpr uint32_t kShade = 0x0A140A88, kInk = 0x003800FF, kInkSoft = 0x3C6B2EFF, kInkLight = 0x5E7F4AFF, kPlateInk = 0x0B3D06FF,
                          kNew = 0x2E8A2EFF, kProgress = 0x2C6CA8FF, kLocked = 0x7C8A6CFF, kStamp = 0xB2392BFF, kRing = 0xFFFFFFFF,
                          kRingBack = 0xD9822BFF, kRule = 0xA9D08AFF, kWhite = 0xFFFFFFFF, kSeal = 0xF2E4C2FF;

// Text measure: the game's font when it is loaded (story_notebook.cpp sets it), else an average
// advance of the overlay font (as a share of the size; generous).
inline constexpr float kAdvance = 0.40f;
inline float (*g_text_measure)(const std::string& text, float size) = nullptr;
// A text that starts with kBold is drawn in the game's bordered menu style (titles, tabs, plates;
// story_notebook_font.h), others in its plain letters.
// kMessage: in the game's message font (story_notebook_font.h).
inline const std::string kBold = "\xEE\x83\xB1", kMessage = "\xEE\x83\xB2";  // U+E0F1, U+E0F2
inline std::string bold(const std::string& s) { return kBold + s; }
inline std::string message(const std::string& s) { return kMessage + s; }
inline float text_width(const std::string& s, float size) {
    if (g_text_measure) return g_text_measure(s, size);
    size_t n = 0, skip = 0;
    while (s.compare(skip, 3, kBold) == 0 || s.compare(skip, 3, kMessage) == 0) skip += 3;
    for (size_t i = skip; i < s.size(); i++) n += (s[i] & 0xC0) != 0x80;
    return float(n) * size * kAdvance;
}
inline size_t char_bytes(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

// One line of text cut to the width, with "...". `style`: the style code it is drawn with (kMessage:
// measured in that font).
inline std::string fit(const std::string& text, float size, float width, const std::string& style = "") {
    if (text_width(style + text, size) <= width) return text;
    std::string best;
    for (size_t i = 0; i < text.size();) {
        const size_t next = std::min(text.size(), i + char_bytes((unsigned char)text[i]));
        if (text_width(style + text.substr(0, next) + "...", size) > width) break;
        best = text.substr(0, next);
        i = next;
    }
    return best + "...";
}

// Text broken into lines of the width at spaces (and at the mod's own line breaks), at most `lines`.
inline std::vector<std::string> wrap(const std::string& text, float size, float width, unsigned lines, const std::string& style = "") {
    std::vector<std::string> out;
    std::string line;
    bool any = false;
    auto flush = [&] {
        out.push_back(line);
        line.clear();
        any = false;
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(" \n", start);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(start, end - start);
        if (any && text_width(style + line + " " + word, size) > width) flush();
        if (any) line += ' ';
        line += word;
        any = true;
        if (end < text.size() && text[end] == '\n') flush();
        start = end + 1;
    }
    if (any || out.empty()) flush();
    if (out.size() > lines) {
        out.resize(lines);
        out.back() = fit(out.back() + " ...........", size, width, style);
    }
    return out;
}

// what a finished quest's detail says (under "Finished", with no Where)
inline constexpr const char* kFinished = "All steps done.";

// Whether a quest's detail offers "Show on chart" / "Stop showing": not for a done quest, nor for one
// whose current step has no place (a quest not started yet: step 1's); a tracked one can always be untracked.
inline bool trackable(const Row& r) { return r.status != Status::Done && (r.quest->has_place || r.tracked); }

// The detail's "Where" text. A quest not started yet names step 1's place, marked as not started.
inline std::string where_text(const Row& r) {
    const Quest& q = *r.quest;
    if (r.status == Status::New || r.status == Status::NotAvailable) {
        if (!q.where.empty()) return "Not started yet: " + q.where;
        if (q.has_place) return "Not started yet: marked on the sea chart.";
    }
    return q.where.empty() ? std::string(q.has_place ? "Marked on the sea chart." : "This step has no place on the chart.") : q.where;
}

inline const char* status_text(Status s) {
    switch (s) {
    case Status::InProgress: return "In progress";
    case Status::New: return "New";
    case Status::NotAvailable: return "Not yet available";
    case Status::Done: return "Done";
    }
    return "";
}

// The mod's emblem colour: a muted ink, the same for all quests of a mod.
inline uint32_t emblem_colour(const std::string& mod) {
    static constexpr uint32_t kInks[] = {0x7D3B2EFF, 0x2E5E6EFF, 0x5B6B2EFF, 0x6A3F72FF, 0x8A5A1EFF, 0x34507EFF};
    return kInks[story_mail::letter_key(mod, "") % (sizeof kInks / sizeof kInks[0])];
}

// The screen that shows the game's item menu, where the page (and the bag icon) belong: the GamePad,
// or the TV when the Pro Controller has the game show its menus there. HUD screens: 0 TV, 1 GamePad.
inline uint32_t menu_screen(bool pro_controller) { return pro_controller ? 0u : 1u; }
// The page is laid out for 1280 x 720; the GamePad's HUD lists are 854 x 480.
inline void fit_to_screen(std::vector<Command>& cmds, uint32_t screen) {
    if (screen != 1) return;
    const float k = 854.0f / 1280.0f;
    for (Command& c : cmds) {
        c.x *= k, c.y *= k, c.size *= k, c.thickness *= k;
        if (c.kind != Command::Circle && c.kind != Command::CircleOutline) c.w *= k, c.h *= k;
    }
}

// Scale draw commands (from index `from`) around a point, move them and fade them (the animations).
inline void transform(std::vector<Command>& cmds, size_t from, float cx, float cy, float scale, float dx, float dy, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    for (size_t i = from; i < cmds.size(); i++) {
        Command& c = cmds[i];
        c.x = cx + (c.x - cx) * scale + dx, c.y = cy + (c.y - cy) * scale + dy;
        c.size *= scale, c.thickness = std::max(0.01f, c.thickness * scale);
        if (c.kind != Command::Circle && c.kind != Command::CircleOutline) c.w *= scale, c.h *= scale;
        c.rgba = (c.rgba & 0xFFFFFF00u) | uint32_t(std::lround(float(c.rgba & 0xFF) * alpha));
    }
}

// The menu sound of an input (story_notebook.cpp plays the game's own: SE_MENU_CURSOR, ...)
enum class Sound : uint8_t { None, Cursor, Decide, Cancel, Tab, Open, Close, TrackOn, TrackOff };

// What the page knows of the world besides the quests (story_notebook.cpp sets it every frame)
struct Scene {
    bool framed = false;      // the painted frame and row banners are loaded (story_notebook_art.h)
    bool pieces = false;      // the detail page's painted pieces (sheet, bands, chart card, plates)
    bool header = false;      // the header band (optional)
    bool close_in_detail = false;  // the detail page shows the close button (the page drawn as a game layout)
    bool link_known = false;  // Link's place on the sea chart
    float link_x = 0, link_z = 0;
};

// The mini sea chart: a world place (the chart ring's x/z) as a share of the 7 x 7 squares
inline void chart_share(float x, float z, float& u, float& v) {
    constexpr float kHalf = 350000.0f;
    u = std::clamp((x + kHalf) / (2 * kHalf), 0.0f, 1.0f), v = std::clamp((z + kHalf) / (2 * kHalf), 0.0f, 1.0f);
}

class Page {
public:
    enum class Mode : uint8_t { Closed, List, Detail };
    struct Action {
        enum Kind : uint8_t { None, Close, Toggle, OpenChart } kind = None;
        uint16_t key = 0;  // Toggle, OpenChart: the quest
        Sound sound = Sound::None;
    };

    Mode mode() const { return mode_; }
    int tab() const { return tab_; }
    void set_scene(const Scene& scene) { scene_ = scene; }
    const Scene& scene() const { return scene_; }

    // Where things are: the classic page (the constants above) or the painted frame
    struct Box {
        float x = 0, y = 0, w = 0, h = 0;
        bool in(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };
    struct Geo {
        Box tab[2], back, track, content;
        float close_x = 0, close_y = 0, close_r = 0;
        float list_x = 0, list_y = 0, list_w = 0, row_h = 0;
    };
    // the frame picture's place: 4:3, the page's height less a margin, centred
    static Box frame_box() {
        const float h = 696, w = h * art::frame::kAspect;
        return {640 - w / 2, 12, w, h};
    }
    static Box frame_part(const art::Rect& r) {
        const Box f = frame_box();
        return {f.x + r.x0 * f.w, f.y + r.y0 * f.h, (r.x1 - r.x0) * f.w, (r.y1 - r.y0) * f.h};
    }
    Geo geo() const {
        Geo g;
        if (!scene_.framed) {
            g.tab[0] = {kTab0X, kTabY, kTabW, kTabH}, g.tab[1] = {kTab1X, kTabY, kTabW, kTabH};
            g.back = {kBackX, kBackY, kBackW, kBackH}, g.track = {kTrackX, kTrackY, kTrackW, kTrackH};
            g.close_x = kCloseX, g.close_y = kCloseY, g.close_r = kCloseR;
            g.list_x = kListX, g.list_y = kListY, g.list_w = kListW, g.row_h = kRowH;
            g.content = {kPageX + 40, kPageY + 90, kPageW - 80, kPageH - 190};
            return g;
        }
        g.tab[0] = frame_part(art::frame::kTab0), g.tab[1] = frame_part(art::frame::kTab1);
        g.back = frame_part(art::frame::kPlateLeft), g.track = frame_part(art::frame::kPlateRight);
        const Box slot = frame_part(art::frame::kSlot);
        g.close_x = slot.x + slot.w / 2, g.close_y = slot.y + slot.h / 2, g.close_r = slot.w / 2;
        g.content = frame_part(art::frame::kContent);
        g.list_x = g.content.x + 8, g.list_y = g.content.y + 8, g.list_w = g.content.w - 16, g.row_h = (g.content.h - 16) / float(kRowsShown);
        return g;
    }
    // the rows of the open tab: the quests (tracked first) or the archive (done quests)
    std::vector<Row> shown(const std::vector<Row>& rows) const {
        std::vector<Row> out;
        for (const Row& r : rows)
            if ((r.status == Status::Done) == (tab_ == 1)) out.push_back(r);
        return out;
    }
    bool open() const { return mode_ != Mode::Closed; }
    uint16_t selected_key() const { return selected_; }
    void show() {
        mode_ = Mode::List;
        tab_ = 0;
        selected_ = 0;
        top_ = 0;
        last_ = ~0u;  // buttons held when the page opens (the A that opened it) do nothing
        touched_ = true;
        opening_ = 0;  // the window opens (tick)
        turn_ = 1, turn_dir_ = 0;
        press_a_ = press_b_ = bump_ = glide_ = 1;
    }
    void hide() { mode_ = Mode::Closed; }

    // Time for the animations (seconds since the last call).
    void tick(float dt) {
        dt = std::clamp(dt, 0.0f, 0.1f);
        time_ += dt;
        opening_ = std::min(1.0f, opening_ + dt / kOpenTime);
        turn_ = std::min(1.0f, turn_ + dt / kTurnTime);
        press_a_ = std::min(1.0f, press_a_ + dt / kPressTime);
        press_b_ = std::min(1.0f, press_b_ + dt / kPressTime);
        bump_ = std::min(1.0f, bump_ + dt / kBumpTime);
        glide_ = std::min(1.0f, glide_ + dt / kGlideTime);
        ticked_ = true;
    }
    float opening() const { return ticked_ ? opening_ : 1.0f; }

    // One game frame of input. Returns what the notebook has to do (Close: the page closed itself)
    // and the menu sound of what happened.
    Action input(const PadInput& pad, const std::vector<Row>& all) {
        Action act;
        if (mode_ == Mode::Closed) return act;
        const std::vector<Row> rows = shown(all);
        uint32_t held = pad.buttons;
        if (pad.ly > 0.5f) held |= kPadUp;
        if (pad.ly < -0.5f) held |= kPadDown;
        if (last_ == ~0u) last_ = held;
        const uint32_t pressed = held & ~last_;
        // Up/Down repeat while held, like the game's cursors
        uint32_t moved = pressed & (kPadUp | kPadDown);
        if (held & (kPadUp | kPadDown) && !(pressed & (kPadUp | kPadDown))) {
            if (++repeat_ >= 18 && (repeat_ - 18) % 5 == 0) moved = held & (kPadUp | kPadDown);
        } else {
            repeat_ = 0;
        }
        last_ = held;
        const bool tap = pad.touch && !touched_;
        touched_ = pad.touch;
        if (pressed & kPadA) press_a_ = 0;
        if (pressed & kPadB) press_b_ = 0;
        int index = index_of(rows);
        if (mode_ == Mode::List) {
            // Left/Right or a tap on a tab: the quests or the archive
            int want = tab_;
            if (pressed & kPadLeft) want = 0;
            if (pressed & kPadRight) want = 1;
            const Geo g = geo();
            if (tap && g.tab[0].in(pad.x, pad.y)) want = 0;
            if (tap && g.tab[1].in(pad.x, pad.y)) want = 1;
            if (want != tab_) {
                turn(want > tab_ ? 1 : -1);
                tab_ = want;
                top_ = 0;
                const std::vector<Row> now = shown(all);
                selected_ = now.empty() ? 0 : now[0].quest->key;
                act.sound = Sound::Tab;
                return act;
            }
            if (rows.empty()) index = -1;
            else if (index < 0) index = 0;
            const int before = index;
            const float before_y = row_y(before);
            if (index >= 0 && (moved & kPadUp) && index > 0) index--;
            if (index >= 0 && (moved & kPadDown) && index + 1 < int(rows.size())) index++;
            // the painted frame's bottom plates: B Close on the left, A Open on the right
            const bool close_plate = scene_.framed && tap && g.back.in(pad.x, pad.y), open_plate = scene_.framed && tap && g.track.in(pad.x, pad.y);
            if ((tap && hit_close(pad.x, pad.y)) || close_plate) {
                if (close_plate) press_b_ = 0;
                hide();
                act.kind = Action::Close;
                act.sound = Sound::Close;
                return act;
            }
            int tapped = tap ? row_at(pad.x, pad.y, rows.size()) : -1;
            if (open_plate && index >= 0) tapped = index;
            if (tapped >= 0) index = tapped;
            if (index >= 0) {
                selected_ = rows[size_t(index)].quest->key;
                if (unsigned(index) < top_) top_ = unsigned(index);
                if (unsigned(index) >= top_ + kRowsShown) top_ = unsigned(index) - kRowsShown + 1;
            }
            if (index != before && before >= 0) {
                act.sound = Sound::Cursor;
                bump_ = 0, glide_ = 0, glide_from_ = before_y;
            }
            if (index >= 0 && ((pressed & kPadA) || tapped >= 0)) {
                mode_ = Mode::Detail;
                turn(1);
                if (tapped >= 0) press_a_ = 0;
                act.sound = Sound::Decide;
            } else if (pressed & kPadB) {
                hide();
                act.kind = Action::Close;
                act.sound = Sound::Close;
            }
        } else {  // Detail
            if (index < 0) {  // the quest is gone (its mod stopped registering it)
                mode_ = Mode::List;
                return act;
            }
            const Row& r = rows[size_t(index)];
            const bool trackable = ::story_notebook::trackable(r);
            const Geo g = geo();
            const bool back_tap = tap && g.back.in(pad.x, pad.y);
            const bool track_tap = tap && g.track.in(pad.x, pad.y);
            const bool chart_tap = tap && !r.quest->chart.empty() && chart_plate(g).in(pad.x, pad.y);
            if (!r.quest->chart.empty() && r.status != Status::Done && ((pressed & kPadX) || chart_tap)) {
                // the step's chart in the MAP screen's chart list: the page closes, the map opens
                hide();
                act.kind = Action::OpenChart;
                act.key = r.quest->key;
                act.sound = Sound::Decide;
            } else if (tap && hit_close(pad.x, pad.y)) {
                hide();
                act.kind = Action::Close;
                act.sound = Sound::Close;
            } else if ((pressed & kPadB) || back_tap) {
                mode_ = Mode::List;
                turn(-1);
                if (back_tap) press_b_ = 0;
                act.sound = Sound::Cancel;
            } else if (trackable && ((pressed & kPadA) || track_tap)) {
                act.kind = Action::Toggle;
                act.key = r.quest->key;
                if (track_tap) press_a_ = 0;
                act.sound = r.tracked ? Sound::TrackOff : Sound::TrackOn;
            }
        }
        return act;
    }

    // The page as draw commands (empty when closed). The caller draws the portraits with its own
    // picture commands at emblem_place() (moved by place_portrait()).
    std::vector<Command> draw(const std::vector<Row>& all) const {
        std::vector<Command> out;
        if (mode_ == Mode::Closed) return out;
        const float fade = std::min(1.0f, opening() * 1.6f);
        out.push_back(rect(-400, -200, 2080, 1120, (kShade & 0xFFFFFF00u) | uint32_t(float(kShade & 0xFF) * fade)));  // the game behind, dimmed
        const size_t window_from = out.size();
        const std::vector<Row> rows = shown(all);
        if (scene_.framed) framed_sheet(out);
        else sheet(out);
        if (mode_ == Mode::List) tabs(out, all);
        // the content turns like a page: it slides in from the side it comes from
        const size_t content_from = out.size();
        const int index = index_of(rows);
        if (mode_ == Mode::Detail && index >= 0) {
            if (scene_.framed) framed_detail(out, rows[size_t(index)]);
            else detail(out, rows[size_t(index)]);
        } else if (scene_.framed) {
            framed_list(out, rows, index);
        } else {
            list(out, rows, index);
        }
        const float t = turned();
        if (t < 1.0f) transform(out, content_from, 0, 0, 1.0f, float(turn_dir_) * (1.0f - t) * kTurnSlide, 0, t);
        // the window opens: scales up from 92 % with a little overshoot, fading in
        transform(out, window_from, kPageX + kPageW / 2, kPageY + kPageH / 2, window_scale(), 0, 0, fade);
        return out;
    }

    // where a row's emblem goes (for the caller's portrait pictures): center and radius
    void emblem_place(unsigned shown_index, float& cx, float& cy, float& r) const {
        const Geo g = geo();
        if (scene_.framed) {  // the banner's medallion
            const float y = g.list_y + g.row_h * float(shown_index) + 2, h = g.row_h - 4;
            cx = art::row_x(art::banner::kMedallionX, g.list_x, g.list_w, h), cy = y + art::banner::kMedallionY * h;
            r = art::banner::kMedallionR * h * 0.86f;
            return;
        }
        cx = kListX + 50;
        cy = kListY + kRowH * float(shown_index) + kRowH * 0.5f;
        r = 32;
    }
    // where the detail page's portrait goes (the quest giver, next to "From"): centre and radius
    void portrait_place(float& cx, float& cy, float& r) const {
        const Geo g = geo();
        if (scene_.framed && scene_.pieces) {  // under the title's wave line
            const Box u = frame_part(art::detail::kUnderline);
            cx = u.x + 22, cy = u.y + u.h + 26, r = 20;
            return;
        }
        cx = g.content.x + 18 + 24, cy = g.content.y + 92, r = 24;
    }
    unsigned top() const { return top_; }
    // a portrait at (x, y, size) follows the page's animation: the content's slide and the window's scale
    void place_portrait(float& x, float& y, float& size, float& alpha) const {
        const float t = turned();
        x += float(turn_dir_) * (1.0f - t) * kTurnSlide;
        const float s = window_scale(), cx = kPageX + kPageW / 2, cy = kPageY + kPageH / 2;
        x = cx + (x - cx) * s, y = cy + (y - cy) * s, size *= s;
        alpha = std::min(1.0f, opening() * 1.6f) * t;
    }

private:
    static constexpr float kOpenTime = 0.22f, kTurnTime = 0.20f, kPressTime = 0.16f, kBumpTime = 0.18f, kGlideTime = 0.10f, kTurnSlide = 70.0f;
    static float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
    static float ease_out_back(float t) {
        const float c = 1.6f, u = t - 1.0f;
        return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
    }
    float window_scale() const { return 0.92f + 0.08f * ease_out_back(opening()); }
    float turned() const { return ticked_ ? ease_out(turn_) : 1.0f; }
    // a pressed plate or prompt: down to 90 % and back
    float press_scale(float t) const { return ticked_ ? 1.0f - 0.10f * std::sin(std::clamp(t, 0.0f, 1.0f) * 3.14159265f) : 1.0f; }
    void turn(int dir) { turn_ = 0, turn_dir_ = dir; }
    float row_y(int index) const { return kListY + kRowH * float(index - int(top_)) + 4; }

    int index_of(const std::vector<Row>& rows) const {
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].quest->key == selected_) return int(i);
        return -1;
    }
    static bool inside(float x, float y, float rx, float ry, float rw, float rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
    bool hit_close(float x, float y) const {
        // the HUD's detail page has the slot under its header band; the layout's shows the close button
        if (mode_ == Mode::Detail && scene_.framed && scene_.pieces && !scene_.close_in_detail) return false;
        const Geo g = geo();
        return (x - g.close_x) * (x - g.close_x) + (y - g.close_y) * (y - g.close_y) <= (g.close_r + 8) * (g.close_r + 8);
    }
    int row_at(float x, float y, size_t count) const {
        const Geo g = geo();
        if (!inside(x, y, g.list_x, g.list_y, g.list_w, g.row_h * kRowsShown)) return -1;
        const unsigned i = top_ + unsigned((y - g.list_y) / g.row_h);
        return i < count ? int(i) : -1;
    }

    static Command rect(float x, float y, float w, float h, uint32_t rgba) {
        Command c;
        c.kind = Command::Rect, c.x = x, c.y = y, c.w = w, c.h = h, c.rgba = rgba;
        return c;
    }
    static Command text(float x, float y, float size, uint32_t rgba, std::string s) {
        Command c;
        c.kind = Command::Text, c.x = x, c.y = y, c.size = size, c.rgba = rgba, c.text = std::move(s);
        return c;
    }
    static Command circle(float x, float y, float r, uint32_t rgba, bool filled, float t = 2) {
        Command c;
        c.kind = filled ? Command::Circle : Command::CircleOutline, c.x = x, c.y = y, c.size = r, c.rgba = rgba, c.thickness = t;
        return c;
    }
    static Command line(float x0, float y0, float x1, float y1, uint32_t rgba, float t) {
        Command c;
        c.kind = Command::Line, c.x = x0, c.y = y0, c.w = x1 - x0, c.h = y1 - y0, c.rgba = rgba, c.thickness = t;
        return c;
    }
    // a style marker: a box the style draws (story_notebook_style.h)
    static Command box(const char* name, float x, float y, float w, float h) {
        Command c;
        // the box's name and its size as laid out (the style composes it once at that size; the
        // animations only scale the picture)
        c.kind = Command::Picture, c.x = x, c.y = y, c.w = w, c.h = h, c.rgba = 0xFFFFFFFF;
        c.text = std::string(name) + "@" + std::to_string(int(std::lround(w))) + "x" + std::to_string(int(std::lround(h)));
        return c;
    }
    // a pressed plate or prompt: its commands scaled around their centre
    static void press(std::vector<Command>& out, size_t from, float cx, float cy, float s) {
        if (s != 1.0f) transform(out, from, cx, cy, s, 0, 0, 1.0f);
    }
    // a button prompt: the game's button picture (A or B), then what it does; returns its width
    float prompt(std::vector<Command>& out, float x, float y, bool a, const char* what, float pressed, float size = 36) const {
        const size_t from = out.size();
        out.push_back(text(x, y, size, kWhite, a ? std::string("\xEE\x80\x80") : std::string("\xEE\x80\x81")));
        out.push_back(text(x + size + 6, y + size * 0.1f, size * 0.8f, kInk, bold(what)));
        press(out, from, x + size / 2, y + size / 2, press_scale(pressed));
        return size + 6 + text_width(bold(what), size * 0.8f);
    }
    static float prompt_width(const char* what, float size = 36) { return size + 6 + text_width(bold(what), size * 0.8f); }
    // a plate the player can press (A on it, or a tap): the game's button plate with its prompt
    void plate(std::vector<Command>& out, float x, float y, float w, float h, bool on, bool a, const char* what) const {
        const size_t from = out.size();
        out.push_back(box(on ? "nb:plate-on" : "nb:plate", x, y, w, h));
        const float size = 34, width = prompt_width(what, size);
        prompt(out, x + (w - width) / 2, y + (h - size) / 2, a, what, 1.0f, size);
        press(out, from, x + w / 2, y + h / 2, press_scale(a ? press_a_ : press_b_));
        if (on) cursor(out, x, y, w, h);
    }
    // "Open chart" (X, or a tap): between the Back and the track plates, else above the track plate
    static Box chart_plate(const Geo& g) {
        const float gap = 16, x = g.back.x + g.back.w + gap, w = g.track.x - gap - x;
        if (w >= 200) return {x, g.track.y, w, g.track.h};
        return {g.track.x, g.track.y - g.track.h - 10, g.track.w, g.track.h};
    }
    void open_chart_plate(std::vector<Command>& out, const Row& r) const {
        if (r.quest->chart.empty() || r.status == Status::Done) return;
        const Box b = chart_plate(geo());
        out.push_back(box("nb:plate", b.x, b.y, b.w, b.h));
        const float size = std::min(30.0f, b.h * 0.55f), width = size + 8 + text_width(bold("Open chart"), size * 0.8f) + 40;
        float x = b.x + (b.w - width) / 2;
        const float cy = b.y + b.h / 2;
        out.push_back(circle(x + size / 2, cy, size / 2, 0x3A3A3AFF, true));  // the X button
        out.push_back(text(x + size / 2 - text_width(bold("X"), size * 0.6f) / 2, cy - size * 0.32f, size * 0.6f, kWhite, bold("X")));
        x += size + 8;
        out.push_back(text(x, cy - size * 0.42f, size * 0.8f, kInk, bold("Open chart")));
        chart_scroll(out, x + text_width(bold("Open chart"), size * 0.8f) + 8, cy - 13, 30, 26);
    }
    // the game's cursor: four corner brackets around what A acts on, breathing in and out
    void cursor(std::vector<Command>& out, float x, float y, float w, float h) const {
        const float breathe = ticked_ ? 2.0f + 2.0f * std::sin(time_ * 6.2831853f * 0.9f) : 2.0f;
        const float b = 4 + breathe;
        out.push_back(box("nb:cursor", x - b, y - b, w + 2 * b, h + 2 * b));
    }
    // the chart ring the tracked quest carries (the white story ring of the sea chart)
    static void ring(std::vector<Command>& out, float cx, float cy, float r) {
        out.push_back(circle(cx, cy, r, kRingBack, true));
        out.push_back(circle(cx, cy, r * 0.62f, kRing, false, 3));
    }
    static void stamp(std::vector<Command>& out, float cx, float cy) {
        out.push_back(circle(cx, cy, 30, kStamp, false, 3));
        out.push_back(circle(cx, cy, 24, kStamp, false, 1.5f));
        out.push_back(text(cx - text_width("DONE", 19) / 2, cy - 10, 19, kStamp, "DONE"));
    }

    void sheet(std::vector<Command>& out) const {
        out.push_back(box("nb:window", kPageX, kPageY, kPageW, kPageH));
        out.push_back(text(kPageX + 66, kPageY + 36, 42, kInk, bold("Story Notebook")));
        // the close mark (a tap closes the page, as B does)
        out.push_back(circle(kCloseX, kCloseY, kCloseR, 0xFFFFFFC8, true));
        out.push_back(circle(kCloseX, kCloseY, kCloseR, kInkSoft, false, 2.5f));
        out.push_back(line(kCloseX - 8, kCloseY - 8, kCloseX + 8, kCloseY + 8, kInkSoft, 3));
        out.push_back(line(kCloseX + 8, kCloseY - 8, kCloseX - 8, kCloseY + 8, kInkSoft, 3));
    }

    void list(std::vector<Command>& out, const std::vector<Row>& rows, int index) const {
        out.push_back(box("nb:panel", kListX - 22, kListY - 14, kListW + 34, kRowH * kRowsShown + 26));
        if (rows.empty() && tab_ == 0) {
            out.push_back(text(kListX + 20, kListY + 60, 30, kInkSoft, "No stories yet."));
            out.push_back(text(kListX + 20, kListY + 104, 25, kInkSoft, "When a story mod writes to you, its quests are kept here."));
        } else if (rows.empty()) {
            out.push_back(text(kListX + 20, kListY + 60, 30, kInkSoft, "No finished stories yet."));
        }
        for (unsigned i = 0; i < kRowsShown && top_ + i < rows.size(); i++) {
            const Row& r = rows[top_ + i];
            const Quest& q = *r.quest;
            const float y = kListY + kRowH * float(i);
            const size_t from = out.size();
            const bool selected = int(top_ + i) == index;
            if (selected) out.push_back(box("nb:plate-on", kListX - 6, y + 4, kListW + 2, kRowH - 8));
            else if (i && int(top_ + i) != index + 1) out.push_back(line(kListX + 10, y, kListX + kListW - 20, y, kRule, 2));
            float cx, cy, rad;
            emblem_place(i, cx, cy, rad);
            if (q.portrait.empty()) {  // the mod's emblem: its initial on a seal
                out.push_back(circle(cx, cy, rad, emblem_colour(q.mod), true));
                out.push_back(circle(cx, cy, rad - 5, kSeal, false, 1.5f));
                std::string initial = q.mod_name.empty() ? "?" : q.mod_name.substr(0, 1);
                out.push_back(text(cx - text_width(initial, 32) / 2, cy - 16, 32, kSeal, initial));
            }
            const float tx = kListX + 100, tw = kListW - 100 - 230;
            const bool dim = r.status == Status::Done || r.status == Status::NotAvailable;
            const std::string title = fit(q.title, 27, tw * 0.64f);
            out.push_back(text(tx, y + 9, 27, dim ? kInkSoft : kInk, bold(title)));
            const float after = tx + std::min(tw * 0.64f, text_width(title, 27)) + 16;
            out.push_back(text(after, y + 14, 19, kInkLight, fit("- " + q.mod_name, 19, tx + tw - after)));
            std::string next = r.status == Status::Done ? "The story is told." : r.status == Status::NotAvailable ? "Not yet available: " + q.condition : q.next;
            out.push_back(text(tx, y + 38, 22, dim ? kInkSoft : kInk, fit(next, 22, tw)));
            std::string where = q.where.empty() ? std::string(q.has_place ? "On the sea chart" : "No place on the chart for this step")
                                                : "Where: " + q.where;
            if (r.status == Status::New || r.status == Status::InProgress) out.push_back(text(tx, y + 61, 18, kInkSoft, fit(where, 18, tw)));
            status(out, r, kListX + kListW - 216, y);
            // the selected row: a little bounce as the cursor arrives
            if (selected && ticked_ && bump_ < 1.0f) press(out, from, kListX + kListW / 2, y + kRowH / 2, 1.0f + 0.025f * std::sin(bump_ * 3.14159265f));
        }
        // the cursor: around the selected row, gliding there from the row before
        if (index >= int(top_) && index < int(top_ + kRowsShown)) {
            float y = row_y(index);
            if (ticked_ && glide_ < 1.0f) y = glide_from_ + (y - glide_from_) * ease_out(glide_);
            cursor(out, kListX - 6, y, kListW + 2, kRowH - 8);
        }
        if (top_ > 0) out.push_back(text(kListX + kListW / 2, kListY - 34, 24, kInkSoft, "^"));
        if (top_ + kRowsShown < rows.size()) out.push_back(text(kListX + kListW / 2, kListY + kRowH * kRowsShown + 6, 24, kInkSoft, "v"));
        // prompts along the bottom, as the game's menus show them
        const float hy = kPageY + kPageH - 66;
        out.push_back(text(kPageX + 66, hy + 6, 23, kInkSoft, tab_ == 0 ? "Right: Archive" : "Left: Quests"));
        float x = kPageX + kPageW - 66 - prompt_width("Close");
        prompt(out, x, hy, false, "Close", press_b_);
        if (!rows.empty()) {
            x -= 36 + prompt_width("Open");
            prompt(out, x, hy, true, "Open", press_a_);
        }
    }

    void tabs(std::vector<Command>& out, const std::vector<Row>& all) const {
        size_t done = 0;
        for (const Row& r : all) done += r.status == Status::Done;
        const std::string labels[2] = {"Quests", "Archive" + (done ? " (" + std::to_string(done) + ")" : std::string())};
        const Geo g = geo();
        for (int i = 0; i < 2; i++) {
            const bool on = i == tab_;
            const Box& t = g.tab[i];
            const float lift = on && !scene_.framed ? -3 : 0;
            if (scene_.framed) {  // the frame's own plates: the open tab lit, the other one shaded
                out.push_back(rect(t.x + 6, t.y + 6, t.w - 12, t.h - 12, on ? 0xFFFFFF50 : 0x1E3C1430));
            } else {
                out.push_back(box(on ? "nb:tab-on" : "nb:tab", t.x, t.y + lift, t.w, t.h));
            }
            const float size = scene_.framed ? 25 : 26, w = text_width(bold(labels[i]), size);
            out.push_back(text(t.x + (t.w - w) / 2, t.y + lift + (t.h - size) / 2, size, on ? kPlateInk : kInkSoft, bold(labels[i])));
        }
    }

    static void status(std::vector<Command>& out, const Row& r, float x, float y) {
        if (r.tracked) {
            ring(out, x + 16, y + 24, 14);
            out.push_back(text(x + 38, y + 12, 21, kInk, "On the chart"));
        }
        const float sy = y + (r.tracked ? 46 : 28);
        switch (r.status) {
        case Status::Done: stamp(out, x + 120, y + kRowH * 0.5f); break;
        case Status::New:
        case Status::InProgress: {  // a label in the colour of its state, as wide as its word
            const char* word = status_text(r.status);
            out.push_back(rect(x, sy, text_width(word, 21) + 22, 30, r.status == Status::New ? kNew : kProgress));
            out.push_back(text(x + 11, sy + 4, 21, kWhite, bold(word)));
            break;
        }
        case Status::NotAvailable: {
            Command c = rect(x, sy, text_width("Not yet available", 19) + 20, 30, kLocked);
            c.kind = Command::RectOutline, c.thickness = 2;
            out.push_back(c);
            out.push_back(text(x + 10, sy + 5, 19, kLocked, "Not yet available"));
            break;
        }
        }
    }

    void detail(std::vector<Command>& out, const Row& r) const {
        const Quest& q = *r.quest;
        const float x = kPageX + 80, w = kPageW - 160;
        float y = kPageY + 108;
        out.push_back(box("nb:panel", x - 22, y - 14, w + 44, kTrackY - y - 4));
        out.push_back(text(x, y, 34, kInk, bold(fit(q.title, 34, w - 260))));
        out.push_back(text(x, y + 40, 21, kInkLight, fit("from " + q.mod_name, 21, w - 260)));
        status(out, r, x + w - 220, y - 6);
        y += 80;
        auto section = [&](const char* head, const std::string& body, unsigned lines) {
            out.push_back(text(x, y, 21, kInkSoft, head));
            y += 25;
            for (const std::string& l : wrap(body, 24, w - 12, lines)) {
                out.push_back(text(x + 12, y, 24, kInk, l));
                y += 28;
            }
            y += 7;
        };
        if (r.status == Status::Done) {
            section("Finished", kFinished, 1);
        } else if (r.status == Status::NotAvailable) {
            section("Not yet available", q.condition, 2);
        } else {
            section("Next step", q.next.empty() ? std::string("(not written down yet)") : q.next, 2);
            if (r.status == Status::New) section("Where", where_text(r), 1);
            else section("Where", q.has_place ? (q.where.empty() ? std::string("Marked on the sea chart when you show it there.") : q.where)
                                         : (q.where.empty() ? std::string("This step has no place on the chart.")
                                                            : q.where + " (this step has no place on the chart)"),
                    1);
        }
        if (!q.hint.empty() && r.status != Status::Done) section("To continue", q.hint, 1);
        if (!q.known.empty()) section("What is known so far", q.known, 4);
        if (!q.reward.empty()) section("Reward", q.reward, 1);
        // plates: Back (B) and Show on chart (A); the cursor is on what A does
        plate(out, kBackX, kBackY, kBackW, kBackH, false, false, "Back");
        if (trackable(r)) {
            plate(out, kTrackX, kTrackY, kTrackW, kTrackH, true, true, r.tracked ? "Stop showing on chart" : "Show on chart");
            if (r.tracked && !q.has_place)
                out.push_back(text(kTrackX, kTrackY - 30, 19, kInkSoft, "No place on the chart for this step"));
        }
        open_chart_plate(out, r);
    }

    // ---- the painted frame (story_notebook_art.h) ----
    void framed_sheet(std::vector<Command>& out) const {
        const Box f = frame_box();
        const bool cleared = mode_ == Mode::Detail && scene_.pieces;  // the detail page: the header band's place
        out.push_back(box(cleared ? "nb:dframe" : "nb:frame", f.x, f.y, f.w, f.h));
        const Box t = frame_part(art::frame::kTitle);
        out.push_back(text(t.x, t.y, 44, kInk, bold("Story Notebook")));
        if (cleared) return;
        // the close mark in the round slot (a tap closes the page, as B does)
        const Geo g = geo();
        const float k = g.close_r * 0.42f;
        out.push_back(line(g.close_x - k, g.close_y - k, g.close_x + k, g.close_y + k, kInkSoft, 3.5f));
        out.push_back(line(g.close_x + k, g.close_y - k, g.close_x - k, g.close_y + k, kInkSoft, 3.5f));
    }
    // a prompt centred on one of the frame's painted plates
    void plate_label(std::vector<Command>& out, const Box& b, bool a, const char* what, bool scroll = false) const {
        const size_t from = out.size();
        const float size = 34, w = prompt_width(what, size) + (scroll ? 44 : 0);
        prompt(out, b.x + (b.w - w) / 2, b.y + (b.h - size) / 2 - 2, a, what, 1.0f, size);
        if (scroll) chart_scroll(out, b.x + (b.w + w) / 2 - 34, b.y + b.h / 2 - 15, 34, 30);
        press(out, from, b.x + b.w / 2, b.y + b.h / 2, press_scale(a ? press_a_ : press_b_));
    }
    // a small chart scroll with a red cross (the A plate's "Show on chart")
    static void chart_scroll(std::vector<Command>& out, float x, float y, float w, float h) {
        out.push_back(rect(x, y, w, h, 0xE9D9A8FF));
        Command o = rect(x, y, w, h, 0x8A6A3AFF);
        o.kind = Command::RectOutline, o.thickness = 2;
        out.push_back(o);
        out.push_back(rect(x - 3, y - 2, 5, h + 4, 0xC9A66BFF));
        out.push_back(rect(x + w - 2, y - 2, 5, h + 4, 0xC9A66BFF));
        for (int i = 0; i < 3; i++) out.push_back(line(x + 5 + i * 6, y + h * 0.7f - i * 3, x + 8 + i * 6, y + h * 0.7f - i * 3 - 2, 0xB2392BFF, 2));
        out.push_back(line(x + w - 13, y + 7, x + w - 5, y + 15, 0xD12E1EFF, 3));
        out.push_back(line(x + w - 5, y + 7, x + w - 13, y + 15, 0xD12E1EFF, 3));
    }

    void framed_list(std::vector<Command>& out, const std::vector<Row>& rows, int index) const {
        const Geo g = geo();
        if (rows.empty()) {
            out.push_back(text(g.content.x + 40, g.content.y + 60, 30, kInkSoft, tab_ == 0 ? "No stories yet." : "No finished stories yet."));
            if (tab_ == 0) out.push_back(text(g.content.x + 40, g.content.y + 104, 24, kInkSoft, "When a story mod writes to you, its quests are kept here."));
        }
        for (unsigned i = 0; i < kRowsShown && top_ + i < rows.size(); i++) {
            const Row& r = rows[top_ + i];
            const Quest& q = *r.quest;
            const float y = g.list_y + g.row_h * float(i) + 2, h = g.row_h - 4, x = g.list_x, w = g.list_w;
            const size_t from = out.size();
            const bool selected = int(top_ + i) == index;
            out.push_back(box("nb:row", x, y, w, h));
            if (selected) {  // a light wash over the body (not the medallion)
                const float bx = art::row_x(art::banner::kText.x0 - 0.02f, x, w, h);
                out.push_back(rect(bx, y + h * 0.12f, x + w - h * 0.25f - bx, h * 0.76f, 0xFFFFFF30));
            }
            // the medallion: the quest's portrait (drawn by the caller) or the mod's initial
            float cx, cy, rad;
            emblem_place(i, cx, cy, rad);
            if (q.portrait.empty()) {
                const std::string initial = q.mod_name.empty() ? "?" : q.mod_name.substr(0, 1);
                out.push_back(text(cx - text_width(bold(initial), rad * 1.3f) / 2, cy - rad * 0.68f, rad * 1.3f, kSeal, bold(initial)));
            }
            // title and next step on the banner's body, the status in its pill
            const float tx = art::row_x(art::banner::kText.x0, x, w, h), pill_x = art::row_x(art::banner::kPill.x0, x, w, h);
            const float pill_x1 = art::row_x(art::banner::kPill.x1, x, w, h), tw = pill_x - tx - 20;
            const bool dim = r.status == Status::Done || r.status == Status::NotAvailable;
            const std::string title = fit(q.title, 25, tw, kBold);
            out.push_back(text(tx, y + h * 0.13f, 25, dim ? kInkSoft : kInk, bold(title)));
            std::string next = r.status == Status::Done ? "The story is told." : r.status == Status::NotAvailable ? "Not yet available: " + q.condition : q.next;
            out.push_back(text(tx, y + h * 0.47f, 19, dim ? kInkSoft : kInk, fit(next, 19, tw)));
            const float py = y + art::banner::kPill.y0 * h, ph = (art::banner::kPill.y1 - art::banner::kPill.y0) * h;
            const uint32_t tint = r.status == Status::New ? 0x2E8A2EC8 : r.status == Status::NotAvailable ? 0x7C8A6CD8 : r.status == Status::Done ? 0x9C3A2AD0 : 0;
            if (tint) out.push_back(rect(pill_x + 2, py + 2, pill_x1 - pill_x - 4, ph - 4, tint));
            const char* word = r.status == Status::NotAvailable ? "Not yet" : status_text(r.status);
            const float ws = 17;
            out.push_back(text(pill_x + (pill_x1 - pill_x - text_width(bold(word), ws)) / 2, py + (ph - ws) / 2, ws, kWhite, bold(word)));
            if (r.tracked) ring(out, pill_x - 18, py + ph / 2, 12);
            if (selected && ticked_ && bump_ < 1.0f) press(out, from, x + w / 2, y + h / 2, 1.0f + 0.02f * std::sin(bump_ * 3.14159265f));
        }
        if (index >= int(top_) && index < int(top_ + kRowsShown)) {
            float y = g.list_y + g.row_h * float(index - int(top_)) + 2;
            if (ticked_ && glide_ < 1.0f) y = glide_from_ + (y - glide_from_) * ease_out(glide_);
            cursor(out, g.list_x + 4, y + 2, g.list_w - 8, g.row_h - 8);
        }
        if (top_ > 0) out.push_back(text(g.list_x + g.list_w / 2, g.list_y - 26, 22, kInkSoft, "^"));
        if (top_ + kRowsShown < rows.size()) out.push_back(text(g.list_x + g.list_w / 2, g.list_y + g.row_h * kRowsShown - 6, 22, kInkSoft, "v"));
        plate_label(out, g.back, false, "Close");
        if (!rows.empty()) plate_label(out, g.track, true, "Open");
    }

    // the section icons: plain shapes (a swirl, a map pin, a small chest, a lantern-ish note mark)
    static void icon(std::vector<Command>& out, int kind, float x, float y, float s, uint32_t colour) {
        const float cx = x + s / 2, cy = y + s / 2;
        switch (kind) {
        case 0: {  // swirl: a disc with a spiral cut
            out.push_back(circle(cx, cy, s * 0.48f, colour, true));
            float px = cx, py = cy;
            for (int i = 1; i <= 14; i++) {
                const float a = i * 0.55f, rr = s * 0.03f * i;
                const float nx = cx + std::cos(a) * rr, ny = cy + std::sin(a) * rr;
                out.push_back(line(px, py, nx, ny, kWhite, s * 0.09f));
                px = nx, py = ny;
            }
            break;
        }
        case 1:  // map pin
            out.push_back(circle(cx, y + s * 0.36f, s * 0.34f, colour, true));
            out.push_back(line(cx - s * 0.28f, y + s * 0.5f, cx, y + s, colour, s * 0.16f));
            out.push_back(line(cx + s * 0.28f, y + s * 0.5f, cx, y + s, colour, s * 0.16f));
            out.push_back(line(cx - s * 0.14f, y + s * 0.66f, cx + s * 0.14f, y + s * 0.66f, colour, s * 0.3f));
            out.push_back(circle(cx, y + s * 0.36f, s * 0.13f, kWhite, true));
            break;
        case 2:  // chest
            out.push_back(rect(x + s * 0.06f, y + s * 0.3f, s * 0.88f, s * 0.62f, 0x8A5A1EFF));
            out.push_back(rect(x + s * 0.06f, y + s * 0.18f, s * 0.88f, s * 0.24f, 0xA8742CFF));
            out.push_back(rect(x + s * 0.06f, y + s * 0.4f, s * 0.88f, s * 0.07f, colour));
            out.push_back(rect(x + s * 0.42f, y + s * 0.36f, s * 0.16f, s * 0.2f, 0xE8C34AFF));
            break;
        default:  // a note: a small page with lines
            out.push_back(rect(x + s * 0.15f, y + s * 0.05f, s * 0.7f, s * 0.9f, 0xFFF8E8FF));
            for (int i = 0; i < 3; i++) out.push_back(line(x + s * 0.25f, y + s * (0.3f + 0.2f * i), x + s * 0.75f, y + s * (0.3f + 0.2f * i), colour, 2));
            break;
        }
    }

    // the progress: one marker per step (done, current, upcoming) and "Step n of m"; with the
    // manifest's steps unknown only "Step n"
    void progress(std::vector<Command>& out, const Row& r, float x, float y) const {
        const Quest& q = *r.quest;
        const unsigned total = unsigned(q.steps.size()), now = r.status == Status::Done ? total : r.step;
        float px = x;
        for (unsigned i = 1; i <= total && i <= 12; i++) {
            const bool done = r.status == Status::Done || i < now, current = !done && i == now;
            out.push_back(circle(px + 9, y + 11, 8, done ? 0x3A8A2AFF : current ? 0xC0392BFF : 0xFFFFFFFF, true));
            out.push_back(circle(px + 9, y + 11, 8, done ? 0x2A6A1EFF : current ? 0x8A2A1EFF : 0xA89A7AFF, false, 2));
            if (done) {
                out.push_back(line(px + 5, y + 11, px + 8, y + 15, kWhite, 2.5f));
                out.push_back(line(px + 8, y + 15, px + 14, y + 7, kWhite, 2.5f));
            }
            if (i < total) out.push_back(line(px + 18, y + 11, px + 24, y + 11, 0xA89A7AFF, 2));
            px += 26;
        }
        std::string label;
        if (r.status == Status::Done) label = total ? "All " + std::to_string(total) + " steps done" : "Done";
        else if (r.status == Status::New || r.status == Status::NotAvailable || !now) label = "Not started";
        else label = "Step " + std::to_string(now) + (total ? " of " + std::to_string(total) : std::string());
        out.push_back(text(px + (total ? 6 : 0), y, 20, kInkSoft, bold(label)));
    }

    void framed_detail(std::vector<Command>& out, const Row& r) const {
        const Quest& q = *r.quest;
        if (!scene_.pieces) {  // the frame alone: the content on its parchment
            framed_detail_plain(out, r);
            return;
        }
        namespace d = art::detail;
        out.push_back(box("nb:sheet", frame_part(d::kSheet).x, frame_part(d::kSheet).y, frame_part(d::kSheet).w, frame_part(d::kSheet).h));
        const Box title = frame_part(d::kTitle), uline = frame_part(d::kUnderline);
        const std::string t = fit(q.title, 34, title.w, kBold);
        out.push_back(text(title.x, title.y + (title.h - 34) / 2, 34, kInk, bold(t)));
        out.push_back(box("nb:uline", uline.x, uline.y, uline.w, uline.h));
        // the sender, the progress and the step done before
        float pcx, pcy, pr;
        portrait_place(pcx, pcy, pr);
        if (q.portrait.empty()) {
            out.push_back(circle(pcx, pcy, pr, emblem_colour(q.mod), true));
            out.push_back(circle(pcx, pcy, pr - 4, kSeal, false, 1.5f));
            const std::string initial = q.mod_name.empty() ? "?" : q.mod_name.substr(0, 1);
            out.push_back(text(pcx - text_width(bold(initial), 22) / 2, pcy - 11, 22, kSeal, bold(initial)));
        }
        out.push_back(text(pcx + pr + 10, pcy - 11, 18, 0x2C6CA8FF, bold("From")));
        out.push_back(text(pcx + pr + 10 + text_width(bold("From"), 18) + 8, pcy - 12, 20, kInk, fit(q.mod_name, 20, uline.w - 2 * pr - 70)));
        // the progress at the right of the sender's line; the step done before under it
        {
            const unsigned total = unsigned(std::min<size_t>(q.steps.size(), 12));
            const float width = 26.0f * float(total) + 6 + text_width(bold("Step 99 of 99"), 20);
            progress(out, r, uline.x + uline.w - width, pcy - 11);
        }
        const unsigned now = r.step;
        if (r.status == Status::InProgress && now >= 2 && now - 2 < q.steps.size() && !q.steps[now - 2].empty())
            out.push_back(text(pcx - pr, pcy + pr + 4, 17, kInkLight, fit("Done: " + q.steps[now - 2], 17, uline.w)));
        // the three bands: what to do next, where, the reward
        // a finished quest: "Finished" with no Where
        const bool done = r.status == Status::Done;
        const char* heads[3] = {done ? "Finished" : "Next step", "Where", "Reward"};
        const uint32_t colours[3] = {0xC0392BFF, 0x1F6FB2FF, 0x3A8A2AFF};
        std::string bodies[3];
        if (done) bodies[0] = kFinished;
        else if (r.status == Status::NotAvailable) bodies[0] = "Not yet available: " + q.condition;
        else if (!q.hint.empty()) bodies[0] = q.hint + (q.next.empty() ? std::string() : " " + q.next);
        else bodies[0] = q.next.empty() ? std::string("(not written down yet)") : q.next;
        if (q.hint.rfind("Not yet possible", 0) == 0 && !q.next.empty()) bodies[0] = q.next + "\n" + q.hint;
        bodies[1] = where_text(r);
        bodies[2] = q.reward.empty() ? std::string("(none named)") : r.status == Status::Done ? q.reward + " (received)" : q.reward;
        for (int i = 0; i < 3; i++) {
            if (done && i == 1) continue;
            const Box b = frame_part(d::kBands[i]);
            icon(out, i == 0 ? 0 : i == 1 ? 1 : 2, b.x - 4, b.y - 28, 22, colours[i]);
            out.push_back(text(b.x + 24, b.y - 28, 21, colours[i], bold(heads[i])));
            out.push_back(box(i == 0 ? "nb:band0" : i == 1 ? "nb:band1" : "nb:band2", b.x, b.y, b.w, b.h));
            const unsigned lines = b.h > 50 ? 2 : 1;
            const std::vector<std::string> ls = wrap(bodies[i], 19, b.w - 36, lines);
            float ty = b.y + (b.h - 23 * float(ls.size())) / 2;
            for (const std::string& l : ls) {
                out.push_back(text(b.x + 18, ty, 19, kInk, l));
                ty += 23;
            }
        }
        // the header band (optional), the status pill, the chart card
        if (scene_.header) {
            const Box h = frame_part(d::kHeader);
            out.push_back(box("nb:header", h.x, h.y, h.w, h.h));
        }
        const Box card = frame_part(d::kCard);
        const float pw = 168, ph = 50, px = card.x + card.w - pw - 10, py = card.y - ph - 8;
        out.push_back(box("nb:pill", px, py, pw, ph));
        const char* word = r.status == Status::NotAvailable ? "Not yet" : status_text(r.status);
        const uint32_t tint = r.status == Status::New ? 0x2E8A2EC8 : r.status == Status::NotAvailable ? 0x7C8A6CD8 : r.status == Status::Done ? 0x9C3A2AD0 : 0;
        if (tint) out.push_back(rect(px + 3, py + 3, pw - 6, ph - 6, tint));
        out.push_back(text(px + (pw - text_width(bold(word), 20)) / 2, py + (ph - 20) / 2, 20, kWhite, bold(word)));
        if (r.tracked) {
            ring(out, px - 16, py + ph / 2, 11);
            out.push_back(text(px - 16 - 12 - text_width("On the chart", 17) - 6, py + ph / 2 - 9, 17, kInk, "On the chart"));
        }
        out.push_back(box("nb:card", card.x, card.y, card.w, card.h));
        card_chart(out, q, card);
        // the plates: Back (B) and Show on chart (A), the cursor on what A does
        const Geo g = geo();  // over the frame's own painted plates
        const Box bl = g.back, br = g.track;
        plate_piece(out, bl, false, "Back");
        if (trackable(r)) {
            plate_piece(out, br, true, r.tracked ? "Stop showing" : "Show on chart", true);
            cursor(out, br.x + 2, br.y + 2, br.w - 4, br.h - 4);
        }
        open_chart_plate(out, r);
    }
    // a painted plate (its own picture over the frame's, so it can be pressed) with its prompt
    void plate_piece(std::vector<Command>& out, const Box& b, bool a, const char* what, bool scroll = false) const {
        const size_t from = out.size();
        out.push_back(box("nb:uplate", b.x, b.y, b.w, b.h));
        const float size = 34, w = prompt_width(what, size) + (scroll ? 44 : 0);
        prompt(out, b.x + (b.w - w) / 2, b.y + (b.h - size) / 2 - 3, a, what, 1.0f, size);
        if (scroll) chart_scroll(out, b.x + (b.w + w) / 2 - 34, b.y + b.h / 2 - 17, 34, 30);
        press(out, from, b.x + b.w / 2, b.y + b.h / 2, press_scale(a ? press_a_ : press_b_));
    }
    // the mini chart inside the tilted card's blue sea
    void card_chart(std::vector<Command>& out, const Quest& q, const Box& card) const {
        auto at = [&](float u, float v, float& x, float& y) {  // a share of the sea (inset) to the page
            u = 0.08f + 0.84f * u, v = 0.08f + 0.84f * v;
            const float tx = art::card::kTL[0] + (art::card::kTR[0] - art::card::kTL[0]) * u, ty = art::card::kTL[1] + (art::card::kTR[1] - art::card::kTL[1]) * u;
            const float bx = art::card::kBL[0] + (art::card::kBR[0] - art::card::kBL[0]) * u, by = art::card::kBL[1] + (art::card::kBR[1] - art::card::kBL[1]) * u;
            x = card.x + (tx + (bx - tx) * v) * card.w, y = card.y + (ty + (by - ty) * v) * card.h;
        };
        for (int i = 0; i <= 7; i++) {  // the squares
            float x0, y0, x1, y1;
            at(i / 7.0f, 0, x0, y0), at(i / 7.0f, 1, x1, y1);
            out.push_back(line(x0, y0, x1, y1, 0xFFFFFF70, 1.2f));
            at(0, i / 7.0f, x0, y0), at(1, i / 7.0f, x1, y1);
            out.push_back(line(x0, y0, x1, y1, 0xFFFFFF70, 1.2f));
        }
        const float pulse = ticked_ ? 0.5f + 0.5f * std::sin(time_ * 6.2831853f * 0.8f) : 0.5f;
        float tu = 0, tv = 0, txp = 0, typ = 0;
        if (q.has_place) {
            chart_share(q.x, q.z, tu, tv);
            at(tu, tv, txp, typ);
            out.push_back(circle(txp, typ, 15 + 4 * pulse, 0xFFF4B0A0, true));  // the glow
            out.push_back(circle(txp, typ, 9, 0xFFFFFFFF, false, 3));
            out.push_back(circle(txp, typ, 4, 0xD12E1EFF, true));
        }
        if (scene_.link_known) {
            float lu, lv, lx, ly;
            chart_share(scene_.link_x, scene_.link_z, lu, lv);
            at(lu, lv, lx, ly);
            if (q.has_place) {  // the route: dots from Link to the place
                const float dx = txp - lx, dy = typ - ly, len = std::sqrt(dx * dx + dy * dy);
                const int dots = int(len / 9);
                for (int i = 1; i < dots; i++) out.push_back(circle(lx + dx * i / dots, ly + dy * i / dots, 2.2f, 0xD12E1EFF, true));
            }
            out.push_back(box("nb:linkface", lx - 16, ly - 16, 32, 32));
        }
        // the compass, near the card's lower right corner
        float cx, cy;
        at(0.86f, 0.84f, cx, cy);
        const float cr = 17;
        out.push_back(circle(cx, cy, cr, 0xF4E9C8D0, true));
        out.push_back(circle(cx, cy, cr, 0x5A7F9AFF, false, 1.5f));
        out.push_back(line(cx, cy - cr + 3, cx, cy + cr - 3, 0x5A7F9AFF, 2));
        out.push_back(line(cx - cr + 3, cy, cx + cr - 3, cy, 0x5A7F9AFF, 2));
        out.push_back(line(cx, cy - cr + 3, cx, cy - 2, 0xD12E1EFF, 3));
        out.push_back(text(cx - 5, cy - cr - 17, 15, 0x2C5C7CFF, bold("N")));
    }

    void framed_detail_plain(std::vector<Command>& out, const Row& r) const {
        const Quest& q = *r.quest;
        const Geo g = geo();
        const Box& c = g.content;
        const float lx = c.x + 18, lw = c.w - 18 - 330 - 24, rx = c.x + c.w - 18 - 312;
        float y = c.y + 8;
        out.push_back(text(lx, y, 34, kInk, bold(fit(q.title, 34, lw, kBold))));
        out.push_back(box("nb:underline", lx - 4, y + 40, std::min(lw, text_width(bold(fit(q.title, 34, lw, kBold)), 34) + 40), 16));
        // the sender: the quest's portrait (drawn by the caller) or the mod's seal
        float pcx, pcy, pr;
        portrait_place(pcx, pcy, pr);
        if (q.portrait.empty()) {
            out.push_back(circle(pcx, pcy, pr, emblem_colour(q.mod), true));
            out.push_back(circle(pcx, pcy, pr - 4, kSeal, false, 1.5f));
            const std::string initial = q.mod_name.empty() ? "?" : q.mod_name.substr(0, 1);
            out.push_back(text(pcx - text_width(bold(initial), 26) / 2, pcy - 13, 26, kSeal, bold(initial)));
        }
        out.push_back(text(pcx + pr + 12, pcy - 12, 19, 0x2C6CA8FF, bold("From")));
        out.push_back(text(pcx + pr + 12 + text_width(bold("From "), 19) + 6, pcy - 13, 21, kInk, fit(q.mod_name, 21, lw - 2 * pr - 80)));
        y = pcy + pr + 10;
        // the sections: a coloured header with its icon, the text on a soft band
        const float bottom = c.y + c.h - 8;
        auto section = [&](int icon_kind, uint32_t colour, uint32_t band, const char* head, const std::string& body, unsigned lines) {
            const std::vector<std::string> ls = wrap(body, 20, lw - 30, lines);
            const float need = 26 + 24 * float(ls.size()) + 14;
            if (y + need > bottom) return;
            icon(out, icon_kind, lx, y, 22, colour);
            out.push_back(text(lx + 30, y, 21, colour, bold(head)));
            y += 26;
            out.push_back(rect(lx + 10, y - 2, lw - 10, 24 * float(ls.size()) + 8, band));
            for (const std::string& l : ls) {
                out.push_back(text(lx + 22, y + 1, 20, kInk, l));
                y += 24;
            }
            y += 14;
        };
        if (r.status == Status::Done) {
            section(3, 0x7A5E3AFF, 0xEDE2C455, "Finished", kFinished, 1);
        } else if (r.status == Status::NotAvailable) {
            section(0, 0xC0661EFF, 0xF7D9A855, "Not yet available", q.condition, 2);
        } else {
            section(0, 0xC0392BFF, 0xF7E3B580, "Next step", q.next.empty() ? std::string("(not written down yet)") : q.next, 2);
            section(1, 0x1F6FB2FF, 0xD8E8EE90, "Where", where_text(r), 1);
        }
        if (!q.hint.empty() && r.status != Status::Done) section(0, 0xC0661EFF, 0xF7D9A870, "To continue", q.hint, 1);
        if (!q.reward.empty()) section(2, 0x3A8A2AFF, 0xDCEFCB90, "Reward", q.reward, 1);
        if (!q.known.empty()) section(3, 0x7A5E3AFF, 0xEDE2C470, "What is known so far", q.known, 3);
        // the status pill (the banner's own) and the chart ring
        const float pw = 176, ph = 54, px = c.x + c.w - 18 - pw, py = c.y + 6;
        out.push_back(box("nb:pill", px, py, pw, ph));
        const char* word = r.status == Status::NotAvailable ? "Not yet" : status_text(r.status);
        const uint32_t tint = r.status == Status::New ? 0x2E8A2EC8 : r.status == Status::NotAvailable ? 0x7C8A6CD8 : r.status == Status::Done ? 0x9C3A2AD0 : 0;
        if (tint) out.push_back(rect(px + 3, py + 3, pw - 6, ph - 6, tint));
        out.push_back(text(px + (pw - text_width(bold(word), 21)) / 2, py + (ph - 21) / 2, 21, kWhite, bold(word)));
        if (r.tracked) {
            ring(out, rx + 10, py + ph / 2, 11);
            out.push_back(text(rx + 26, py + ph / 2 - 10, 18, kInk, "On the chart"));
        }
        mini_chart(out, q, rx, py + ph + 10, 312, 300);
        // plates: Back (B) and Show on chart (A); the cursor is on what A does
        plate_label(out, g.back, false, "Back");
        if (trackable(r)) {
            plate_label(out, g.track, true, r.tracked ? "Stop showing" : "Show on chart", true);
            cursor(out, g.track.x + 4, g.track.y + 4, g.track.w - 8, g.track.h - 8);
        }
        open_chart_plate(out, r);
    }

    // The mini sea chart: the chart's 7 x 7 squares on a torn chart paper, the step's place glowing,
    // Link (the chart's own Link icon when the sea chart has been shown) and a dotted route between
    // them, a compass. Same mapping as the chart ring (story_chart.h).
    void mini_chart(std::vector<Command>& out, const Quest& q, float x, float y, float w, float h) const {
        out.push_back(box("nb:chart", x, y, w, h));
        const float side = std::min(w, h) - 44, gx = x + (w - side) / 2, gy = y + (h - side) / 2;
        out.push_back(rect(gx, gy, side, side, 0x3C8CC8E0));  // the sea
        for (int i = 0; i <= 7; i++) {
            const float t = side * float(i) / 7;
            out.push_back(line(gx + t, gy, gx + t, gy + side, 0xFFFFFF60, 1.2f));
            out.push_back(line(gx, gy + t, gx + side, gy + t, 0xFFFFFF60, 1.2f));
        }
        const float pulse = ticked_ ? 0.5f + 0.5f * std::sin(time_ * 6.2831853f * 0.8f) : 0.5f;
        float tu = 0, tv = 0;
        if (q.has_place) {
            chart_share(q.x, q.z, tu, tv);
            const float px = gx + tu * side, py = gy + tv * side;
            const int col = std::min(6, int(tu * 7)), row = std::min(6, int(tv * 7));
            out.push_back(rect(gx + side * col / 7 + 1, gy + side * row / 7 + 1, side / 7 - 2, side / 7 - 2, 0xFFF2B060));
            out.push_back(circle(px, py, 14 + 4 * pulse, 0xFFE8A0A0, true));  // the glow
            out.push_back(circle(px, py, 9, 0xFFFFFFFF, false, 3));
            out.push_back(circle(px, py, 4, 0xD12E1EFF, true));
        }
        if (scene_.link_known) {
            float lu, lv;
            chart_share(scene_.link_x, scene_.link_z, lu, lv);
            const float lx = gx + lu * side, ly = gy + lv * side;
            if (q.has_place) {  // the route: dots from Link to the place
                const float dx = gx + tu * side - lx, dy = gy + tv * side - ly, len = std::sqrt(dx * dx + dy * dy);
                const int dots = int(len / 9);
                for (int i = 1; i < dots; i++) out.push_back(circle(lx + dx * i / dots, ly + dy * i / dots, 2.2f, 0xD12E1EFF, true));
            }
            out.push_back(box("nb:linkface", lx - 15, ly - 15, 30, 30));
        }
        // the compass, at the corner
        const float cx = x + w - 34, cy = y + h - 34, cr = 18;
        out.push_back(circle(cx, cy, cr, 0xF4E9C8E0, true));
        out.push_back(circle(cx, cy, cr, 0x8A6A3AFF, false, 1.5f));
        out.push_back(line(cx, cy - cr + 3, cx, cy + cr - 3, 0x8A6A3AFF, 2));
        out.push_back(line(cx - cr + 3, cy, cx + cr - 3, cy, 0x8A6A3AFF, 2));
        out.push_back(line(cx, cy - cr + 3, cx, cy - 2, 0xD12E1EFF, 3));
        out.push_back(text(cx - 5, cy - cr - 18, 15, 0x8A6A3AFF, bold("N")));
        if (!q.where.empty()) out.push_back(text(x + 8, y + h + 2, 17, kInkSoft, fit(q.where, 17, w - 16)));
    }

    Mode mode_ = Mode::Closed;
    Scene scene_;
    int tab_ = 0;
    uint16_t selected_ = 0;
    unsigned top_ = 0, repeat_ = 0;
    uint32_t last_ = 0;
    bool touched_ = false;
    // animations (tick)
    bool ticked_ = false;
    float time_ = 0, opening_ = 1, turn_ = 1, press_a_ = 1, press_b_ = 1, bump_ = 1, glide_ = 1, glide_from_ = 0;
    int turn_dir_ = 0;
};

}  // namespace story_notebook

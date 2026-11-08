// The story notebook's detail view as layout panes (story_notebook_layout_detail.h).
#include "story_notebook_layout_detail.h"

#include <algorithm>
#include <map>

#include "ppc.h"
#include "runtime.h"
#include "story_lyt_screen.h"
#include "story_lyt_text.h"
#include "story_notebook_layout_view.h"
#include "story_notebook_page.h"

namespace story_notebook_layout::detail {
namespace {
namespace nb = story_notebook;

constexpr uint32_t kInks[] = {0x7D3B2EFF, 0x2E5E6EFF, 0x5B6B2EFF, 0x6A3F72FF, 0x8A5A1EFF, 0x34507EFF};  // page.h emblem_colour
constexpr unsigned kBodyLines[3] = {2, 1, 1};  // page.h framed_detail(): a band higher than 50 takes 2 lines
constexpr float kPromptIcon = 34, kPromptGap = 6, kScroll = 44;  // page.h prompt() / plate_piece()

std::map<std::string, std::u16string> g_texts;  // what each text pane holds (since built())

float pane_f(uint32_t pane, uint32_t off) { return pane ? u32_as_f32(ld32(pane + off)) : 0.0f; }
uint32_t pane(Cpu* c, const std::string& name) { return story_lyt_screen::pane(c, name.c_str()); }
void show(Cpu* c, const std::string& name, bool on) { story_lyt_screen::set_visible(pane(c, name), on); }

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = (unsigned char)s[i];
        const size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        uint32_t cp = n == 1 ? b : n == 2 ? b & 31 : n == 3 ? b & 15 : b & 7;
        for (size_t k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 63);
        i += n;
        if (cp >= 0x10000) out += char16_t(0xD800 + ((cp - 0x10000) >> 10)), out += char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF));
        else out += char16_t(cp);
    }
    return out;
}

// the pane's text (plain: the quest's texts are no markup), set only when it changes
void set_text(Cpu* c, const std::string& name, const std::u16string& s) {
    auto& now = g_texts[name];
    if (now == s) return;
    now = s;
    if (const uint32_t p = pane(c, name)) story_lyt_text::set_text(c, p, s);
}

// `text` cut with "..." to `width` (the pane's width when 0), measured with the pane's font
std::u16string fitted(Cpu* c, uint32_t p, const std::u16string& text, float width = 0) {
    if (width <= 0) width = pane_f(p, 0x3C);
    if (!p || width <= 0 || story_lyt_text::text_width(c, p, text) <= width) return text;
    std::u16string best;
    for (size_t i = 1; i <= text.size(); i++) {
        if (i < text.size() && text[i - 1] >= 0xD800 && text[i - 1] < 0xDC00) continue;  // not inside a surrogate pair
        if (story_lyt_text::text_width(c, p, text.substr(0, i) + u"...") > width) break;
        best = text.substr(0, i);
    }
    while (!best.empty() && best.back() == u' ') best.pop_back();
    return best + u"...";
}

// `text` broken into lines of the pane's width by the game's line breaker, at most `lines` (the last one
// cut with "..." when there is more, as page.h wrap())
std::u16string wrapped(Cpu* c, uint32_t p, const std::string& text, unsigned lines) {
    const float width = pane_f(p, 0x3C);
    if (!p || width <= 0) return utf16(text);
    const std::u16string all = story_lyt_text::wrap(c, p, utf16(text), width);
    std::u16string out;
    size_t start = 0;
    for (unsigned n = 0; n < lines; n++) {
        size_t end = all.find(u'\n', start);
        if (end == std::u16string::npos) return out + all.substr(start);
        if (n + 1 == lines) return out + fitted(c, p, all.substr(start, end - start) + u" ...........", width);
        out += all.substr(start, end - start + 1);
        start = end + 1;
    }
    return out;
}

std::string first_char(const std::string& s) { return s.empty() ? std::string("?") : s.substr(0, nb::char_bytes((unsigned char)s[0])); }

}  // namespace

DetailView view_of(const nb::Page&, const nb::Row& r) {
    const nb::Quest& q = *r.quest;
    DetailView v;
    v.title = q.title;
    v.sender = q.mod_name;
    v.initial = first_char(q.mod_name);
    const uint32_t ink = nb::emblem_colour(q.mod);
    for (int i = 0; i < int(std::size(kInks)); i++)
        if (kInks[i] == ink) v.seal = i;
    v.portrait = q.portrait.empty() ? 0 : q.key;
    // the progress (page.h progress()): one marker per step, "Step n of m"
    const unsigned total = unsigned(q.steps.size()), now = r.status == nb::Status::Done ? total : r.step;
    v.steps = int(std::min<unsigned>(total, kSteps));
    for (unsigned i = 1; i <= unsigned(v.steps); i++) {
        const bool done = r.status == nb::Status::Done || i < now;
        v.marker[i - 1] = done ? 2 : i == now ? 1 : 0;
    }
    if (r.status == nb::Status::Done) v.progress = total ? "All " + std::to_string(total) + " steps done" : "Done";
    else if (r.status == nb::Status::New || r.status == nb::Status::NotAvailable || !now) v.progress = "Not started";
    else v.progress = "Step " + std::to_string(now) + (total ? " of " + std::to_string(total) : std::string());
    if (r.status == nb::Status::InProgress && r.step >= 2 && r.step - 2u < q.steps.size() && !q.steps[r.step - 2u].empty())
        v.done_before = "Done: " + q.steps[r.step - 2u];
    // the bands (page.h framed_detail())
    if (r.status == nb::Status::Done) v.bodies[0] = nb::kFinished, v.head0 = "Finished", v.where = false;
    else if (r.status == nb::Status::NotAvailable) v.bodies[0] = "Not yet available: " + q.condition;
    else if (!q.hint.empty()) v.bodies[0] = q.hint + (q.next.empty() ? std::string() : " " + q.next);
    else v.bodies[0] = q.next.empty() ? std::string("(not written down yet)") : q.next;
    if (q.hint.rfind("Not yet possible", 0) == 0 && !q.next.empty()) v.bodies[0] = q.next + "\n" + q.hint;
    v.bodies[1] = nb::where_text(r);
    v.bodies[2] = q.reward.empty() ? std::string("(none named)") : r.status == nb::Status::Done ? q.reward + " (received)" : q.reward;
    v.status = r.status == nb::Status::NotAvailable ? "Not yet" : nb::status_text(r.status);
    v.pill = r.status == nb::Status::New ? 1 : r.status == nb::Status::NotAvailable ? 2 : r.status == nb::Status::Done ? 3 : 0;
    v.tracked = r.tracked;
    v.track_plate = nb::trackable(r);
    v.back_label = "Back";
    v.track_label = r.tracked ? "Stop showing" : "Show on chart";
    return v;
}

bool same(const DetailView& a, const DetailView& b) {
    if (a.title != b.title || a.sender != b.sender || a.initial != b.initial || a.progress != b.progress || a.done_before != b.done_before ||
        a.seal != b.seal || a.portrait != b.portrait || a.steps != b.steps || a.status != b.status || a.pill != b.pill || a.tracked != b.tracked ||
        a.track_plate != b.track_plate || a.back_label != b.back_label || a.track_label != b.track_label)
        return false;
    for (int i = 0; i < 3; i++)
        if (a.bodies[i] != b.bodies[i]) return false;
    if (a.head0 != b.head0 || a.where != b.where) return false;
    return std::equal(a.marker, a.marker + kSteps, b.marker);
}

void built(Cpu* c) {
    g_texts.clear();
    auto room = [&](const char* name, uint32_t chars) { story_lyt_text::set_capacity(c, pane(c, name), chars); };
    room("D_Title", 128), room("D_SenderName", 96), room("D_Progress", 32), room("D_DoneBefore", 160);
    room("D_Body0", 320), room("D_Body1", 320), room("D_Body2", 320);
    room("D_Status", 24), room("D_SenderInitial", 4), room("D_OnChart", 24);
}

void apply(Cpu* c, const DetailView& v) {
    set_text(c, "D_Title", fitted(c, pane(c, "D_Title"), utf16(v.title)));
    // the sender: the quest's portrait, else the mod's seal in its ink with its initial
    const bool portrait = v.portrait && portrait_into(5, v.portrait);
    show(c, "D_SenderPortrait", portrait);
    for (int i = 0; i < int(std::size(kInks)); i++) show(c, "D_Seal" + std::to_string(i), !portrait && i == v.seal);
    show(c, "D_SealRing", !portrait);
    show(c, "D_SenderInitial", !portrait);
    set_text(c, "D_SenderInitial", utf16(v.initial));
    set_text(c, "D_SenderName", fitted(c, pane(c, "D_SenderName"), utf16(v.sender)));
    // the progress: step i's marker in slot steps - i (slot 0 next to the label), linked to its right one
    set_text(c, "D_Progress", utf16(v.progress));
    for (int j = 0; j < kSteps; j++) {
        const std::string s = "D_Step" + std::to_string(j);
        const bool on = j < v.steps;
        show(c, s, on);
        if (!on) continue;
        const uint8_t m = v.marker[v.steps - 1 - j];
        show(c, s + "Done", m == 2);
        show(c, s + "Now", m == 1);
        show(c, s + "Todo", m == 0);
        if (j > 0) show(c, s + "Link", true);
    }
    show(c, "D_DoneBefore", !v.done_before.empty());
    set_text(c, "D_DoneBefore", fitted(c, pane(c, "D_DoneBefore"), utf16(v.done_before)));
    set_text(c, "D_Head0", utf16(v.head0));
    for (const char* p : {"D_Icon1", "D_Head1", "D_Band1", "D_Body1"}) show(c, p, v.where);
    for (int i = 0; i < 3; i++) {
        const std::string name = "D_Body" + std::to_string(i);
        set_text(c, name, wrapped(c, pane(c, name), v.bodies[i], kBodyLines[i]));
    }
    show(c, "D_PillNew", v.pill == 1);
    show(c, "D_PillNotYet", v.pill == 2);
    show(c, "D_PillDone", v.pill == 3);
    set_text(c, "D_Status", utf16(v.status));
    show(c, "D_Ring", v.tracked);
    show(c, "D_OnChart", v.tracked);
}

void place_prompts(Cpu* c, const std::string& back_label, const std::string& track_label, bool scroll) {
    // page.h plate_label() / plate_piece(): the button picture, 6 px, the label (and the scroll), centred
    // on the plate (the prompts are children of their plate's null pane, centred on it)
    auto place = [&](const char* icon, const char* label, const std::string& text, const char* extra) {
        const uint32_t ip = pane(c, icon), lp = pane(c, label);
        if (!ip || !lp) return;
        const std::u16string t = utf16(text);
        set_text(c, label, t);
        const float lw = story_lyt_text::text_width(c, lp, t);
        const float total = kPromptIcon + kPromptGap + lw + (extra ? kScroll : 0), left = -total / 2;
        story_lyt_screen::set_translate(ip, left + pane_f(ip, 0x3C) / 2, pane_f(ip, 0x20));
        story_lyt_screen::set_translate(lp, left + kPromptIcon + kPromptGap + pane_f(lp, 0x3C) / 2, pane_f(lp, 0x20));
        if (extra) {
            const uint32_t sp = pane(c, extra);
            // the scroll (34 wide, its rods 3 beyond) ends with the prompt
            story_lyt_screen::set_translate(sp, total / 2 + 3 - pane_f(sp, 0x3C) / 2, pane_f(sp, 0x20));
        }
    };
    place("T_BackIcon", "T_BackLabel", back_label, nullptr);
    place("T_TrackIcon", "T_TrackLabel", track_label, scroll ? "D_TrackScroll" : nullptr);
    show(c, "D_TrackScroll", scroll);
    for (const char* corner : {"D_TrackCursorTL", "D_TrackCursorTR", "D_TrackCursorBL", "D_TrackCursorBR"}) show(c, corner, scroll);
}

}  // namespace story_notebook_layout::detail

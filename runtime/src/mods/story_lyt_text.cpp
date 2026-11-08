// Story layouts: runtime text for the game's text panes (story_lyt_text.h). Local only (story mods are WIP).
#include "story_lyt_text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "guest_addr.h"
#include "ppc.h"
#include "runtime.h"

namespace story_lyt_text {
namespace {
// the game's HD text pane (decomp: FW-parts_text_valid, hd_parts_text_pane.cpp / hd_copy_text_pane.cpp)
const uint32_t kSetStringPrepared = GC(0x027046D4);  // hdTextPane_c::setStringPrepared(text, work = NULL)
const uint32_t kSetCapacity = GC(0x02703C04);        // hdTextPane_c::setCapacity(characters)
const uint32_t kGetCapacity = GC(0x02877834);        // the text box's buffer size in characters
const uint32_t kGetTextWidth = GC(0x02704820);       // hdTextPane_c::getTextWidth(be<u16>* text) -> f1
const uint32_t kTextBreakWrap = GC(0x02706A68);      // hdTextBreak_wrap(output, input, measure, options)
const uint32_t kTextPaneVtable = GD(0x1010BE80);     // hdTextPane_c's vtable (CopyTextPane_l::construct)
const uint32_t kWideBufferVtable = GD(0x1010BE68);   // wide buffered SafeString {text, vtable, capacity}
const uint32_t kWideStringVtable = GD(0x1010C0F8);   // wide SafeString {text, vtable} (the line breaker's)
const uint32_t kFindPaneSlot = 0x5C;                 // pane vtable: findPaneByName(name, recursive)
const uint32_t kPaneWidth = 0x3C;                    // pane size (f32 width, height)
const size_t kMaxText = 0x1FF;                       // setString converts into a 0x200-unit buffer
const size_t kBuffer = 0x800;                        // code units of our guest buffers

struct Guest {
    uint32_t text = 0, text2 = 0, measure_text = 0, string = 0, wrap_in = 0, wrap_out = 0, name = 0, measure = 0, measure_vtable = 0,
             options = 0;
};
Guest g;
uint32_t g_measure_pane = 0;
float g_measure_width = 0;

float measure(Cpu* c, uint32_t pane, uint32_t text) {
    guest_call(c, kGetTextWidth, {pane, text});
    return float(c->f[1].ps0);
}

void init() {
    if (g.text) return;
    g.text = mem::host_alloc(2 * kBuffer + 16, 4);
    g.text2 = mem::host_alloc(2 * kBuffer + 16, 4);
    g.measure_text = mem::host_alloc(2 * kBuffer + 32, 4);
    g.string = mem::host_alloc(16, 4);    // PaneString_l {text, vtable, capacity}
    g.wrap_in = mem::host_alloc(8, 4);    // {text, vtable}
    g.wrap_out = mem::host_alloc(16, 4);  // {text, vtable, capacity}
    g.name = mem::host_alloc(32, 4);
    g.options = mem::host_alloc(16, 4);
    // a line-breaker measure object of our own: {vtable}, vtable +0xC = measure(this, slice, count)
    g.measure_vtable = mem::host_alloc(16, 4);
    g.measure = mem::host_alloc(8, 4);
    st32(g.measure_vtable + 0xC, dispatch::register_host([](Cpu* c) {
        // does the line slice[0 .. count) overflow the width? (the breaker then breaks before it)
        const uint32_t text = ld32(c->r[4]);
        const uint32_t count = std::min<uint32_t>(c->r[5], kBuffer);
        for (uint32_t i = 0; i < count; i++) st16(g.measure_text + 2 * i, ld16(text + 2 * i));
        for (uint32_t i = count; i < count + 8; i++) st16(g.measure_text + 2 * i, 0);  // a cut tag reads zeros
        c->r[3] = measure(c, g_measure_pane, g.measure_text) > g_measure_width ? 1 : 0;
    }, "story_lyt_text measure"));
    st32(g.measure, g.measure_vtable);
}

}  // namespace
void state_loaded() { g = Guest{}; }
namespace {

void put(uint32_t buf, const std::u16string& s, size_t n) {
    for (size_t i = 0; i < n; i++) st16(buf + 2 * uint32_t(i), uint16_t(s[i]));
    st16(buf + 2 * uint32_t(n), 0);
}

std::u16string get(uint32_t buf, size_t max) {
    std::u16string s;
    for (size_t i = 0; i < max; i++) {
        const uint16_t ch = ld16(buf + 2 * uint32_t(i));
        if (!ch) break;
        s += char16_t(ch);
    }
    return s;
}

// at most `max` code units, never cutting a tag
size_t fit(const std::u16string& s, size_t max) {
    size_t n = 0;
    while (n < s.size()) {
        const size_t e = element_size(s, n);
        if (n + e > max) break;
        n += e;
    }
    return n;
}
}  // namespace

uint32_t find_pane(Cpu* c, uint32_t lyt, const char* name) {
    init();
    const uint32_t root = lyt ? ld32(lyt + 0xC) : 0;
    if (!root) return 0;
    mem::write_cstr(g.name, name, 32);
    const uint32_t find = ld32(ld32(root + 8) + kFindPaneSlot);
    return guest_call(c, find, {root, g.name, 1});
}

bool is_text_pane(uint32_t pane) { return pane && ld32(pane + 8) == kTextPaneVtable; }

uint32_t capacity(Cpu* c, uint32_t pane) { return is_text_pane(pane) ? guest_call(c, kGetCapacity, {pane}) : 0; }

bool set_capacity(Cpu* c, uint32_t pane, uint32_t chars) {
    if (!is_text_pane(pane) || !chars || chars > 0xFFFF) return false;
    guest_call(c, kSetCapacity, {pane, chars});
    return capacity(c, pane) >= chars;
}

float text_width(Cpu* c, uint32_t pane, const std::u16string& text) {
    init();
    if (!is_text_pane(pane)) return 0;
    put(g.text2, text, fit(text, kBuffer - 1));
    return measure(c, pane, g.text2);
}

std::u16string wrap(Cpu* c, uint32_t pane, const std::u16string& text, float width) {
    init();
    if (!is_text_pane(pane) || text.empty()) return text;
    const std::u16string tagged = text.substr(0, fit(text, kBuffer / 2));  // room for the inserted line breaks
    const std::u16string plain = strip_tags(tagged);
    put(g.text2, plain, plain.size());
    st32(g.wrap_in, g.text2);
    st32(g.wrap_in + 4, kWideStringVtable);
    st16(g.text, 0);
    st32(g.wrap_out, g.text);
    st32(g.wrap_out + 4, kWideBufferVtable);
    st32(g.wrap_out + 8, uint32_t(kBuffer));
    st32(g.options, 0);          // no line limit
    st8(g.options + 4, 1);       // skip the spaces at a break (as the game's own caller)
    st32(g.options + 8, 0);
    g_measure_pane = pane;
    g_measure_width = width;
    guest_call(c, kTextBreakWrap, {g.wrap_out, g.wrap_in, g.measure, g.options});
    g_measure_pane = 0;
    return restore_tags(tagged, get(g.text, kBuffer));
}

bool set_text(Cpu* c, uint32_t pane, const std::u16string& text, const Options& options) {
    init();
    if (!is_text_pane(pane)) return false;
    std::u16string s = text;
    if (options.wrap) {
        float width;
        if (options.width > 0) width = options.width;
        else { uint32_t bits = ld32(pane + kPaneWidth); memcpy(&width, &bits, 4); }
        s = wrap(c, pane, s, width);
    }
    const size_t n = fit(s, kMaxText);
    put(g.text, s, n);
    st32(g.string, g.text);  // PaneString_l {text, vtable, capacity}
    st32(g.string + 4, kWideBufferVtable);
    st32(g.string + 8, uint32_t(n + 1));
    guest_call(c, kSetStringPrepared, {pane, g.string, 0});
    return true;
}

bool set_text(Cpu* c, uint32_t pane, const std::string& utf8, const Options& options) {
    return set_text(c, pane, game_text(utf8), options);
}

void demo(Cpu* c, uint32_t lyt, uint32_t draw) {
    struct Pane { const char* name; uint32_t addr; };
    static Pane panes[] = {{"T_Count_00", 0}, {"T_Wrap_00", 0}, {"T_Clip_00", 0}, {"T_Fit_00", 0}, {"T_Title_00", 0}};
    if (draw == 0) {
        for (auto& p : panes) {
            p.addr = find_pane(c, lyt, p.name);
            LOG("[story-lyt-text] %s -> %08X%s, capacity %u", p.name, p.addr, is_text_pane(p.addr) ? " (text pane)" : "",
                capacity(c, p.addr));
        }
        // the wrapped pane gets room for its whole text; the clipped one keeps the game's automatic size
        const bool grown = set_capacity(c, panes[1].addr, 480);
        LOG("[story-lyt-text] T_Wrap_00 capacity -> %u (%s)", capacity(c, panes[1].addr), grown ? "ok" : "failed");
        Options w;
        w.wrap = true;
        set_text(c, panes[1].addr,
                 std::string("A long string set while the game runs, wrapped by the game's own line breaker at the "
                             "pane's width and measured with its font: {c:1}colour one{c}, {c:2}two{c}, {c:3}three{c}, "
                             "{c:4}four{c}. Accents \xC3\xA9\xC3\xA0\xC3\xBC\xC3\xB1 and a line\nbreak of our own."),
                 w);
        set_text(c, panes[2].addr,
                 std::string("Without wrapping, a string longer than the pane runs past its edge and is cut off at the "
                             "pane's capacity, the way the game's own panes handle text that is too long for them."));
        set_text(c, panes[3].addr, std::string("Scalable: this line is squeezed to fit the pane's width (min 50%)"));
        const std::u16string fixed = u"Runtime text in the game's font";
        LOG("[story-lyt-text] width of \"%s\": %.1f", to_utf8(fixed).c_str(), text_width(c, panes[4].addr, fixed));
    }
    if (draw % 30 == 0) {
        char buf[96];
        const unsigned n = draw / 30;
        snprintf(buf, sizeof buf, "Counter: {c:%u}%u{c}  (draw %u)", 1 + n % 8, n, draw);
        set_text(c, panes[0].addr, std::string(buf));
    }
}

}  // namespace story_lyt_text

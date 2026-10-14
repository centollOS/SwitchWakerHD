// The settings overlay's own text fields (text_entry.h, field()): a plain Dear ImGui field, or, where the
// platform registered its own keyboard (extensions.h, set_text_input), a read-only field that opens it.
// Kept apart from text_entry.cpp so that the offscreen tests of the Mods tab can link it without the
// prompt's game and host dependencies.
#include "text_entry.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "extensions.h"
#include "imgui.h"
#include "imgui_internal.h"  // ClearActiveID

namespace text_entry {
namespace {

std::u16string utf16(const char* s, const char* end) {
    std::u16string out;
    const unsigned char* p = (const unsigned char*)s;
    while (p && *p && (const char*)p < end) {
        uint32_t c = *p++;
        int rest = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80 && c < 0xC0) continue;  // malformed: skipped
        c &= rest == 3 ? 7 : rest == 2 ? 15 : rest == 1 ? 31 : 127;
        while (rest-- > 0 && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 63);
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xD800 + (c >> 10));
            out += char16_t(0xDC00 + (c & 0x3FF));
        } else {
            out += char16_t(c);
        }
    }
    return out;
}

// UTF-8 of `s`, whole characters only, at most `limit` bytes
std::string utf8(const std::u16string& s, size_t limit) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        char b[4];
        size_t n;
        if (c < 0x80) b[0] = char(c), n = 1;
        else if (c < 0x800) b[0] = char(0xC0 | c >> 6), b[1] = char(0x80 | (c & 63)), n = 2;
        else if (c < 0x10000) b[0] = char(0xE0 | c >> 12), b[1] = char(0x80 | (c >> 6 & 63)), b[2] = char(0x80 | (c & 63)), n = 3;
        else b[0] = char(0xF0 | c >> 18), b[1] = char(0x80 | (c >> 12 & 63)), b[2] = char(0x80 | (c >> 6 & 63)), b[3] = char(0x80 | (c & 63)), n = 4;
        if (out.size() + n > limit) break;
        out.append(b, n);
    }
    return out;
}

// the answer of the platform keyboard for the field that opened it (one at a time)
struct Pending {
    std::mutex mu;
    uint64_t serial = 0;  // per keyboard shown: a late answer of an older one is dropped
    ImGuiID id = 0;
    bool ready = false, ok = false;
    std::u16string text;
};
Pending g_pending;
std::atomic<bool> g_shown{false};

}  // namespace

bool field_active() { return g_shown.load(std::memory_order_relaxed); }

bool field(const char* label, char* buf, size_t size, int flags, const char* hint) {
    const overlay::ext::TextInput& keyboard = overlay::ext::text_input();
    if (!keyboard)
        return hint ? ImGui::InputTextWithHint(label, hint, buf, size, flags) : ImGui::InputText(label, buf, size, flags);
    const ImGuiID id = ImGui::GetID(label);
    bool entered = false;
    {
        std::lock_guard<std::mutex> lk(g_pending.mu);
        if (g_pending.ready && g_pending.id == id) {
            g_pending.ready = false;
            if (g_pending.ok && size) {
                const std::string text = utf8(g_pending.text, size - 1);
                memcpy(buf, text.c_str(), text.size() + 1);
                entered = true;
            }
        }
    }
    const int shown_flags = (flags & ~ImGuiInputTextFlags_EnterReturnsTrue) | ImGuiInputTextFlags_ReadOnly;
    if (hint) ImGui::InputTextWithHint(label, hint, buf, size, shown_flags);
    else ImGui::InputText(label, buf, size, shown_flags);
    if (ImGui::IsItemActivated() && !field_active()) {
        Request r;
        r.initial = utf16(buf, buf + strnlen(buf, size));
        // the platform keyboard's guide text: the hint, or the label without its "##id" part
        const char* guide = hint ? hint : label;
        const char* guide_end = hint ? guide + strlen(guide) : ImGui::FindRenderedTextEnd(label);
        r.hint = utf16(guide, guide_end);
        r.max_len = size ? int(size) - 1 : 0;
        r.mode = 0;
        r.language = 1;
        uint64_t serial;
        {
            std::lock_guard<std::mutex> lk(g_pending.mu);
            serial = ++g_pending.serial;
            g_pending.id = id;
            g_pending.ready = false;
        }
        g_shown = true;
        ImGui::ClearActiveID();  // the read-only field does not keep the focus for typing
        keyboard(r, [serial](bool ok, std::u16string text) {
            std::lock_guard<std::mutex> lk(g_pending.mu);
            if (serial != g_pending.serial) return;
            g_pending.ok = ok;
            g_pending.text = std::move(text);
            g_pending.ready = true;
            g_shown = false;
        });
    }
    return entered;
}

}  // namespace text_entry

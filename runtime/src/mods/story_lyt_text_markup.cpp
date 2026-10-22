// Story layouts: UTF-8 markup -> the game's pane text (story_lyt_text_markup.h).
#include "story_lyt_text_markup.h"

#include <algorithm>

namespace story_lyt_text {

std::u16string colour_tag(uint16_t number) {
    return std::u16string{kTagBegin, 0, 3, 2, char16_t(number)};
}

static void put(std::u16string& out, uint32_t cp) {
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out += char16_t(0xD800 + (cp >> 10));
        out += char16_t(0xDC00 + (cp & 0x3FF));
    } else {
        out += char16_t(cp);
    }
}

std::u16string game_text(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = s[i];
        if (b == '{') {
            if (i + 1 < s.size() && s[i + 1] == '{') { out += u'{'; i += 2; continue; }
            const size_t close = s.find('}', i);
            const std::string tag = close == std::string::npos ? std::string() : s.substr(i + 1, close - i - 1);
            if (tag == "c") { out += colour_tag(0); i = close + 1; continue; }
            if (tag.size() > 2 && tag.compare(0, 2, "c:") == 0 && tag.find_first_not_of("0123456789", 2) == std::string::npos &&
                tag.size() <= 6) {
                out += colour_tag(uint16_t(std::stoul(tag.substr(2))));
                i = close + 1;
                continue;
            }
            out += u'{';
            ++i;
            continue;
        }
        if (b < 0x80) { out += char16_t(b == '\r' ? kNewLine : b); i += (b == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ? 2 : 1; continue; }
        int n = b >= 0xF0 && b < 0xF8 ? 3 : b >= 0xE0 ? 2 : b >= 0xC2 && b < 0xE0 ? 1 : -1;
        uint32_t cp = n == 3 ? b & 7 : n == 2 ? b & 15 : b & 31;
        bool ok = n > 0;
        for (int k = 1; ok && k <= n; k++) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
            else cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (ok && ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000))) ok = false;
        if (!ok) { out += char16_t(0xFFFD); ++i; continue; }
        put(out, cp);
        i += n + 1;
    }
    return out;
}

size_t element_size(const std::u16string& s, size_t i) {
    if (s[i] == kTagBegin && i + 3 < s.size()) return std::min(s.size() - i, size_t(4 + s[i + 3] / 2));
    if (s[i] == kTagEnd) return std::min(s.size() - i, size_t(3));
    return 1;
}

std::u16string strip_tags(const std::u16string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = element_size(s, i);
        if (s[i] != kTagBegin && s[i] != kTagEnd) out += s[i];
        i += n;
    }
    return out;
}

std::u16string restore_tags(const std::u16string& tagged, const std::u16string& wrapped) {
    std::u16string out;
    size_t j = 0;
    for (size_t i = 0; i < tagged.size();) {
        const size_t n = element_size(tagged, i);
        const char16_t ch = tagged[i];
        if (ch == kTagBegin || ch == kTagEnd) {
            out.append(tagged, i, n);
            i += n;
            continue;
        }
        while (j < wrapped.size() && wrapped[j] == kNewLine && ch != kNewLine) out += wrapped[j++];  // inserted break
        if (j < wrapped.size() && wrapped[j] == ch) out += wrapped[j++];
        // else: a space the breaker dropped at a line break
        i += n;
    }
    out.append(wrapped, std::min(j, wrapped.size()), std::u16string::npos);
    return out;
}

std::string to_utf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 63));
        else if (cp < 0x10000) out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 63)), out += char(0x80 | (cp & 63));
        else out += char(0xF0 | cp >> 18), out += char(0x80 | (cp >> 12 & 63)), out += char(0x80 | (cp >> 6 & 63)), out += char(0x80 | (cp & 63));
    }
    return out;
}

}  // namespace story_lyt_text

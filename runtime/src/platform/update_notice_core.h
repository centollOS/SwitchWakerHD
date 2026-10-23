// SwitchWakerHD (this fork): the pure part of the update notice (update_notice_switch.h): versions as
// build::version() and the release tags write them, the HTTP answer and the release's tag in GitHub's JSON.
// No Switch headers, so tools/switch/update_notice_host_test.cpp checks it on a computer.
#pragma once
#include <cctype>
#include <cstdlib>
#include <string>

namespace update_notice {

// "v0.4.7" (a release), "v0.4.7+55" (55 commits later), "0.4.7"; `after` is the +N part
struct Version {
    int major = 0, minor = 0, patch = 0, after = 0;
};

// false for "dev", a hash or anything else that does not start with [v]X.Y.Z: such a build never checks
inline bool parse_version(const std::string& text, Version* out) {
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) i++;
    int parts[3] = {};
    for (int k = 0; k < 3; k++) {
        if (i >= text.size() || !isdigit((unsigned char)text[i])) return false;
        long v = 0;
        while (i < text.size() && isdigit((unsigned char)text[i])) {
            v = v * 10 + (text[i++] - '0');
            if (v > 1000000) return false;
        }
        parts[k] = int(v);
        if (k < 2) {
            if (i >= text.size() || text[i] != '.') return false;
            i++;
        }
    }
    Version r{parts[0], parts[1], parts[2], 0};
    if (i < text.size() && text[i] == '+') {
        r.after = atoi(text.c_str() + i + 1);
    } else if (i != text.size() && text[i] != '-') {
        return false;
    }
    *out = r;
    return true;
}

// the release is newer than the running build: a higher X.Y.Z (a build past a tag is not older than it)
inline bool is_newer(const Version& release, const Version& running) {
    if (release.major != running.major) return release.major > running.major;
    if (release.minor != running.minor) return release.minor > running.minor;
    return release.patch > running.patch;
}

// An HTTP/1.x answer: its status and its body (a chunked body put together). False when it is not one.
inline bool parse_http(const std::string& raw, int* status, std::string* body) {
    if (raw.compare(0, 5, "HTTP/") != 0) return false;
    const size_t sp = raw.find(' ');
    const size_t end = raw.find("\r\n\r\n");
    if (sp == std::string::npos || end == std::string::npos || sp > end) return false;
    *status = atoi(raw.c_str() + sp + 1);
    std::string head = raw.substr(0, end);
    for (char& c : head) c = char(tolower((unsigned char)c));
    std::string data = raw.substr(end + 4);
    if (head.find("\r\ntransfer-encoding: chunked") == std::string::npos) {
        *body = std::move(data);
        return true;
    }
    std::string out;
    size_t p = 0;
    for (;;) {
        const size_t nl = data.find("\r\n", p);
        if (nl == std::string::npos) return false;
        const unsigned long n = strtoul(data.substr(p, nl - p).c_str(), nullptr, 16);
        p = nl + 2;
        if (n == 0) break;
        if (p + n > data.size()) return false;
        out.append(data, p, n);
        p += n + 2;  // the chunk's CRLF
    }
    *body = std::move(out);
    return true;
}

// The string value of a top-level-looking "key" in JSON (escapes kept simple: \" \\ \/ and \uXXXX as '?'),
// "" when absent or null. The release fields read here (tag_name, name) are plain text.
inline std::string json_string(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p >= json.size() || json[p] != ':') return "";
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p >= json.size() || json[p] != '"') return "";
    std::string out;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] != '\\' || p + 1 >= json.size()) {
            out += json[p];
            continue;
        }
        const char e = json[++p];
        if (e == 'u') {
            out += '?';
            p += 4;
        } else {
            out += e == 'n' ? ' ' : e == 't' ? ' ' : e;
        }
    }
    return p < json.size() ? out : "";
}

}  // namespace update_notice

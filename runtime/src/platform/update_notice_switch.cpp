// SwitchWakerHD (this fork): the update notice (update_notice_switch.h).
#include "update_notice_switch.h"

#include <switch.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../build_info.h"
#include "../overlay/hostui.h"
#include "../runtime.h"
#include "debug_server.h"
#include "ui_text_switch.h"
#include "update_notice_core.h"

namespace update_notice {
namespace {

constexpr const char* kHost = "api.github.com";
constexpr const char* kPath = "/repos/centollOS/SwitchWakerHD/releases/latest";
constexpr size_t kMaxAnswer = 1 << 20;  // a release's JSON is a few KiB; its notes can make it more

std::mutex g_mu;
Info g_info;
bool g_running = false;  // a check thread is out
double g_shownAt = -1.0;  // when the notice first showed (now_s), -1 not yet

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void publish(State s, const std::string& detail = {}) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_info.state = s;
    g_info.detail = detail;
}

// the socket service: the debug server's when it runs, else initialised here once (and kept: the process ends it)
bool sockets_up() {
    static int state = 0;  // 0 not tried, 1 up, -1 failed
    if (state == 0) {
        if (debugsrv::running()) {
            state = 1;
        } else {
            const Result rc = socketInitializeDefault();
            state = R_SUCCEEDED(rc) ? 1 : -1;
            if (R_FAILED(rc)) LOG("[update] socket service unavailable (rc 0x%x)", rc);
        }
    }
    return state == 1;
}

// GET https://kHost kPath: the raw answer (headers and body) into `out`; false with `why` on any failure.
// HTTP/1.0 with Connection: close, so the body ends with the connection.
bool https_get(std::string& out, std::string& why) {
    if (!sockets_up()) {
        why = "no socket service";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(kHost, "443", &hints, &res) != 0 || !res) {
        why = "cannot resolve api.github.com (offline?)";
        return false;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    const bool connected = fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!connected) {
        if (fd >= 0) close(fd);
        why = "cannot connect to api.github.com";
        return false;
    }
    Result rc = sslInitialize(1);
    if (R_FAILED(rc)) {
        close(fd);
        why = "ssl service unavailable (rc " + std::to_string(rc) + ")";
        return false;
    }
    SslContext ctx{};
    SslConnection conn{};
    bool ok = false;
    if (R_FAILED(rc = sslCreateContext(&ctx, SslVersion_Auto))) {
        why = "sslCreateContext failed";
    } else {
        if (R_FAILED(rc = sslContextCreateConnection(&ctx, &conn))) {
            why = "sslContextCreateConnection failed";
        } else {
            // the socket moves to the ssl service (fd is closed by the wrapper; a returned one is ours to close)
            const int kept = socketSslConnectionSetSocketDescriptor(&conn, fd);
            fd = -1;
            if (kept < 0 && errno != ENOENT) {
                why = "cannot hand the socket to the ssl service";
            } else if (R_FAILED(rc = sslConnectionSetHostName(&conn, kHost, strlen(kHost) + 1))) {
                why = "sslConnectionSetHostName failed";
            } else if (R_FAILED(rc = sslConnectionSetVerifyOption(&conn, SslVerifyOption_PeerCa | SslVerifyOption_HostName))) {
                why = "sslConnectionSetVerifyOption failed";
            } else if (R_FAILED(rc = sslConnectionDoHandshake(&conn, nullptr, nullptr, nullptr, 0))) {
                why = "TLS handshake failed (rc 0x" + [&] {
                    char b[16];
                    snprintf(b, sizeof b, "%x", rc);
                    return std::string(b);
                }() + ")";
            } else {
                const std::string req = std::string("GET ") + kPath + " HTTP/1.0\r\nHost: " + kHost +
                                        "\r\nUser-Agent: SwitchWakerHD/" + build::version() +
                                        "\r\nAccept: application/vnd.github+json\r\nConnection: close\r\n\r\n";
                u32 n = 0;
                if (R_FAILED(rc = sslConnectionWrite(&conn, req.data(), u32(req.size()), &n)) || n != req.size()) {
                    why = "cannot send the request";
                } else {
                    char buf[4096];
                    ok = true;
                    for (;;) {
                        n = 0;
                        rc = sslConnectionRead(&conn, buf, sizeof buf, &n);
                        if (R_FAILED(rc) || n == 0) break;  // the end of the answer (Connection: close)
                        out.append(buf, n);
                        if (out.size() > kMaxAnswer) {
                            ok = false;
                            why = "answer too long";
                            break;
                        }
                    }
                }
            }
            if (kept >= 0) close(kept);
            sslConnectionClose(&conn);
        }
        sslContextClose(&ctx);
    }
    if (fd >= 0) close(fd);
    sslExit();
    return ok;
}

void* check_thread(void*) {
    std::string current = build::version();
    if (const char* e = getenv("WWHD_UPDATE_VERSION"); e && *e) current = e;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.current = current;
    }
    Version cur;
    if (!parse_version(current, &cur)) {
        LOG("[update] version %s is a development build: no check", current.c_str());
        publish(State::kDevBuild);
    } else {
        publish(State::kChecking);
        std::string raw, why, body;
        int status = 0;
        Version latest;
        if (!https_get(raw, why)) {
            LOG("[update] check failed: %s", why.c_str());
            publish(State::kFailed, why);
        } else if (!parse_http(raw, &status, &body) || status != 200) {
            why = status ? "GitHub answered HTTP " + std::to_string(status) : "not an HTTP answer";
            LOG("[update] check failed: %s", why.c_str());
            publish(State::kFailed, why);
        } else {
            const std::string tag = json_string(body, "tag_name");
            if (!parse_version(tag, &latest)) {
                LOG("[update] check failed: no release tag in the answer");
                publish(State::kFailed, "no release tag in the answer");
            } else {
                const bool newer = is_newer(latest, cur);
                LOG("[update] latest release %s, this build %s: %s", tag.c_str(), current.c_str(),
                    newer ? "newer" : "up to date");
                std::lock_guard<std::mutex> lk(g_mu);
                g_info.latest = tag;
                g_info.title = json_string(body, "name");
                g_info.state = newer ? State::kNewer : State::kUpToDate;
                g_info.detail.clear();
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_running = false;
    return nullptr;
}

void run_check() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_running) return;
        g_running = true;
        g_info.state = State::kChecking;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 << 10);  // Horizon commits a small default stack
    pthread_t t;
    const int err = pthread_create(&t, &attr, check_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        LOG("[update] cannot start the check thread (%d)", err);
        std::lock_guard<std::mutex> lk(g_mu);
        g_running = false;
        g_info.state = State::kFailed;
        g_info.detail = "no thread";
        return;
    }
    pthread_detach(t);
}

}  // namespace

bool check_enabled() {
    std::string v;
    return !hostui::get(kKeyUpdateCheck, v) || v != "0";
}

void set_check_enabled(bool on) {
    hostui::post([on] { hostui::set(kKeyUpdateCheck, on ? "1" : "0"); });
    if (on) {
        run_check();
    } else {
        publish(State::kOff);
    }
}

void start() {
    if (!check_enabled()) {
        LOG("[update] check at start off (Dev tab, %s=0)", kKeyUpdateCheck);
        publish(State::kOff);
        return;
    }
    run_check();
}

Info info() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_info;
}

bool notice(std::string& line) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_info.state != State::kNewer) return false;
    const double now = now_s();
    if (g_shownAt < 0) g_shownAt = now;
    if (now - g_shownAt > kShowSeconds) return false;
    char text[256];
    snprintf(text, sizeof text, ui_text::menu().updateNotice, g_info.latest.c_str(), g_info.current.c_str());
    line = text;
    return true;
}

}  // namespace update_notice

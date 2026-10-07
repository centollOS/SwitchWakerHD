// Cemuhook (DSU) motion client (see dsu.h). Protocol: https://v1993.github.io/cemuhook-protocol/
#include "dsu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kNoSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kNoSocket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

void log_msg(const char* fmt, ...);

namespace dsu {
namespace {

void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8); }
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i)); }
void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) { for (int i = 0; i < 8; i++) v[at + i] = (uint8_t)(x >> (8 * i)); }
void putf(std::vector<uint8_t>& v, size_t at, float f) { uint32_t x; memcpy(&x, &f, 4); put32(v, at, x); }
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
float getf(const uint8_t* p) { uint32_t x = get32(p); float f; memcpy(&f, &x, 4); return f; }

// header + message type; the length field counts everything after the 16-byte header
std::vector<uint8_t> message(const char magic[4], uint32_t id, uint32_t type, size_t size) {
    std::vector<uint8_t> v(size, 0);
    memcpy(v.data(), magic, 4);
    put16(v, 4, kProtocolVersion);
    put16(v, 6, (uint16_t)(size - kHeaderSize));
    put32(v, 12, id);
    put32(v, 16, type);
    return v;
}
void finish(std::vector<uint8_t>& v) {
    put32(v, 8, 0);
    put32(v, 8, crc32(v.data(), v.size()));
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> encode_port_info_request(uint32_t client_id, const std::vector<uint8_t>& slots) {
    size_t n = std::min<size_t>(slots.size(), 4);
    auto v = message("DSUC", client_id, kPortInfo, 20 + 4 + n);
    put32(v, 20, (uint32_t)n);
    for (size_t i = 0; i < n; i++) v[24 + i] = slots[i];
    finish(v);
    return v;
}

std::vector<uint8_t> encode_pad_data_request(uint32_t client_id, uint8_t slot) {
    auto v = message("DSUC", client_id, kPadData, 20 + 8);
    v[20] = 1;  // register by slot
    v[21] = slot;
    finish(v);
    return v;
}

uint32_t message_type(const uint8_t* p, size_t size) {
    if (size < 20 || memcmp(p, "DSUS", 4) != 0) return 0;
    if (get16(p + 4) > kProtocolVersion) return 0;
    if ((size_t)get16(p + 6) + kHeaderSize > size) return 0;  // truncated
    size_t len = get16(p + 6) + kHeaderSize;
    std::vector<uint8_t> copy(p, p + len);
    memset(copy.data() + 8, 0, 4);
    if (crc32(copy.data(), len) != get32(p + 8)) return 0;
    return get32(p + 16);
}

bool parse_pad_data(const uint8_t* p, size_t size, PadData& d) {
    if (message_type(p, size) != kPadData || (size_t)get16(p + 6) + kHeaderSize < kPadDataSize) return false;
    d.slot = p[20];
    d.state = p[21];
    d.model = p[22];
    d.connected = p[31] != 0;
    d.packet = get32(p + 32);
    // 36: buttons, PS, touch button, 4 sticks, 12 analog buttons, 2 touches (6 bytes each)
    d.timestamp_us = get64(p + 68);
    for (int i = 0; i < 3; i++) d.accel[i] = getf(p + 76 + 4 * i);
    for (int i = 0; i < 3; i++) d.gyro[i] = getf(p + 88 + 4 * i);
    return true;
}

std::vector<uint8_t> encode_pad_data(uint32_t server_id, const PadData& d) {
    auto v = message("DSUS", server_id, kPadData, kPadDataSize);
    v[20] = d.slot;
    v[21] = d.state;
    v[22] = d.model;
    v[23] = 2;     // bluetooth
    v[30] = 0x05;  // battery full
    v[31] = d.connected ? 1 : 0;
    put32(v, 32, d.packet);
    v[40] = v[41] = v[42] = v[43] = 128;  // sticks centred
    put64(v, 68, d.timestamp_us);
    for (int i = 0; i < 3; i++) putf(v, 76 + 4 * i, d.accel[i]);
    for (int i = 0; i < 3; i++) putf(v, 88 + 4 * i, d.gyro[i]);
    finish(v);
    return v;
}

// ---- client thread ----

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Client::start(const std::string& host, uint16_t port, uint8_t slot) {
    stop();
    quit_ = false;
    {
        std::lock_guard lk(mu_);
        status_ = "connecting to " + host + ":" + std::to_string(port);
    }
    thread_ = std::thread(&Client::run, this, host, port, slot);
}

void Client::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    last_data_ms_ = 0;
    std::lock_guard lk(mu_);
    status_ = "off";
}

bool Client::receiving() const { return now_ms() - last_data_ms_.load() < 1000; }

std::string Client::status() const {
    std::lock_guard lk(mu_);
    return receiving() ? "receiving motion" : status_;
}

void Client::run(std::string host, uint16_t port, uint8_t slot) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::lock_guard lk(mu_);
        status_ = "network unavailable";
        return;
    }
#endif
    auto set_status = [&](const std::string& s) {
        std::lock_guard lk(mu_);
        if (s != status_) log_msg("[gyro] Cemuhook %s:%u: %s", host.c_str(), (unsigned)port, s.c_str());
        status_ = s;
    };
    const uint32_t id = (uint32_t)now_ms() ^ 0x57574844u;  // any value; servers tell clients apart by it
    socket_t s = kNoSocket;
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int64_t next_resolve = 0, next_request = 0;
    uint32_t last_packet = 0;
    bool any = false;
    while (!quit_) {
        int64_t t = now_ms();
        if (s == kNoSocket) {
            if (t < next_resolve) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            next_resolve = t + 3000;
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            const std::string service = std::to_string(port);
            if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
                set_status("cannot resolve " + host);
                continue;
            }
            memcpy(&addr, res->ai_addr, res->ai_addrlen);
            addr_len = (socklen_t)res->ai_addrlen;
            s = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
            freeaddrinfo(res);
            if (s == kNoSocket) {
#ifdef __ANDROID__
                // the Android app does not ask for the INTERNET permission (docs/gyro.md): no sockets
                if (errno == EACCES || errno == EPERM) {
                    set_status("unavailable: the Android app has no network permission");
                    next_resolve = t + 60000;
                    continue;
                }
#endif
                set_status("no socket");
                continue;
            }
#ifdef _WIN32
            DWORD tv = 100;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
#else
            timeval tv{0, 100000};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
            next_request = 0;
            set_status("waiting for the server");
        }
        if (t >= next_request) {
            next_request = t + 1000;
            auto info = encode_port_info_request(id, {slot});
            auto data = encode_pad_data_request(id, slot);
            sendto(s, (const char*)info.data(), (int)info.size(), 0, (const sockaddr*)&addr, addr_len);
            sendto(s, (const char*)data.data(), (int)data.size(), 0, (const sockaddr*)&addr, addr_len);
            if (any && t - last_data_ms_.load() > 3000) set_status("no data from the server (is it running?)");
        }
        uint8_t buf[512];
        int n = (int)recv(s, (char*)buf, sizeof buf, 0);
        if (n <= 0) continue;  // timeout (or ICMP "port unreachable" on some systems): ask again later
        PadData d;
        if (!parse_pad_data(buf, (size_t)n, d) || d.slot != slot) continue;
        if (!d.connected || d.state != 2) { set_status("slot " + std::to_string(slot) + ": no controller"); continue; }
        if (any && d.packet == last_packet) continue;  // duplicate
        any = true;
        last_packet = d.packet;
        last_data_ms_ = now_ms();
        set_status("receiving motion");
        sink_(d);
    }
    if (s != kNoSocket) close_socket(s);
#ifdef _WIN32
    WSACleanup();
#endif
}

}  // namespace dsu

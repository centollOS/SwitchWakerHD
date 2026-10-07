// Cemuhook (DSU) motion client: the UDP protocol that DS4Windows, BetterJoy, SteamDeckGyroDSU, phone
// apps and others serve motion data with (https://v1993.github.io/cemuhook-protocol/).
//
// The packet code (encode_*, parse_*) is plain C++ without sockets, so it is unit-tested
// (runtime/tools/motion_test.cpp). Client runs one background thread with a short receive timeout:
// it asks the server for one slot's data about once a second (servers drop a client that stays quiet
// for a few seconds), takes every data packet and hands its motion to a callback. Nothing here ever
// blocks the game: the callback only stores the sample.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dsu {

constexpr uint16_t kProtocolVersion = 1001;
constexpr uint16_t kDefaultPort = 26760;
enum MessageType : uint32_t { kVersion = 0x100000, kPortInfo = 0x100001, kPadData = 0x100002 };
constexpr size_t kHeaderSize = 16;       // magic, version, length, crc32, sender id
constexpr size_t kPortInfoSize = 32;     // header + type + 11 bytes of slot info + 1
constexpr size_t kPadDataSize = 100;     // header + type + slot info + buttons, sticks, touch, motion

uint32_t crc32(const uint8_t* data, size_t size);

// client -> server
std::vector<uint8_t> encode_port_info_request(uint32_t client_id, const std::vector<uint8_t>& slots);
// registration for one slot's pad data (flag 1 = by slot)
std::vector<uint8_t> encode_pad_data_request(uint32_t client_id, uint8_t slot);

// server -> client: one motion sample of a pad data message
struct PadData {
    uint8_t slot = 0;
    uint8_t state = 0;           // 0 disconnected, 1 reserved, 2 connected
    uint8_t model = 0;           // 0 n/a, 1 no or partial gyro, 2 full gyro
    bool connected = false;
    uint32_t packet = 0;         // packet counter
    uint64_t timestamp_us = 0;   // motion timestamp, microseconds (0: the server sends none)
    float accel[3] = {};         // g, DS4 frame: x right, y down... as servers send it (see motion.h)
    float gyro[3] = {};          // degrees per second: pitch, yaw, roll
};
// Validates magic ("DSUS"), length and CRC; false for anything else (other messages, junk).
bool parse_pad_data(const uint8_t* data, size_t size, PadData& out);
// The message type of a valid server packet, 0 if invalid.
uint32_t message_type(const uint8_t* data, size_t size);

// A test server's packet (motion_test.cpp builds recorded-style samples with it).
std::vector<uint8_t> encode_pad_data(uint32_t server_id, const PadData& d);

class Client {
public:
    using Sink = std::function<void(const PadData&)>;
    explicit Client(Sink sink) : sink_(std::move(sink)) {}
    ~Client() { stop(); }
    // (re)starts the thread for host:port and slot; stop() ends it (joins within ~0.2 s)
    void start(const std::string& host, uint16_t port, uint8_t slot);
    void stop();
    bool running() const { return thread_.joinable(); }
    // for the settings overlay: a connected pad's data arrived within the last second
    bool receiving() const;
    std::string status() const;  // a short line for the overlay

private:
    void run(std::string host, uint16_t port, uint8_t slot);
    Sink sink_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<int64_t> last_data_ms_{0};
    mutable std::mutex mu_;
    std::string status_;
};

}  // namespace dsu

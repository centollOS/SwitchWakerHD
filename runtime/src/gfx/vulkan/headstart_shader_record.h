#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace gfxvk {
struct HeadstartShaderRecord {
  bool vertex;
  std::span<const uint8_t> program, fetch, linkedVertex, registers;
};
inline HeadstartShaderRecord decode_headstart_shader_record(std::span<const uint8_t> bytes, uint32_t type) {
  if (type != 1 && type != 3 && type != 6) throw std::runtime_error("invalid shader record type");
  const size_t header = type == 6 ? 20 : 16;
  if (bytes.size() < header) throw std::runtime_error("truncated shader record");
  auto word = [&](size_t at) { return uint32_t(bytes[at]) | uint32_t(bytes[at+1]) << 8 |
      uint32_t(bytes[at+2]) << 16 | uint32_t(bytes[at+3]) << 24; };
  const uint32_t vertex = word(0), code = word(4), fetch = word(8), regs = word(12);
  const uint32_t linked = type == 6 ? word(16) : 0;
  if (vertex > 1 || !code || code > 1024 * 1024 || code % 8 || fetch > 4096 || fetch % 4 || regs > 65536 ||
      (type == 6 && (vertex || !linked || linked > 1024 * 1024 || linked % 8)) ||
      uint64_t(header) + code + fetch + linked + uint64_t(regs) * 8 != bytes.size())
    throw std::runtime_error("invalid shader record bounds");
  const auto fetchBytes = bytes.subspan(header + code, fetch);
  if (fetch % 8 && !(fetch >= 32 && !std::memcmp(fetchBytes.data(), "WWFA0001", 8)))
    throw std::runtime_error("unaligned fetch record");
  return {vertex != 0, bytes.subspan(header, code), fetchBytes,
      bytes.subspan(header + code + fetch, linked), bytes.subspan(header + code + fetch + linked)};
}
}

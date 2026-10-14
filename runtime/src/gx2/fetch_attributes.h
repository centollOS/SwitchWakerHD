#pragma once
#include <cstdint>
#include <span>
struct LatteFetchShader;
namespace gx2 {
LatteFetchShader* decode_fetch_attributes_bytes(std::span<const uint8_t>, uint64_t* sourceHash=nullptr);
LatteFetchShader* build_fetch_shader_bytes(std::span<const uint8_t>, uint64_t identity);
void normalize_fetch_shader(LatteFetchShader*);
}

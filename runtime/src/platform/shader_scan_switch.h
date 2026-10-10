// The game's shader programs from the player's own game files on the SD card (as upstream's tools/shaderprep.py
// game_shaders on the computer): every .szs/.pack/.sarc/.sharcfb under content/, Yaz0 and SARC nesting opened, each
// SHARCFB archive's vertex and pixel shader binaries (their GX2 register words and microcode). Nothing of it is
// shipped: it is read on the console, from the console's own copy of the game.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace shader_scan {

struct Program {
    bool vertex;
    std::vector<uint32_t> words;  // the GX2VertexShader (52) / GX2PixelShader (41) register words
    std::vector<uint8_t> code;    // the microcode, as the game hands it to the GPU
};

struct Stats {
    size_t files = 0, archives = 0, shaders = 0, distinct = 0;
    uint64_t bytesRead = 0, bytesInflated = 0;
    double readSeconds = 0, inflateSeconds = 0, totalSeconds = 0;
};

// every distinct program under game_dir/content; `stats` filled
std::vector<Program> scan(const std::string& game_dir, Stats& stats);

}  // namespace shader_scan

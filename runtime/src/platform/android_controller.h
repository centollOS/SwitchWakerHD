#pragma once
#include <cstdint>
#include <string>

namespace android_controller {
// Issue #152: Thor's built-in Odin Controller (2020:0111) prints A/B/X/Y in
// Nintendo positions, but Android keycodes and SDL's generated mapping use the
// printed letters as Xbox positions. Normalize only this device to SDL positions.
inline bool odin(uint16_t vendor, uint16_t product) { return vendor == 0x2020 && product == 0x0111; }
inline std::string mapping(uint16_t vendor, uint16_t product, const std::string& original) {
    if (!odin(vendor, product)) return original;
    const auto first = original.find(',');
    const auto second = first == std::string::npos ? first : original.find(',', first + 1);
    if (second == std::string::npos) return original;
    std::string result = original.substr(0, second + 1);
    for (size_t start = second + 1; start < original.size();) {
        const auto end = original.find(',', start);
        const auto token = original.substr(start, end == std::string::npos ? end : end - start);
        const auto key = token.substr(0, token.find(':'));
        if (key != "a" && key != "b" && key != "x" && key != "y" && key != "face" && !token.empty())
            result += token + ',';
        if (end == std::string::npos) break;
        start = end + 1;
    }
    // Android's fixed joystick button indices: A=0 B=1 X=2 Y=3.
    // Include X/Y even if the driver's hasKeys capability report omitted them.
    return result + "a:b1,b:b0,x:b3,y:b2,face:bayx,";
}
}  // namespace android_controller

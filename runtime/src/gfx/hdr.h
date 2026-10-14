#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

// Explicit Windows scRGB output. The guest scan is SDR; this is a controlled
// highlight expansion, not recovery of the game's pre-tonemap lighting.
namespace hdr {
inline std::atomic<bool> enabled{false}, displayEnabled{false}, formatSupported{false};
inline std::atomic<bool> fellBackToSdr{false};
// The saved choice stays HDR; this session presents SDR and the overlay says so.
inline void fall_back_to_sdr() {
    enabled = false;
    fellBackToSdr = true;
}
inline bool resolve_output() {
    if (!enabled.load()) return false;
    if (displayEnabled.load() && formatSupported.load()) {
        fellBackToSdr = false;
        return true;
    }
    fall_back_to_sdr();
    return false;
}
inline std::atomic<float> paperWhite{240}, peak{1025}, exposure{-0.32f}, highlights{0.32f}, menuWhite{160};
inline void set(std::atomic<float>& dst, float value, float lo, float hi) {
    if (std::isfinite(value)) dst = std::clamp(value,lo,hi);
}
struct Parameters { float paper, peak, exposure, highlights; };
inline Parameters parameters() {
    const float maxNits=peak.load();
    return {std::min(paperWhite.load(),maxNits),maxNits,exposure.load(),highlights.load()};
}
// Max-channel scaling preserves hue, including saturated highlights; zero stays
// zero. The smooth shoulder limits all RGB channels to the selected peak.
inline std::array<float,3> map(std::array<float,3> c, Parameters p) {
    for(auto& v:c) v=std::max(0.0f,v)*std::exp2(p.exposure);
    const float m=std::max({c[0],c[1],c[2]});
    if(m<=0) return {};
    const float t=std::clamp((m-0.5f)*2.0f,0.0f,1.0f);
    float n=m*p.paper+(p.peak-p.paper)*p.highlights*t*t*(3-2*t);
    const float knee=std::min(p.paper,p.peak*0.75f);
    if(n>knee) n=knee+(p.peak-knee)*(1-std::exp(-(n-knee)/(p.peak-knee)));
    for(auto& v:c) v*=n/(m*80.0f); // Windows scRGB: 1.0 = 80 cd/m^2.
    return c;
}
}

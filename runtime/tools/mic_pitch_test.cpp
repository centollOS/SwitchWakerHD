// mic_pitch.h: the microphone service's pitch detector on synthetic sounds
#include "platform/mic_pitch.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static std::vector<float> tone(float hz, int n, float amp = 0.3f, int rate = 48000, float harmonics = 0) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const double t = double(i) / rate;
        v[size_t(i)] = float(amp * (std::sin(2 * M_PI * hz * t) + harmonics * std::sin(4 * M_PI * hz * t) +
                                    harmonics * 0.5 * std::sin(6 * M_PI * hz * t)));
    }
    return v;
}

int main() {
    mic::PitchDetector d;
    // clean tones across a singing range: within 0.5 %
    for (float hz : {82.4f, 110.0f, 196.0f, 261.6f, 440.0f, 659.3f, 880.0f, 1200.0f}) {
        auto v = tone(hz, d.window());
        mic::Pitch p = d.analyse(v.data());
        assert(std::fabs(p.hz - hz) / hz < 0.005f);
        assert(p.clarity > 0.8f && p.level > 0.1f);
    }
    // a voice-like tone with strong harmonics: the fundamental, not an octave up
    {
        auto v = tone(220.0f, d.window(), 0.2f, 48000, 0.9f);
        mic::Pitch p = d.analyse(v.data());
        assert(std::fabs(p.hz - 220.0f) < 2.0f);
    }
    // silence and quiet noise: no pitch
    {
        std::vector<float> v(size_t(d.window()), 0.0f);
        assert(d.analyse(v.data()).hz == 0);
        srand(7);
        for (auto& s : v) s = 0.005f * (float(rand()) / RAND_MAX - 0.5f);
        assert(d.analyse(v.data()).hz == 0);
    }
    // loud white noise: no clean dip (no pitch, or a low clarity)
    {
        std::vector<float> v(size_t(d.window()));
        srand(11);
        for (auto& s : v) s = 0.5f * (float(rand()) / RAND_MAX - 0.5f);
        mic::Pitch p = d.analyse(v.data());
        assert(p.hz == 0 || p.clarity < 0.9f);
    }
    // the stream: odd block sizes, half-window hops
    {
        mic::PitchStream s;
        auto v = tone(330.0f, 48000 / 4);
        mic::Pitch latest;
        int windows = 0;
        for (size_t at = 0; at < v.size(); at += 333) windows += s.push(v.data() + at, std::min<size_t>(333, v.size() - at), latest);
        assert(windows == int((v.size() - 2048) / 1024) + 1);
        assert(std::fabs(latest.hz - 330.0f) < 2.0f);
    }
    printf("mic_pitch_test: ok\n");
    return 0;
}

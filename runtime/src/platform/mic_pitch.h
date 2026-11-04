// SPDX-License-Identifier: MPL-2.0
// Pitch of the microphone's sound for the Mod SDK's microphone service (mic.h, wwhd_mic_*): the YIN method
// (de Cheveigné and Kawahara, 2002) on one window of mono samples. Pure: no audio device, so mic_pitch_test checks it on
// synthetic tones. The samples are analysed and dropped; nothing here keeps or writes them anywhere.
#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace mic {

struct Pitch {
    float hz = 0;         // the fundamental, 0: none (silence, noise)
    float level = 0;      // RMS of the window, 0..1
    float clarity = 0;    // 1 - YIN's dip at the period (1 = a clean tone)
};

class PitchDetector {
public:
    // rate: samples per second; window: samples per analysis; lowest / highest pitch looked for (Hz)
    PitchDetector(int rate = 48000, int window = 2048, float lowest = 70.0f, float highest = 1500.0f)
        : rate_(rate), window_(window), min_lag_(std::max(2, int(rate / highest))),
          max_lag_(std::min(window / 2, int(rate / lowest) + 1)), d_(size_t(window / 2 + 1)) {}

    int window() const { return window_; }

    // one window of `window()` samples (-1..1)
    Pitch analyse(const float* x) {
        Pitch p;
        double sum = 0;
        for (int i = 0; i < window_; i++) sum += double(x[i]) * x[i];
        p.level = float(std::sqrt(sum / window_));
        if (p.level < kSilence) return p;
        const int w = window_ / 2;
        // difference function and its cumulative mean normalisation
        d_[0] = 1;
        double running = 0;
        for (int lag = 1; lag <= max_lag_; lag++) {
            double s = 0;
            for (int i = 0; i < w; i++) {
                const double v = double(x[i]) - x[i + lag];
                s += v * v;
            }
            running += s;
            d_[size_t(lag)] = running > 0 ? float(s * lag / running) : 1.0f;
        }
        // the first dip under the threshold, followed to its minimum
        int best = -1;
        for (int lag = min_lag_; lag <= max_lag_; lag++) {
            if (d_[size_t(lag)] < kThreshold) {
                while (lag + 1 <= max_lag_ && d_[size_t(lag + 1)] < d_[size_t(lag)]) lag++;
                best = lag;
                break;
            }
        }
        if (best < 0) return p;
        // parabolic interpolation around the minimum
        float lag = float(best);
        if (best > 1 && best < max_lag_) {
            const float a = d_[size_t(best - 1)], b = d_[size_t(best)], c = d_[size_t(best + 1)];
            const float den = a - 2 * b + c;
            if (std::fabs(den) > 1e-9f) lag += 0.5f * (a - c) / den;
        }
        p.hz = float(rate_) / lag;
        p.clarity = 1.0f - d_[size_t(best)];
        return p;
    }

    static constexpr float kSilence = 0.01f;   // RMS below this: no pitch
    static constexpr float kThreshold = 0.15f;  // YIN's absolute threshold

private:
    int rate_, window_, min_lag_, max_lag_;
    std::vector<float> d_;
};

// Collects samples (any block size) into windows with half a window of overlap and analyses each.
class PitchStream {
public:
    explicit PitchStream(int rate = 48000) : det_(rate), buf_(size_t(det_.window())) {}
    // returns how many windows were analysed; `latest` holds the last result
    int push(const float* x, size_t n, Pitch& latest) {
        int done = 0;
        for (size_t i = 0; i < n; i++) {
            buf_[fill_++] = x[i];
            if (fill_ == buf_.size()) {
                latest = det_.analyse(buf_.data());
                done++;
                const size_t half = buf_.size() / 2;
                for (size_t k = 0; k < half; k++) buf_[k] = buf_[k + half];
                fill_ = half;
            }
        }
        return done;
    }
    void reset() { fill_ = 0; }

private:
    PitchDetector det_;
    std::vector<float> buf_;
    size_t fill_ = 0;
};

}  // namespace mic

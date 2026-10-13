// SwitchWakerHD (this fork): the console's sound output (audout) for audio_out.cpp (audio_out_switch.cpp).
#pragma once
#include <cstdint>

namespace audio_switch {
// starts audout (48 kHz stereo s16) and a thread that fills its buffers from `pull` (interleaved stereo
// frames); false when audout is unavailable
bool start(void (*pull)(int16_t* out, uint32_t frames));
}  // namespace audio_switch

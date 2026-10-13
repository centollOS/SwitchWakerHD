// SwitchWakerHD (this fork): audout, the console's sound output, behind audio_out.cpp's marked hook
// (audio_out_switch.h).
#include "audio_out_switch.h"

#include <switch.h>

#include <cstdlib>
#include <thread>

#include "../runtime.h"
#include "host.h"

namespace audio_switch {
namespace {
void (*g_pull)(int16_t*, uint32_t) = nullptr;

// audout plays 48 kHz stereo s16 natively: a thread keeps four page-aligned buffers queued
constexpr uint32_t kBufferFrames = 1024;
void audout_thread() {
    host::raise_thread_priority();
    AudioOutBuffer bufs[4]{};
    for (auto& b : bufs) {
        b.buffer = aligned_alloc(0x1000, kBufferFrames * 4);
        b.buffer_size = kBufferFrames * 4;
        b.data_size = kBufferFrames * 4;
        g_pull((int16_t*)b.buffer, kBufferFrames);
        armDCacheFlush(b.buffer, b.buffer_size);
        audoutAppendAudioOutBuffer(&b);
    }
    for (;;) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, UINT64_MAX)) || !released) continue;
        g_pull((int16_t*)released->buffer, kBufferFrames);
        armDCacheFlush(released->buffer, released->buffer_size);
        audoutAppendAudioOutBuffer(released);
    }
}
}  // namespace

bool start(void (*pull)(int16_t* out, uint32_t frames)) {
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        LOG("[audio] audout unavailable");
        return false;
    }
    g_pull = pull;
    std::thread(audout_thread).detach();
    LOG("[audio] audout started (%u Hz, %u channels)", audoutGetSampleRate(), audoutGetChannelCount());
    return true;
}
}  // namespace audio_switch

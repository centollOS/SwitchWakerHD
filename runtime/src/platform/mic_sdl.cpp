// SPDX-License-Identifier: MPL-2.0
// The microphone service's backend for the SDL host (mic.h): Windows, Linux, the Steam Deck and Android. On Android
// the RECORD_AUDIO runtime permission is asked here, the first time a mod wants the microphone (declared in the
// manifest, never asked at install or start); then an SDL recording stream gives 48 kHz mono floats, which go straight
// to the pitch analysis (mic::feed) and nowhere else.
#include <SDL3/SDL.h>

#include <atomic>
#include <mutex>

#include "../mic.h"
#include "../runtime.h"

namespace mic {
namespace {
std::mutex g_stream_mu;
SDL_AudioStream* g_stream = nullptr;
std::atomic<bool> g_audio_init{false};
constexpr int kRate = 48000;

void SDLCALL recorded(void*, SDL_AudioStream* s, int additional, int) {
    float buf[2048];
    while (additional > 0) {
        const int got = SDL_GetAudioStreamData(s, buf, int(sizeof buf));
        if (got <= 0) break;
        feed(buf, size_t(got) / sizeof(float), kRate);
        additional -= got;
    }
}

void open_stream() {
    if (!g_audio_init.load()) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG("[mic] audio: %s", SDL_GetError());
            started(kUnavailable);
            return;
        }
        g_audio_init = true;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, recorded, nullptr);
    if (!s) {
        LOG("[mic] no recording device: %s", SDL_GetError());
        started(kUnavailable);
        return;
    }
    {
        std::lock_guard lock(g_stream_mu);
        g_stream = s;
    }
    SDL_ResumeAudioStreamDevice(s);
    started(kOpen);
}

#ifdef __ANDROID__
void SDLCALL answered(void*, const char*, bool granted) {
    if (granted) open_stream();
    else started(kDenied);
}
#endif
}  // namespace

void backend_start() {
#ifdef __ANDROID__
    if (!SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", answered, nullptr)) started(kUnavailable);
#else
    open_stream();
#endif
}

void backend_stop() {
    SDL_AudioStream* s;
    {
        std::lock_guard lock(g_stream_mu);
        s = g_stream;
        g_stream = nullptr;
    }
    if (s) SDL_DestroyAudioStream(s);
}

}  // namespace mic

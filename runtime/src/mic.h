// SPDX-License-Identifier: MPL-2.0
// The microphone for code mods (Mod SDK, wwhd_mic_*; QoL track M0 item 4: singing the songs). Off unless a mod asks:
// nothing opens the device or asks the system for permission before a mod calls wwhd_mic_open, which a mod does only
// while its option says so (the QoL package's "Sing the songs", off by default). The first ask asks the system
// (Android's RECORD_AUDIO runtime permission, macOS's microphone prompt); declined, the state is kDenied and the port
// turns the option that asked off again (guest_mods.cpp). When no mod wants it any more the device closes at once.
// The sound is analysed for its pitch as it comes (platform/mic_pitch.h) and dropped: never kept, written, logged
// or sent anywhere; mods get only the pitch, its loudness and clarity.
//
// Backends (one per host): platform/mic_apple.mm (AppKit host: AVFoundation permission, AudioQueue input),
// platform/mic_sdl.cpp (SDL host, Android included: SDL_RequestAndroidPermission, SDL recording stream);
// without either the microphone is kUnavailable.
// Test aids: WWHD_TEST_MIC=from-to:hz,... (TV frames) feeds tones of those pitches instead of a device (no permission
// asked); WWHD_TEST_MIC_DENY=1 answers the ask with "declined".
#pragma once
#include <cstddef>
#include <cstdint>

#include "platform/mic_pitch.h"

namespace mic {

enum State : int { kClosed = 0, kAsking = 1, kOpen = 2, kDenied = 3, kUnavailable = 4 };

// a code mod (its index) wants the microphone or no longer (game thread)
void want(int owner, bool on);
bool wanted(int owner);
State state();
// the newest analysis: false when there is none yet; serial grows with each analysed window
bool latest(Pitch& p, uint64_t& serial);
// game thread, each controller read: opens / closes the device as wanted, the test aids
void service();
// the state went to kDenied since the last call (game thread: the port turns the asking options off)
bool take_denied();

// ---- for the backends
// the samples of the device (mono, -1..1; the capture thread), at `rate`
void feed(const float* mono, size_t frames, int rate);
// the outcome of backend_start (any thread): kOpen, kDenied or kUnavailable
void started(State s);
// implemented by the backend: ask (if needed) and open, asynchronously, then call started(); close at once
void backend_start();
void backend_stop();

}  // namespace mic

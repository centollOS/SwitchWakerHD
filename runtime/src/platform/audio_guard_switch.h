// The game's DSP-overload guard (issue #13): its audio code (028137E0, once per audio subframe) compares the
// subframe's time with the gap before its frame and, below 1.1, takes it for DSP overload: "kill DSP channel" and
// sounds or music tracks are stopped. On the Switch the mixing is the runtime's and never falls behind; the guard
// fires when the game's audio thread is delayed on a busy core. The threshold (a float only that comparison reads,
// 10170030) is set to 0, so the guard never fires; its timing history stays as the game keeps it.
#pragma once

namespace audio_guard {
void frame();          // the main thread, every frame (interp.cpp): applies the setting once the game's data is there
void set_kept(bool on);  // true: the game's own guard (1.1), false (the default): never fires (debug server, A/B)
bool kept();
}  // namespace audio_guard

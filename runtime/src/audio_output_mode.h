// Which of the Wii U's two sound outputs the host plays: the TV mix, or in Off-TV Play the TV and
// GamePad (DRC) mixes summed. Driven by the game's own output mode.
//
// The game switches its output in Snd_outputMode (hd_snd 02030880 / 020309AC, mode 0 TV, 1 GamePad,
// 2 both): fade_both(frames, tv, drc) fades the sound player's two master faders (player +0x194 TV,
// +0x1AC GamePad; nw fader: +0 value, +4 target, +0xC frames left) over 5 game frames (0 for the
// instant variant), and every game frame the player volumes follow them (0202F41C: TV t*t, GamePad
// d*d). Off-TV Play (Minus) is mode 1: TV to 0, GamePad to 1.
//
// The mode is read from those faders once per audio frame (ax.cpp), not from the call: the state
// is in game memory, so a boot, a mode switch and a loaded save state all give the right mode with
// no extra state of our own.
//
// Decision (gamepad_audible):
//   TV play (GamePad fader at 0)          host = TV final mix, exactly as without a GamePad mix;
//                                         sounds only on the GamePad stay unheard, as before
//   Off-TV Play and the fades into and    host = TV + GamePad final mixes. Entering, the sum starts
//   out of it                             as soon as the GamePad fader moves up (the TV fades out
//                                         meanwhile); leaving, it lasts until the TV fader is back
//                                         up, which is when the GamePad fader reaches 0
//   "both" (mode 2: both faders up)       host = TV only (both carry the same sounds; no doubling)
//                                         (from Off-TV Play the sum lasts while the TV fades in,
//                                         up to 5 game frames, then TV only)
// The sum adds nothing the game didn't fade itself: through a switch the host hears TV t^2 plus
// GamePad d^2, the game's own crossfade (its middle is ~0.52 of full level, as on the hardware).
// WWHD_AUDIO_OUTPUT=auto (default) | tv | gamepad overrides it (debug).
#pragma once
#include <cstdint>
#include <cstring>

namespace audio {

struct Fader {
    float value = 0, target = 0;
    uint32_t frames = 0;  // frames left of the fade
    float goal() const { return frames ? target : value; }  // target is stale once a fade ended
};

// the game's faders -> the GamePad mix is heard (added to the TV mix)
inline bool gamepad_audible(const Fader& tv, const Fader& drc) {
    if (drc.value <= 0 && drc.goal() <= 0) return false;  // GamePad silent and staying so
    return !(tv.value >= 1 && tv.goal() >= 1);           // TV fully up: it carries everything
}

class OutputSelect {
public:
    enum Source { kAuto, kTv, kGamePad };
    Source source = kAuto;

    static Source parse(const char* s) {
        if (!s) return kAuto;
        if (!strcmp(s, "tv")) return kTv;
        if (!strcmp(s, "gamepad")) return kGamePad;
        return kAuto;
    }
    // per audio frame: which mixes go to the host. faders_ok false: the game's sound player is not
    // there (yet): TV only
    void update(bool faders_ok, const Fader& tv, const Fader& drc) {
        play_tv = source != kGamePad;
        play_drc = source == kGamePad || (source == kAuto && faders_ok && gamepad_audible(tv, drc));
    }
    bool play_tv = true, play_drc = false;

    // one frame of n samples per channel -> out (not clamped)
    void mix(const int32_t* tv_l, const int32_t* tv_r, const int32_t* drc_l, const int32_t* drc_r, int n, int32_t* out_l,
             int32_t* out_r) const {
        for (int i = 0; i < n; i++) {
            out_l[i] = (play_tv ? tv_l[i] : 0) + (play_drc ? drc_l[i] : 0);
            out_r[i] = (play_tv ? tv_r[i] : 0) + (play_drc ? drc_r[i] : 0);
        }
    }
};

}  // namespace audio

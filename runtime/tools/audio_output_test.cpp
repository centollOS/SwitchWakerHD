// Host audio output mode (runtime/src/audio_output_mode.h): TV play plays the TV mix bit-exactly,
// Off-TV Play plays TV + GamePad, the switches in and out follow the game's own fades without a gap
// or doubling, the "both" mode is not doubled, and the mode comes from game state alone (a boot or
// a loaded save state in Off-TV Play gets it right).
#include "audio_output_mode.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

using audio::Fader;
using audio::OutputSelect;

// the game's fader (nw, 02762258 set / 027621EC per-frame update)
struct GameFader {
    Fader f;
    float step = 0;
    void set(uint32_t frames, float v) {
        if (f.value == v) { f.frames = 0; return; }  // (target left as it was)
        f.target = v;
        f.frames = frames;
        if (frames == 0) { f.value = v; step = 0; }
        else step = (f.value - v) / (float)frames;
    }
    void update() {
        if (!f.frames) return;
        if (--f.frames) f.value -= step;
        else { f.value = f.target; step = 0; }
    }
};

// the game: sound player master faders, per-frame volumes (0202F41C: TV t*t, GamePad d*d)
struct Game {
    GameFader tv, drc;
    float tv_gain = 1, drc_gain = 0;
    Game() { tv.f.value = tv.f.target = 1; }
    void output_mode(int mode, uint32_t frames) {  // 02030880 (5 frames) / 020309AC (0)
        float t = mode == 1 ? 0.f : 1.f, d = mode == 0 ? 0.f : 1.f;
        tv.set(frames, t);
        drc.set(frames, d);
    }
    void frame() {
        tv.update();
        drc.update();
        tv_gain = tv.f.value * tv.f.value;
        drc_gain = drc.f.value * drc.f.value;
    }
};

// runs `ms` of time: game frames at 60 Hz, audio frames every 3 ms; one sound playing on whatever
// devices the game routes it to. Returns min/max of the host level (1 = the sound at full level)
struct Levels { double lo = 1e9, hi = 0; bool drc_end = false; };
static Levels run(Game& g, OutputSelect& o, int ms) {
    Levels L;
    double next_game = 0;
    for (int t = 0; t < ms; t += 3) {
        while (next_game <= t) { g.frame(); next_game += 1000.0 / 60; }
        o.update(true, g.tv.f, g.drc.f);
        double level = (o.play_tv ? g.tv_gain : 0) + (o.play_drc ? g.drc_gain : 0);
        // nothing the game plays is dropped: the host hears every device that carries sound...
        double carried = g.tv_gain + g.drc_gain;
        bool tv_full = g.tv_gain >= 1;
        assert(level >= (tv_full ? 1.0 : carried) - 1e-6);
        if (level < L.lo) L.lo = level;
        if (level > L.hi) L.hi = level;
        L.drc_end = o.play_drc;
    }
    return L;
}

int main() {
    {
        // TV play: the host gets the TV final mix bit-exactly, the GamePad mix not at all
        OutputSelect o;
        Game g;
        o.update(true, g.tv.f, g.drc.f);
        assert(o.play_tv && !o.play_drc);
        int32_t tl[144], tr[144], dl[144], dr[144], ol[144], orr[144];
        srand(5);
        for (int i = 0; i < 144; i++) {
            tl[i] = rand() % 80000 - 40000; tr[i] = rand() % 80000 - 40000;
            dl[i] = rand() % 80000 - 40000; dr[i] = rand() % 80000 - 40000;
        }
        o.mix(tl, tr, dl, dr, 144, ol, orr);
        for (int i = 0; i < 144; i++) assert(ol[i] == tl[i] && orr[i] == tr[i]);
        // before the game's sound player exists (boot): TV
        o.update(false, Fader{}, Fader{});
        assert(o.play_tv && !o.play_drc);
        printf("TV play: TV mix bit-exact\n");
    }
    {
        // Off-TV Play (Minus): summed from the call on, through the fade, and while it lasts
        OutputSelect o;
        Game g;
        run(g, o, 200);
        g.output_mode(1, 5);
        o.update(true, g.tv.f, g.drc.f);
        assert(o.play_drc);  // immediately, before the next game frame
        Levels L = run(g, o, 300);
        printf("into Off-TV Play: host level %.3f .. %.3f (the game's crossfade t^2 + d^2)\n", L.lo, L.hi);
        assert(L.lo > 0.5 && L.hi <= 1.0 + 1e-6 && L.drc_end);
        assert(g.tv_gain == 0 && g.drc_gain == 1);
        int32_t tl[4] = {0, 0, 0, 0}, dl[4] = {100, -200, 300, -400}, ol[4], orr[4];
        o.mix(tl, tl, dl, dl, 4, ol, orr);
        for (int i = 0; i < 4; i++) assert(ol[i] == dl[i] && orr[i] == dl[i]);
        printf("Off-TV Play: host = TV + GamePad (= the GamePad mix)\n");

        // a save state taken now and loaded into a fresh session: the mode comes from game memory
        OutputSelect fresh;
        fresh.update(true, g.tv.f, g.drc.f);
        assert(fresh.play_drc);
        printf("save state in Off-TV Play: loads as Off-TV Play\n");

        // back to the TV: summed until the fade is over, then the TV alone
        g.output_mode(0, 5);
        Levels B = run(g, o, 60);
        printf("back to TV play: host level %.3f .. %.3f\n", B.lo, B.hi);
        assert(B.lo > 0.5 && B.hi <= 1.0 + 1e-6);
        Levels C = run(g, o, 200);
        assert(!C.drc_end && C.lo == 1 && C.hi == 1);
        assert(g.drc.f.value == 0 && g.tv.f.value == 1);
        printf("TV play again: TV only\n");
    }
    {
        // the instant variant (020309AC) both ways
        OutputSelect o;
        Game g;
        g.output_mode(1, 0);
        Levels L = run(g, o, 100);
        assert(L.lo >= 1 - 1e-6 && L.hi <= 1 + 1e-6 && L.drc_end);
        g.output_mode(0, 0);
        L = run(g, o, 100);
        assert(L.lo >= 1 - 1e-6 && L.hi <= 1 + 1e-6 && !L.drc_end);
        printf("instant switches: level 1 throughout\n");
    }
    {
        // "both" (mode 2) from TV play: both devices carry everything -> TV only, not doubled
        OutputSelect o;
        Game g;
        g.output_mode(2, 5);
        Levels L = run(g, o, 200);
        assert(!L.drc_end && L.hi <= 1 + 1e-6);
        printf("both mode: TV only, level %.3f\n", L.hi);
    }
    {
        // a fader set to the value it is fading through keeps a stale target: the goal is its value
        GameFader f;
        f.f.value = 1;
        f.set(5, 0);
        f.update();
        f.update();
        f.set(5, f.f.value);
        assert(f.f.frames == 0 && f.f.goal() == f.f.value);
        Fader tv;
        tv.value = 1;
        assert(!audio::gamepad_audible(tv, Fader{}));
        printf("stale fader target: handled\n");
    }
    {
        // debug overrides
        OutputSelect o;
        assert(OutputSelect::parse(nullptr) == OutputSelect::kAuto && OutputSelect::parse("auto") == OutputSelect::kAuto);
        o.source = OutputSelect::parse("tv");
        Game g;
        g.output_mode(1, 0);
        g.frame();
        o.update(true, g.tv.f, g.drc.f);
        assert(o.play_tv && !o.play_drc);
        o.source = OutputSelect::parse("gamepad");
        o.update(false, Fader{}, Fader{});
        assert(!o.play_tv && o.play_drc);
        printf("overrides: tv / gamepad\n");
    }
    printf("audio output: all passed\n");
    return 0;
}

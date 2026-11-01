# Issue #101: a Mods slider locks the settings menu on Switch

## What happens

Moving a built-in mod's option slider (Mods > Built-in > Sailing speed > Normal sail; reproduced on the console
with v1.0.0) leaves the menu stuck:

- the value keeps moving up and down on its own,
- the controller no longer reaches the menu (the D-pad, A and Minus do nothing),
- `settings.ini` and `ModManager/profiles.json` are rewritten on the SD card without a pause,
- only restarting the game gets out of it.

## Cause

1. On Switch, one thread (`gfx/deko/backend.cpp` `run_main_loop`) reads the controllers (`input::update`) and runs
   the overlay's posted changes (`hostui::run_posted`). `run_posted` (`platform/hostui_switch.cpp`) keeps going
   until the queue is empty, including posts added while it runs.
2. Each step of these sliders posts `apply + hostui::set + packages::remember_option`. That is two SD card writes
   per step: `settings.ini` (tmp + rename) and `profiles.json`.
3. The slider reads the option back every frame. While posts wait in the queue, that value is stale, so the slider
   tweaks the stale value and posts again.
4. Once posts come in faster than the SD writes finish, `run_posted` never returns. `input::update` stops, and the
   overlay keeps the last controller state it saw (a D-pad direction held). ImGui keeps repeating the tweak, which
   keeps the queue full.

## Fix (two steps, one commit each)

### Step 1: `run_posted` runs only what was queued when it started (platform/hostui_switch.cpp, fork file)

Swap the queue into a local one and run that. Posts added meanwhile wait for the next loop pass, so
`input::update` runs between passes however many posts come in. This is the guard that removes the livelock for
every option, including those not changed in step 2.

### Step 2: option sliders apply while held and save once on release (overlay/overlay.cpp)

The audio volume and the Switch picture sliders already work this way. A small helper, `option_slider`, does the
same for the built-in mod options:

- While the slider is held, it shows and moves its own value instead of reading the option back.
- Each step posts the option's in-memory setter only, with no file writes.
- On release (`IsItemDeactivatedAfterEdit`), or for a change made without holding the slider, it posts the save
  (`hostui::set` + `remember_option`) once.

It is used by the built-in mod sliders in `builtin_options`: camera speed, lock-on range, running, swimming,
boost time, cooldown, ladder/sidle/ivy/hang, the normal sail, the Swift Sail and mouse sensitivity. It is also
used by Gameplay FOV, which writes `settings.ini` on every step too. `mods::manager::set_traversal_option` gets a
`save` flag (default true) so the traversal sliders can apply without saving.

Not changed here: the GamePad picture-in-picture percent sliders and the Vulkan-only stereo/HDR sliders. Step 1
already keeps them from locking the menu; they can move to `option_slider` later.

## Checks

- Build the Switch NRO (USA, build/gen from the v1.0.0 recomp: no tools/recomp changes since).
- On the console, ask the user first. Mods > Built-in > Sailing speed: hold the D-pad on Normal sail for a few
  seconds, then let go.
  - The value stops when the D-pad is released.
  - B leaves the slider and Minus closes the menu.
  - `settings.ini` and `profiles.json` hold the final value, and are not rewritten after release (poll with the
    debug server's `ls`).
  - The boat sails at the new speed (no restart needed).
- Restart the game: the value is kept.
- Afterwards, restore the console's sail value (2.36x) as it was.

## Results (2026-11-01, console, HOS 22.1.0, handheld, USA, build b2e51df + this branch)

- Normal sail: A, D-pad right held 3 s. The value went smoothly to 3.00x and stopped when the D-pad was released.
  While the slider was held, `settings.ini` and `profiles.json` kept the old value (no writes). A again: both
  saved 3.00 once, then stayed unchanged.
- A, 32 single taps left, A: exactly 2.36x, and both files saved 2.36.
- Minus closes the menu right after (the log shows `[overlay] closed`).
- Not checked: sailing at the new speed in game, and a restart (the load path is unchanged and the files hold
  the value).

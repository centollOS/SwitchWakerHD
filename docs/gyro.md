# Gyro aiming

On the Wii U, Wind Waker HD lets you aim in first person by moving the GamePad: the bow, hookshot,
boomerang, telescope, Picto Box, grappling hook and the plain first-person look (R3) all use it.
The port has no GamePad, so it turns a **virtual GamePad** with one of these sources:

| Source | What it uses |
|---|---|
| Off (default) | the GamePad lies still: no gyro aiming, as before |
| Controller gyro | the gyro and accelerometer of a DualSense, DualShock 4, Switch Pro, Joy-Con, Steam Deck / Steam Controller and other controllers SDL3 reads (the macOS app uses GameController.framework: DualSense, DualShock 4, Switch Pro, Joy-Con) |
| Cemuhook (DSU) | a Cemuhook motion server over UDP: DS4Windows, BetterJoy, SteamDeckGyroDSU, phone apps. Default 127.0.0.1, port 26760, slot 1 |
| Mouse | mouse movement turns the GamePad while the game aims; made for Steam Input's "gyro to mouse" output, works with a plain mouse too |

Settings overlay (F1) → **Controls** → **Gyro…**:

- **Source** (above). `WWHD_GYRO=off|controller|cemuhook|mouse` overrides it at start.
- **Sensitivity left/right** and **up/down** (0.1x to 5x), each with **Invert**.
- **Mouse: degrees per point**: how far one point of mouse movement turns the GamePad.
- **Cemuhook**: server, port and controller slot (1 to 4).
- **Recenter**: a controller input and/or a key, and a **Recenter now** button. The game re-reads the
  GamePad's direction every frame, so recentering is rarely needed; it resets the virtual GamePad's
  direction (and the mouse source's pose).

Everything is saved with the other settings (`gyro.*` in `settings.ini`, or `display.plist` on
the macOS app). The game's own **Options → Gyro** switch (on by default in the game) still decides
whether the game uses the motion, and the game ignores it while the right stick is pushed.

Why off by default: the gyro moves the first-person camera with every hand movement, which surprises
players who never asked for it, and a controller lying on a desk works fine either way. The Gyro
window says when a connected controller has a gyro.

## How the game reads the GamePad

From the decompilation (`wwhd_src`):

- The game imports `VPADRead` and no VPAD gyro setup function, so it runs with the library defaults.
- `ControllerMgr::calc` (02617AF4) copies the **direction matrix** of the newest `VPADStatus` sample
  (+0x6C, +0x78, +0x84: the GamePad's X, Y and Z axes) every frame (026173B0). The gyro rate (+0x38),
  angle (+0x44) and accelerometer (+0x1C) are not read.
- Its only reader is `dCamera_c::CalcSubjectAngle` (02506964), the first-person ("subject") camera:
  `R = Cᵀ·M` with `C` the matrix of the previous frame (`calibrate`, 026185BC, runs every frame and
  when first person starts), yaw input `(R[2][0] − R[1][0])·30` (rotation about the GamePad's Y or Z,
  so flat and upright holding both work), pitch input `R[2][1]·30`. It runs only when the options
  byte `+5` (the in-game Gyro switch, default on) is set, the controller mode is not "Pro Controller
  only", and the C-stick is within its 0.1 dead zone.
- Every item aim goes through that camera; no item code reads the gyro itself.

So what matters is that the direction matrix turns exactly as a real GamePad's would, frame to frame.
The port still fills the rate, angle and accelerometer fields consistently for completeness.

## How the port does it

`runtime/src/motion/`:

- `fusion.cpp`: the axis mapping of the SDL and Cemuhook sources into one frame, orientation
  integration (quaternion) with a light gravity correction, gyro bias estimation while the
  controller rests (no slow camera drift), a small noise floor, sensitivity / invert applied to the
  turn about the world's vertical (left/right) and the controller's right axis (up/down), and the
  VPAD values. The conventions follow the matrices Cemu recorded from a real GamePad (Cemu is
  MPL-2.0, as this port; the axis signs and the attitude-matrix layout are adapted from it, the
  code is our own). The unit test checks the matrices against those recordings.
- `dsu.cpp`: a small Cemuhook client: one background thread, 100 ms receive timeout, a data request
  per second (servers drop quiet clients), CRC-checked packets, never blocks the game.
- `motion.cpp`: the sources, settings, the recenter binding and what `VPADRead` gets. The mouse
  source only takes movement while the game aims (first-person camera or an item aim, from
  `mods/camera.cpp`); the pointer is captured then, and the mouse camera mod leaves the mouse alone.
- Hosts: `platform/input_sdl.cpp` (SDL3 sensors, on only for the controller source),
  `gfx/input.mm` (GameController.framework, macOS 11+), `platform/mouse_sdl.cpp` and `mods/mouse.mm`
  (mouse).

Debug: `WWHD_TEST_GYRO=from-to:yaw:pitch,...` turns the virtual GamePad by yaw / pitch degrees per
second during game-time seconds (any source, also off; for headless tests).
`WWHD_GYRO_LOG=1` (macOS app) logs a raw GameController sample twice a second.

## Testing with real hardware (wanted)

The axis mapping is tested against recorded GamePad matrices and simulated controllers, but not yet
with physical controllers. Please report (with the log) if something is off:

1. **DualSense / DualShock 4 / Switch Pro** with **Controller gyro**: enter first person (R3) and
   draw the bow. Turning the controller right turns the view right, tilting its top up looks up,
   in both holds (flat and upright). With the controller on a desk the view must stay still after
   a second (bias calibration); `[gyro]` log lines show the source and calibration.
2. Same on the **macOS app** (GameController.framework, a different axis mapping:
   `WWHD_GYRO_LOG=1` logs the raw values).
3. **Steam Deck** (SDL) and **Joy-Con** pairs.
4. **Cemuhook**: DS4Windows or BetterJoy with the server on, source Cemuhook; the Gyro window shows
   "receiving motion". Stop the server: the game must keep running smoothly. Android needs network
   permission for this, which the app does not ask for yet.
5. **Steam Input gyro to mouse**: source Mouse; in first person the pointer is captured and the
   controller's gyro turns the view; after leaving first person the pointer is free again.
6. Sensitivity, invert and recenter bindings; the in-game Options → Gyro switch off disables it.

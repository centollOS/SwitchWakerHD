# Wind Waker HD on Android

**English** · [Português](android.pt.md) · [Español](android.es.md)

The Wind Waker HD runs natively on Android devices. It isn't an emulator: the game's own
code is recompiled for ARM processors and draws with Vulkan. You need your own copy of the Wii U
game; this project never provides game files.

## What the Android version does

| | |
|---|---|
| **On-screen controls** | Both sticks, the D-pad, A B X Y, L R ZL ZR and + −. The 🎮 button under the view button (top left) shows or hides them. They hide while a controller is connected and come back on the next touch. A Bluetooth or USB controller works too. |
| **The GamePad screen on pause** | Press + and the picture switches to the GamePad screen (map and items), like the pause menu of the GameCube version. Press + again to go back. |
| **60 fps that keeps the game's speed** | On fast Android devices the game shows 60 frames a second. When the Android device gets hot it switches to a steady 30 by itself, so the game never slows down. |
| **Older Android devices** | If your Adreno GPU's driver is too old, the error screen offers to install a custom driver such as Mesa Turnip. |
| **Saves** | Export and import your save files and keep them in Android's backup. Saves from a real Wii U (USA or Europe) work too. |

## Tested phones

Measured with the game's own profiler in the busiest part of Outset Island (about 3,900 draws per
frame). *Game speed* is the game's logic steps per second: 30 is full speed.

| Device | SoC / GPU | Frame rate | Game speed | Status |
|---|---|---|---|---|
| Galaxy S25 Ultra | Snapdragon 8 Elite, Adreno 830 | about 59 fps | 29.7 / 30 | great |
| Galaxy Z Fold 8 | SM8850, Adreno 840 | 60 fps, 30 when hot | 26–30 / 30 | great, on both screens |
| Lenovo Legion Tab | Snapdragon 8 Gen 3, Adreno 750 | steady 30 fps | 28.7–30 / 30 | good |
| OnePlus 8 Pro | Snapdragon 865, Adreno 650 (Mesa Turnip) | 30 fps | 26–30 / 30 | runs; one GPU crash still being looked at |
| Galaxy Tab S6 Lite | Snapdragon 720G, Adreno 618 (Mesa Turnip), 4 GB | 15–24 fps | 15–24 / 30 | runs slowly: about half speed in Outset |

You need a 64-bit Android device with Android 13 or newer and Vulkan 1.3 (or a custom driver on Adreno),
and a few GB of free storage.

## How to play

### The setup app (no PC needed)

The setup app comes with every release: download `WindWakerHD-<version>-android-arm64.apk` from the
[Releases](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/releases) page (the `android-x86_64` APK is for emulators and Chromebooks). It carries no game
code: it builds the game on your Android device from your own copy. Later versions install over it and keep
your saves and settings (the Android device prepares the game again when an update needs it).

1. Install the APK. If you already have a version you built on your PC, export your save first
   (long-press the app icon), then uninstall it: the two are signed differently.
2. Open the app and choose your game: **Choose extracted game folder**, **Choose WUA archive**, or
   **Choose WUD / WUX disc image** (then also **Choose disc key file** and **Choose common key file**).
3. Tap **Start / resume / retry setup**. The Android device extracts and compiles the game once; the screen
   shows the progress, for example "Compiling: 30/80 · about 12 min left". You can leave the app
   meanwhile, and **Pause setup** stops it safely.
4. When it says the setup is complete, tap **Play current build**.

### Or: build it on your PC

1. Install the Android SDK and NDK, JDK 17 or newer, CMake, Ninja and Python 3.
2. Extract your game and generate its code (steps 1 and 2 under *Building* in the [README](../README.md)).
3. Run `android/build_native.sh`, then `./gradlew assembleRelease` in `android/`.
4. Install the APK over USB with `adb install -r app/build/outputs/apk/release/app-release.apk`.

The full steps are in the README, section *Android (build it yourself)*.

## Fair play

- Use your own copy of the game, dumped from your own disc or console.
- An APK you build on your PC contains your recompiled game. Keep it for your own Android device and never
  share it.
- If someone offers a ready-made APK with the game inside, it isn't from this project.

## Questions

**Do I need a controller?**
No. The on-screen controls cover the whole GamePad. A controller works too and hides the touch
buttons while it is connected.

**Which version of the game?**
The Wii U release of The Wind Waker HD, USA or Europe. Saves from a real Wii U work too.

**My Android device gets hot. Is that normal?**
The game is demanding. When the Android device gets warm, the game drops from 60 to a steady 30 fps on its
own and keeps its full speed.

**Something doesn't work.**
Please open an [issue](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/issues) with your Android device
model, Android version and what happened. After a crash, the next start offers the crash report
to share.

---

The Android port is by [rhemfur](https://github.com/rhemfur), part of ZeldaWWHDRecomp. A fan
project, not affiliated with or endorsed by Nintendo. The Legend of Zelda and The Wind Waker are
trademarks of Nintendo.

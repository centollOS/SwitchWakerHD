# Video clips with the Capture button (#38)

## Goal

Holding the Capture button saves the last 30 seconds as a video, as in other games. Off by default, because it
takes 96 MiB of the game's memory.

## How the Switch does it

- The forwarder's NACP has `VideoCapture = Manual` (`tools/switch/forwarder/make_nacp.py`). Automatic (2) would
  make hbloader hold back 96 MiB for every player.
- With Manual, the software turns recording on: `appletInitializeGamePlayRecording()` gives the system a 96 MiB
  (0x6000000) transfer memory out of our heap, kept until the program ends (libnx frees it in `appletExit`), then
  `appletSetGamePlayRecordingState(true/false)`.
- Only for an application. Started from the Homebrew Menu (an applet) there are no clips. Started through another
  title (hbmenu on a game, "title takeover"), that title's NACP decides: a game with automatic recording already
  records (#38: works on Breath of the Wild), and initializing a second buffer there is not safe. So the option
  works only when our process is the forwarder's title (0x01FF575748440000, `forwarder.env`). A forwarder built
  with `WWHD_FORWARDER_TITLE_ID` shows the option as "through another game".

## Memory

The worst case measured (`docs/first-start-memory-plan.md`, step 4): the first start with empty shader caches, then
a 40-warp tour. 408 MiB of heap never used at the end. Minus 96 MiB leaves about 312 MiB, above the 256 MiB at
which the texture pool is emptied. The buffer is taken at start (`settings_switch::apply_at_start`), while the heap
is free, or when the option is switched on.

## Code (fork module)

- `platform/video_clips_switch.{h,cpp}`: availability (applet type, program ID, `appletIsGamePlayRecordingSupported`),
  the saved option `switchVideoClips`, the buffer once, and recording on or off. `[clips]` log lines, with the heap
  never used before and after.
- `settings_switch.cpp`: `video_clips::apply_at_start()` (one call).
- `overlay_switch.cpp`, Picture tab, after the frame rate counter: "Video clips" heading and the option, disabled
  with the reason when not available. Turning it off stops recording, and a note says that the 96 MiB stay in use
  until the game restarts.
- `ui_text_switch.{h,cpp}`: the texts in English, Spanish, French, German and Italian.

## Checks (console)

1. From the HOME menu icon, the option off: no `[clips]` buffer line, no change in memory.
2. Switch it on: `[clips] 96 MiB video buffer given ... heap never used A -> B MiB` (B = A - 96) and
   `recording on: rc 0x0`. Hold Capture in game: a video in the album.
3. Switch it off: `recording off`, the note shows. Holding Capture then saves no video.
4. Restart with the option on: the buffer is taken at start, and clips work without opening the menu.
5. Memory, worst case: with the option on, the first start with empty shader caches, then the warp tour. No crash,
   and heap never used stays well above 256 MiB.

## Results (2026-11-01, console, HOS 22.1.0, handheld, started from the HOME menu icon)

1. Option off (the default): `[clips] video clips: off (the default), available`, no buffer.
2. Switched on in the Picture tab: `96 MiB video buffer given to the system (Picture tab): heap never used 656 ->
   560 MiB`, `recording on: rc 0x0`, and `switchVideoClips=1` saved. The user held Capture in game: the clip is in
   the album.
3. Switched off: `recording off: rc 0x0`, and the note "the 96 MiB stay in use until the game is restarted" shows.
4. Restarted with the option on: at start, `video clips: on, available`, buffer given (`3224 -> 3128 MiB`), and
   `recording on: rc 0x0` without opening the menu.
- The help tooltip was too wide for the screen on one line: a line break added in all five languages. Checked.
- Not done yet: check 5 (memory, worst case: first start with empty shader caches plus the warp tour, with the
  option on), and checking that holding Capture saves nothing while the option is off.

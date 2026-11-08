# Update notice (first version)

SwitchWaker (the GameCube port) checks GitHub for a new release and installs it from the game
(SwitchWaker issues #27/#28). SwitchWakerHD cannot install one: every player builds the NRO from their own
dump. This first version only says that a new release is out.

## What it does

- At start, when Dev tab > Updates > "Check for updates at start" (`switchUpdateCheck`, on by default) is
  on and the build has a release version (`build::version()` starts with vX.Y.Z; "dev" never checks): a
  background thread GETs `https://api.github.com/repos/centollOS/SwitchWakerHD/releases/latest` and
  compares `tag_name` with the build's version (a build past a tag, `v0.4.7+55`, is not older than v0.4.7).
- HTTPS through libnx's ssl service (the console's CA store, peer CA and host name verified) on a plain
  socket: no switch-curl, so the installers and build scripts need no new devkitPro package. HTTP/1.0 with
  `Connection: close`; a chunked answer is put together anyway.
- Newer: a line at the bottom of the picture for 12 s (the frame layer's notice, while the menu is closed and
  no first-start card or Prepare graphics screen is up): "SwitchWakerHD vX is out (you have vY): update it
  with the installer; your saves stay." The Dev tab's Updates section (shown to every player, above the
  developer options) says the same, or up to date / checking / why a check failed.
- Offline or any error: one `[update]` log line, the Dev tab's status; nothing on screen.
- Texts in English, Spanish, French, German and Italian (ui_text::MenuTexts).
- `[dev] WWHD_UPDATE_VERSION=v0.4.0` pretends an older build (tests).

## Pieces

- `runtime/src/platform/update_notice_core.h`: versions, the HTTP answer, `tag_name` / `name` from the JSON
  (header only; `tools/switch/update_notice_host_test.cpp` on a computer).
- `runtime/src/platform/update_notice_switch.{h,cpp}`: the thread, the TLS GET, the state, the notice line.
- Hooks: `main_switch::start()` (start), `overlay_switch.cpp` (the frame layer's notice, the Dev tab
  section), `ui_text_switch` (texts), `cmake/SwitchPort.cmake` (the source).

## Later (not in this version)

- "Don't tell me about this version", the release notes, a popup instead of the line.
- Telling the installer to update (it already builds the latest release).

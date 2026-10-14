# Windows Vulkan stutter check

Use this checklist for a test build supplied by the maintainer. Keep the previous installation and its settings intact. All draws use Exact state. Pipeline waits can repeat the last completed picture.

## Record the build and machine

- Record the source revision, any patch identifier, archive SHA-256 and executable SHA-256 supplied with the build. Check downloaded files with `Get-FileHash -Algorithm SHA256`. Keep baseline and candidate executable hashes separately.
- Record Windows version/build, CPU, RAM, GPU and driver version, display refresh rate, resolution/render scale, graphics options, interpolation mode and enabled mods. Record game region/version without sharing game files.
- Keep the laptop on AC with the same power profile. Note battery state, thermal throttling, fans and background activity. Pause downloads, builds and other heavy work for comparisons; record interruptions and repeat affected samples.
- Install into a separate folder and let the installer compile your own game locally. The test archive must contain runtime/tools and the guard-validated code-free state template only: no generated game code, game files, saves, recorded shader payloads or shader/pipeline caches. Let **Preparing shaders** build private caches from your own game files; [manual Windows instructions](../README.md#vulkan-shader-head-start-on-windows-and-linux) are also available.

## Check startup and settings

1. Start the supplied executable with `--renderer-smoke` and retain its output. A passing build or unit test alone does not establish GPU compatibility.
2. Use separate test settings and cache directories. Confirm startup, the title screen and the tested scene render correctly. Include a cache directory with non-ASCII characters to check Unicode file publication.
3. Remove old experimental shader-compilation and delay environment overrides. Compare the supplied baseline and Exact candidate with matching graphics settings.
4. Check setup preparation, skipping, and **Graphics → Rebuild shader cache** followed by setup repair/update. Record whether preparation finishes or falls back with a message. After skipping, confirm the next start plays without repeating preparation; a rebuild request should enable preparation in setup again. Ordinary launches must never start preparation, including after an update or a previous preparation failure. Quit once while fresh caches are compiling and check that shutdown completes without device loss. A recoverable pipeline driver error should be logged and its draws omitted while the game continues; keep any shader capture files private. A Windows cross-build or passing unit tests do not establish native Windows GPU behavior.

## Compare the same route

First test native **30 fps**, without interpolation, on an agreed route from the same local save/checkpoint. Then repeat the slowdown scenario from [issue #68](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/issues/68) at 60 fps. Keep the sail/interpolation symptom separate from first-use shader stutter. Describe where the route starts, its duration and actions, and whether a slowdown repeats on a second visit.

For each executable, collect at least **three cold and three warm runs**, alternating the executable order. Restart the application for every run. Cold means a new, empty application-cache directory each time. For warm runs, prepare each executable's cache on the complete route, then copy that seed into a new private cache directory for every repetition. Never clear your normal caches or modify the seed. Record hashes of seed files locally; do not distribute them. GPU driver caches are separate and may remain warm even with empty application caches.

Keep `WWHD_SHADER_CACHE`, `WWHD_VK_SHADER_CACHE` and `WWHD_VK_PIPELINE_CACHE` inside each private run directory. Keep settings and copied saves there too. Disable the head start consistently when measuring ordinary cold compilation; test a locally prepared head start separately. A cache flag or copied directory does not prove the route was fully warmed.

The repository benchmark runner's macOS load-average/process gates do not directly work in native Windows Python. Use a maintainer-reviewed Windows harness or a documented manual route with matching checkpoints; do not substitute Windows CPU percentage for the macOS load/core thresholds.

## Collect useful evidence

Enable `WWHD_VK_STUTTER_TRACE` with a unique local CSV path per run and retain the runtime log. Record overlay CPU/GPU frame times during a slowdown, shader and pipeline worker activity, compilation waits. Keep the route/checkpoint, mode, binary hash, cache preparation and interruptions alongside each result. Reject traces with dropped events, failed writes or synthetic publication-delay events.

For picture checks, use warmed caches and a checkpoint relative to the completed load, with matching game counters and inputs. Capture baseline twice and candidate twice; compare TV, GamePad and presented images. Repeat baseline variation before attributing waves or particles to a rendering change. Both executables must produce the same completed picture at matching checkpoints.

Share a concise result table, reviewed logs/traces, overlay screenshots and comparison screenshots. Logs may contain local paths; review them first. **Do not share game files, generated game code, saves, shader recordings/templates/captures, or shader/pipeline cache files.** Keep those inputs and all cache seeds local.

Windows requires Windows 10 version 1903 or newer (Windows 11 on ARM64). The game,
setup/launcher and bundled tools select UTF-8 for Windows file paths and console
logs, so names such as `Jürgen` work consistently with Python setup. Maintainers
can run `windows_utf8_paths` and `windows_utf8_python_agreement` with CTest on a
Windows host to check narrow controls/settings I/O and the shared shader cache root.
Cross-compilation alone does not validate Windows filesystem or console behavior.

# Shipping a shader state manifest with releases (issue #88)

Goal: setup builds `shadercache_dksh.bin` on the player's computer from their own dump and a code-free manifest we
ship, so the console's first start compiles almost nothing. Compiled or translated shaders never ship. This revisits
the decision of 2026-10-10 in `docs/shader-cache-from-dump-plan.md`: upstream (v0.4.0) now packages a code-free
shader state template into its releases from a private, pinned repository (`tools/shadercache/README.md` upstream).

## What the console records today (measured on a full-game manifest, 2026-10-23)

`shader_manifest.bin` (WSM1): 164,243 variants (141,487 vertex, 22,756 pixel), 8,432 distinct programs, 131 MiB.
Each record: program hash/size/prefix hash, the fetch shader, ~191 non-zero context registers, the runtime's
variant key.

- **Fetch shaders:** 120,075 vertex records hold the GX2 fetch shader **microcode** (legacy form, `02 00 00 00`...);
  21,412 hold the runtime's own compact description (`WWFS`). Microcode must not ship.
- **Redundancy:** 154,537 distinct variant keys, but the console's caches hold ~11,000 compiled shaders: most
  variants translate to the same shader.

## The shipped file

**Format: upstream's code-free schema** (`tools/shaderprep_format.py`, WWHT0003: bounded zlib canonical JSON of
states `{hash, vertex, fetch, registers}`; fetch as semantic attributes; no byte or string fields). Our
`tools/release/guard.py` already knows and validates it. Registers as upstream keeps them: every pair but the program
addresses (`SQ_PGM_START_*`) and the constant banks (0xC000-0xDFFF).

**Distilled:** one state per distinct translated shader (per stage), chosen on our computer with our dump: a few
thousand states instead of 164,243.

**Encrypted** with a key from the player's own dump: SHA-256 over `content/Common/Shaders/{Prim,particle}/
wii_pipeline.sharcfb` and `content/Common/Shaders/render_buffer/render_buffer.sharcfb` (byte-identical in the USA and
European games, checked 2026-10-23). File: `WSME0001`, a 16-byte nonce, the ciphertext (SHAKE-256 keystream over key
+ nonce, Python's `hashlib`), a SHA-256 tag of key + nonce + plaintext so a wrong dump is reported, not
mis-decrypted. Not secrecy against attackers: the data only means something to someone who owns the game.

**Checked for game bytes** before encryption: no 16-byte window of any program the dump's archives hold (and of the
three files above) appears in the plaintext.

## Pieces

1. `dksh_cache translate`: accept semantic fetch attributes (WWFA0001, as the runtime's `gx2/fetch_attributes.cpp`)
   besides compact (WWFS) and legacy fetch; report which variant gave which source (`--map`).
2. `tools/switch/shader_manifest.py`:
   - `distill CONSOLE_MANIFEST GAME_DIR OUT` (maintainers): translate everything, keep one state per distinct output,
     convert fetch to semantic attributes, drop the program addresses and constants, write WWHT0003 JSON, run the
     game-bytes scan.
   - `seal PLAIN GAME_DIR OUT` / `open SEALED GAME_DIR OUT`: the encryption.
   - `from-template TEMPLATE GAME_DIR OUT_MANIFEST`: back to WSM1 records for the existing speculate / translate /
     build chain.
3. Release: private repository `centollOS/SwitchWakerHD-shader-manifest` holding the sealed file; the public
   repository pins it in `tools/shadercache/manifest.ref` (commit + SHA-256). SwitchWakerHD releases are packaged by
   a maintainer (`tools/switch/package_release.sh`, not CI), so a maintainer script fetches the pinned file with the
   maintainer's own GitHub access, checks the hash and attaches it to the release as an asset: no deploy key or CI
   secret. Forks have no access and attach none.
4. Setup: `make_sd.py` downloads the release's asset (or takes `--shaders`), opens it with the dump, builds the cache;
   on by default with an opt-out; a missing or mismatched file means no prepared shaders, never an error. Installers
   follow.
5. Console: unchanged (a present `shadercache_dksh.bin` already skips the first-start screen).
6. Docs: README, INSTALL.md, `docs/shader-cache-from-dump-plan.md` (decision), AGENTS.md, release checklist.

## Checks

- Same manifest + same dump, twice: byte-identical `shadercache_dksh.bin`. Two different dumps of one region: the
  same (needs a second dump; WUX vs extracted folder at least).
- Coverage: the cache from the distilled manifest against the one from the full console manifest (shader count,
  `dksh_cache coverage` against the console's own cache).
- Console: a fresh install starts without the first-start screen; a first tour without black places.

## Updating

The test console records its manifest always (`WWHD_SHADER_MANIFEST`, on by default). Players' consoles record theirs
too, for their own console's background compiler and their own `make_sd.py --shaders`; nothing is asked of them.

After the test console has played (new places, cutscenes, a Prepare graphics sweep), with it mounted over UMS or the
game running with the debug server:

    tools/switch/update_shader_template.sh GAME_DIR

It pulls the private repository's clone (`../SwitchWakerHD-shader-manifest`), takes the console's
`shader_manifest.bin`, and runs `shader_manifest.py distill --base` with the current sealed template: the template's
states stay, the recording adds the shaders the template does not have. Nothing new: it says so and stops. Otherwise
it seals the new template into the clone and updates `provenance.json`; then commit and push the private repository
and update `tools/shadercache/manifest.ref`. Also needed after a change of the shader translation (the Latte
decompiler, the translation keys): distill the recordings again, since one state per shader was chosen with the old
translation. Not needed for uam, deko3d or runtime changes: players compile the states on their own computers.

## Results so far (2026-10-23, Mac, the USA dump, the full-game console manifest)

| | |
|---|---|
| Fetch shaders as semantic attributes | every variant translates to the same GLSL as with the console's microcode (164,243 map lines identical) |
| Distilled template | 164,243 variants -> 8,565 states (2,985 vertex, 5,580 pixel), 131 MiB -> 767 KiB; translated again: the same 8,565 sources; no game bytes; `guard.py` accepts it |
| Sealed | 767,273 bytes; sealed with the USA dump, opens with the European dump to the identical template; deterministic |
| Cache from the sealed template (`make_sd.py --shaders`) | 23,380 shaders (38.9 of the 40 MiB budget), 101 s, byte-identical twice; the full console manifest gives 23,280, 22,736 in both (every recorded variant's shader is in both; the rest are speculative guesses) |
| Console (2026-10-23, a fresh cache folder holding only that cache, `WWHD_DK_SHADER_CACHE_DIR`) | no first-start screen; 23,380 DKSH loaded in 0.8 s; title screen and a warp tour of 8 places (Outset, Windfall, Dragon Roost and its cavern, Forest Haven, Forbidden Woods, Tower of the Gods, Forsaken Fortress): **0 shaders compiled, 0 draws skipped** (1,080 translations to find their DKSH) |
| `distill --base` | the sealed template plus the same recording: adds 0, writes nothing; a template cut to 8,000 states plus the recording: adds the 565 missing, the 8,000 kept as they were; `update_shader_template.sh` over the debug server: 1 min 14 s, nothing new |
| Private repository | `centollOS/SwitchWakerHD-shader-manifest` created; `shader-template.sealed` (SHA-256 e7eaffcc...) at 3fe58156 |

## Steps (one commit each)

1. This plan.
2. `dksh_cache translate`: WWFA fetch and `--map`.
3. `shader_manifest.py distill` + game-bytes scan + tests.
4. `seal` / `open` + tests.
5. `from-template` and `make_sd.py --shaders` taking the sealed file; checks above on the Mac.
6. Release packaging (the private repository: needs the owner's go-ahead) and the release checklist.
7. Installers, docs, console test.

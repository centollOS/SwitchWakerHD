# Wind Waker save tools (GameCube → Wii U HD)

## How to use (players)

Bring your GameCube Wind Waker progress to Wind Waker HD:

1. Export the save from your memory card as a `.gci` file (Dolphin: Tools › Memory Card
   Manager › Export; a real card: GCMM, Swiss or similar). USA (GZLE01), Japanese (GZLJ01), and
   European (PAL, GZLP01) saves are supported.
2. Convert it (Python 3, nothing else needed):

   ```
   python3 tools/savegame/gc2hd.py "My Save.gci" -o converted
   python3 tools/savegame/hd_save_info.py converted      # check: hearts, items, songs, ...
   ```
3. Put `converted/cking.sav` in place. **It replaces all three Quest Logs: back up the old
   `cking.sav` first.**
   - this port: `save/user/cking.sav` (or the folder you pass with `--save`, plus `/user/`);
   - Cemu: `mlc01/usr/save/00050000/10143500/user/80000001/cking.sav` for the USA game
     (title ID 0005000010143500; `80000001` is the first Wii U user). For other regions, use the
     folder of your own copy: Cemu shows the title ID in the game list (right-click → properties).
   Leave the other files there (`cking_pic*.sav` are the Picto Box album, `cking_playlog.sav`
   the play log); HD creates them if they are missing.
4. Start the game: the three GameCube files appear as Quest Logs 1–3 with their names, places and
   progress. Saving in HD then works as usual.

**Japanese saves:** names in Japanese characters cannot be shown by the USA HD font; full-width
Latin letters are converted to normal ones, any other name becomes "Link" (`--name NAME` sets a
name of your choice for all three files).

**Not converted:** GameCube Picto Box pictures (HD has its own album format), the memory card
banner/icon. The **Tingle Tuner** becomes HD's **Tingle Bottle** (same item number and the same
Tingle Island event; `--drop-tingle-tuner` removes it instead). HD-only data (for example its
play statistics) starts as in a new HD file. Files that were empty on the GameCube stay
"New Game".

**European saves:** accented Latin names, including Œ/œ, are decoded from the PAL name-entry
character set. The PAL card-wide language is copied to byte 4 of every HD per-slot
option record. HD still chooses its text pack from the console/port language setting; choose
your display language there.
The same converted file targets USA HD (00050000-10143500) and European HD
(00050000-10143600); no region option is needed.

## Tools

Plain Python 3, no dependencies, no game data. Layouts come from the zeldaret tww decompilation
(GameCube) and this project's verified WWHD decompilation (HD); `wwsave.py` names the exact
functions.

| Tool | Use |
|---|---|
| `gc2hd.py SAVE.gci -o OUTDIR` | GameCube memory-card save (`.gci`, USA GZLE01, Japan GZLJ01 or Europe GZLP01) → `OUTDIR/cking.sav` for HD. `--batch SRC_DIR -o ROOT` converts every `.gci` below `SRC_DIR` into `ROOT/<path>/user/cking.sav`. Options: `--drop-tingle-tuner`, `--name NAME`. |
| `hd_save_info.py PATH` | progress of each file: hearts, rupees, magic, items, sword/shield, arrows/bombs, songs, Triforce shards, pearls, sail (Sail / Swift Sail), islands visited and charted, charts owned, dungeon items and bosses, return stage, time of day, deaths, New Game+ count, save counter. `PATH` = `cking.sav`, a `save/` or `save/user/` folder, or a `.gci`. `--short` (one line per file), `--json`, `--file N`. |
| `hd2gc.py cking.sav -o OUT.gci` | the reverse direction (shared fields only), mainly for the round-trip check. `--set-gc-name` writes the HD name into the GameCube name field, `--template X.gci` copies the banner/icon block. `--code GZLP01` writes PAL; `--pal-language english\|german\|french\|spanish\|italian` chooses its card-wide language (default: recover HD per-slot language; mixed used Quest Logs require an override). |
| `roundtrip_test.py cking.sav ...` | HD → GameCube layout → HD; every shared field must come back bit-identical (GameCube and HD checksums are verified on the way). |
| `wwstate.py info STATE.wwstate` / `wwstate.py to-sav STATE.wwstate -o OUTDIR [--into cking.sav] [--file N]` | a portable save state (`slotN.wwstate`, [docs/portable-save-states.md](../../docs/portable-save-states.md), e.g. from a bug report): check it and show its place and progress, or write its save data as Quest Log N of a `cking.sav`. |
| `wwsave.py repair-medli cking.sav -o OUTDIR [--file N]` | repair an early save whose premature Master Sword ownership hides Medli (#85). Writes a repaired `OUTDIR/cking.sav` and an exact `OUTDIR/cking.sav.before-medli.bak`; never changes the input or overwrites existing output files. Quest Log N defaults to 1. |

Work on copies: none of the tools writes to its input. Use a converted save like any test save:
`cp -R <folder>/user <test>/save/` and start the game with `--save <test>/save`.

## Medli missing on the first Dragon Roost visit (#85)

The old **Master Sword + Mirror Shield** cheat also recorded ownership of every sword upgrade.
The game uses those ownership bits as story milestones: the Earth Temple upgrade removes Medli
from her earlier locations. Turning mods off or restarting does not undo changes already saved.
The corrected cheat equips the weapons without changing ownership and is labelled **until reload**;
loading a Quest Log or portable state restores the equipment earned in that playthrough.

For an affected early save, close the game and run:

```sh
python3 tools/savegame/wwsave.py repair-medli /path/to/cking.sav -o medli-repaired --file 1
python3 tools/savegame/hd_save_info.py medli-repaired/cking.sav --file 1
```

Use the actual Quest Log number (1–3). Keep the source and the generated `.bak`, then replace the
game's `cking.sav` with `medli-repaired/cking.sav` and load that Quest Log normally. The tool restores
the Hero's Sword and its ownership in that Quest Log; it preserves the shield, inventory, event
flags, other progress, and the other Quest Logs. It verifies the save checksums and refuses saves
past the first pearl, New Game+ saves, or saves without the premature sword flags. It does not
guess earned sword progress in later saves.

An old portable state still contains the old flags: do not load it over the repaired save. Make
a fresh portable state after loading the repair. If your only current progress is in a portable
state, first use `wwstate.py to-sav STATE.wwstate -o state-copy --into /path/to/cking.sav`, then repair
`state-copy/cking.sav` as above, choosing the state's Quest Log. Neither step changes its input.

## File formats

**GameCube `.gci`** (`m_Do_MemCardRWmng`): 0x40-byte directory entry, then 12 card blocks of
0x2000. Block 0: banner, icon, comment. Blocks 1 and 2: two copies of `card_savedata`
(save count, data version, three `card_gamedata` of 0x770 = 0x768 bytes of save data + u64
checksum (byte sum, complement sum), u32 checksum of big-endian u16 sums at 0x1FFC). Blocks 3..11:
Picto Box pictures. Read like `mDoMemCdRWm_Restore`: per file the first copy, the second if the
first fails its checksum.

**HD `cking.sav`** (8966 bytes, SaveMgr in `wwhd_src/d/d_menu_save_*`):

| Offset | Size | Content |
|---|---|---|
| 0x0000 | 3 × 0xA94 | per file: 0x768 bytes of save data, zero up to 0xA8C, then u32 byte sum and u32 complement sum over the first 0xA8C bytes |
| 0x1FBC | 3 × 16 | HD per-file "player" data (fresh: byte 5 = 1) |
| 0x1FEC | 3 × 4 | HD per-file "status" (fresh: 0) |
| 0x1FF8 | 3 × 20 | HD per-file "event" (fresh: 0) |
| 0x2034 | 3 × 18 | player name, UTF-16BE, 8 characters + terminator (fresh: "Link") |
| 0x206A | 3 × 220 | HD per-file "map" (fresh: 0) |
| 0x22FE | 4 | format word, always 4 (checked on load) |
| 0x2302 | 4 | CRC-32 of the five HD sections (0x342 bytes); quirk: each name contributes only its first (characters + 1) **bytes**, zero padded to 18 |

The picture album (`cking_pic*.sav`) and `cking_playlog.sav` are separate files; HD creates them
when missing (checked: the game loads a folder holding only `cking.sav`).

## Field mapping GameCube → HD

The 0x768-byte save data has the same packed layout on both systems: HD's
`dSv_info_c::card_to_memory` (025BA7B0) copies the same blocks with the same sizes in the same
order as the GameCube one, and the structures inside have the same sizes. `wwsave.py` writes the
two layouts down independently (442 named fields), checks that they agree, and `gc2hd.py` copies
field by field through them. Group by group:

| Group | Mapping |
|---|---|
| status A (life, rupees, item buttons, equipment, wallet, magic) | copied |
| status B (date, time of day, Wind Waker wind direction) | copied. The GameCube save date (OSTime) is kept; HD overwrites it with its own clock on the next save |
| return place (stage, room, spawn point) | copied |
| items, get-item flags, item record/max (arrows, bombs, bottles), bags (spoils, bait, delivery) | copied, except the Tingle Tuner (below) |
| collect (Triforce, songs, pearls, …), map (charts, sea squares), priest, status C (the four recollection records) | copied |
| info (player names, deaths, puzzle, New Game+ count, salvage point) | copied; **save counter** (+0x10): HD shows a file whose counter is 0 as "New Game" (GameCube leaves the field 0 and uses the name), so a used GameCube file gets 1. Japanese: Shift-JIS bytes in the three name fields are cleared (HD's own files leave them empty) |
| config (furigana, sound, Z-targeting, vibration) | copied (HD sets the sound mode itself on load) |
| dungeon memory (16 stages: chests, switches, items, rooms, keys, map/compass/boss key/boss), ocean, event flags, reserve | copied |
| Tingle Tuner (item 0x21 in slot 7) | HD uses the same item number and slot for the **Tingle Bottle**, given by the same Tingle Island event, so the item is kept and becomes the Tingle Bottle. `--drop-tingle-tuner` clears the slot, its get-item flag and any item button on it instead |
| GameCube pictures | dropped (different format; HD has its own album) |
| HD-only per-file data (player, status, event, map) | the values of a fresh HD file (HD's init 0271FF4C) |
| HD player name (UTF-16) | the GameCube name; Japanese names are folded to ASCII when possible (full-width letters), otherwise "Link" (the USA HD font is Latin only) |

Unused GameCube files (no player name) are copied as they are; HD shows them as "New Game"
(save counter 0) and starting a game re-initialises them. GameCube USA and Japan saves have the
same packed layout as PAL (tww: `d_save.h` only differs for the demo build, event flags have
no region conditionals). See the PAL and HD region details below.

## Validation (2026-10-05)

- Round trip HD → GameCube layout → HD (`roundtrip_test.py`) on the three HD saves of the local
  collection: all 442 shared fields bit-identical in every file; only HD-only data resets to
  fresh values.
- 167 GameCube saves (CryZe/WindWakerSaves: 5 USA, 162 Japan, nine routes) convert without
  errors.
- Game test in the original build (headless, copies of the saves): early, middle and late save
  of every route (26 saves). Every converted save shows its Quest Logs with name and place in
  the HD file select and loads into gameplay. Hearts, rupees, magic, items and place match
  `hd_save_info.py`. No crash. A New Game+ file shows the Triforce mark. The first test found
  the save counter rule (files with counter 0 showed "New Game"); fixed in `gc2hd.py`.

## PAL layout and HD region evidence (2026-10-12)

Checked against [zeldaret/tww at 342d447b](https://github.com/zeldaret/tww/tree/342d447b6b6b3bafe7a0dc42d6b1e36b2de172e0):

- [`d_save.h`](https://github.com/zeldaret/tww/blob/342d447b6b6b3bafe7a0dc42d6b1e36b2de172e0/include/d/d_save.h)
  and [`d_save.cpp`](https://github.com/zeldaret/tww/blob/342d447b6b6b3bafe7a0dc42d6b1e36b2de172e0/src/d/d_save.cpp):
  retail USA/Japan/PAL share `dSv_save_c_PACKED`, 0x768 bytes, and the
  `memory_to_card`/`card_to_memory` packing order. PAL has different initial defaults
  (e.g. config ruby); these are saved values and are copied, not reinitialized.
- [`m_Do_MemCardRWmng.h`](https://github.com/zeldaret/tww/blob/342d447b6b6b3bafe7a0dc42d6b1e36b2de172e0/include/m_Do/m_Do_MemCardRWmng.h)
  and [`.cpp`](https://github.com/zeldaret/tww/blob/342d447b6b6b3bafe7a0dc42d6b1e36b2de172e0/src/m_Do/m_Do_MemCardRWmng.cpp):
  `card_savedata` is 0x2000 bytes with three 0x770-byte slots starting at +8;
  its first u32 (`save_count`) holds PAL language instead of a card save sequence number.
  **This is not the per-slot u16 `info.save_count` at info +0x10.** `Store` writes
  `dComIfGs_getPalLanguage()`; `Restore`/`Restore2` accept 0..4 and clamp larger values to 0.
  `BuildHeader` establishes the order: English, German, French, Spanish, Italian. The
  language is card-wide, read from backup block 1 even when slot data needs block 2.
  The localized memory-card comment differs; directory structure, two save copies,
  data version 0, slot and block checksum algorithms do not. Artwork/comments are not converted.
- [`d_name.cpp`](https://github.com/zeldaret/tww/blob/342d447b6b6b3bafe7a0dc42d6b1e36b2de172e0/src/d/d_name.cpp),
  PAL `str01`/`str02` in `screenSet`: single-byte Western names, including accented Latin
  letters and bytes 0x8C/0x9C for Œ/œ. Use CP1252 for PAL rather than Latin-1 (Japan remains
  Shift-JIS). Names still use the shared 17-byte NUL-terminated fields.

HD evidence: this repository's executable-verified `tools/recomp/builds/eu.json` maps
USA `card_to_memory`/`memory_to_card` (025BA7B0/025BA9FC) to EU 025BA770/025BA9BC
with identical relocation-normalized function bodies. Likewise SaveMgr slot checksums (02721AC4/02721AFC →
02722380/027223B8), readers (027246A0..02724B74 → 02724F5C..02725430), slot writer
(02724C20 → 027254DC), extra-data CRC (027201E4 → 02720AA0), and fresh-data init
(0271FF4C → 02720808) have identical bodies. Both use the file format documented above,
without a title ID or region marker. The verified HD storage routines in
`d_menu_save_storage.cpp` serialize slots, player/status/event/name/map sections, format 4,
and CRC only. `hd_sys_setting.cpp` 025F9448 reads console language into runtime options
(save +0x12F0, byte +4). SaveMgr 02720D34 (EU 027215F0) copies a selected slot's option
bytes 0..8 to those runtime options via 0271FCB4 (EU 02720570); 02721B34 (EU 027223F0)
copies them back on save. Thus byte 4 of each serialized 16-byte `player` record can
preserve PAL language in the same 0..4 order. The console language still selects the text pack.
See also `runtime/src/game_languages.h` and `runtime/src/language_region.cpp`.

Fresh per-slot option defaults are not completely identical: initializer 0271FBE8
(EU 027204A4) stores byte 3 as 0 for USA, 1 for EU, and byte 5 as 1 in both. This is
an option default, not a format/region tag: the shared readers accept either value. The converter
keeps the existing USA defaults for the other HD-only option bytes and maps PAL language at +4.
No target-region flag is needed to make the file load.

PAL output example (override the recovered language if desired):

```sh
python3 tools/savegame/hd2gc.py converted/cking.sav -o back.gci --code GZLP01 --pal-language french --set-gc-name
python3 tools/savegame/pal_test.py
```

The PAL round trip preserves shared progress; a used slot's zero counter becomes 1 for HD,
and its language is copied into HD per-slot options and recovered on export. A PAL card
has one language, so conflicting used HD slots need an explicit `--pal-language` override.
HD-only fields and pictures have the same limitations as USA/Japan conversion. `--template` copies artwork but the requested
`--code` always controls the output directory entry's region.

Validation for this change:

- `python3 tools/savegame/pal_test.py`: six synthetic tests pass, covering all five languages,
  every packed field in three slots, PAL → HD → PAL → HD, CP1252 accents/ligatures,
  empty slots, invalid language fallback, backup recovery, checksum rejection, CLI language
  recovery/override, conflicting slot languages, and a different-region artwork template.
- Existing `repair_medli_test.py` (three tests) and `tools/recomp/test_builds.py` (18 tests) pass.
- `mkbuildmap.py --check tools/recomp/builds/eu.json` against the local USA/EU RPX pair:
  map unchanged; regional save-function equivalence confirmed.
- A Release Metal EU build from this branch loaded a converted PAL-layout test copy into
  Earth Temple gameplay, headless with audio disabled. The timed test completed; the
  in-memory name, current/max hearts and magic, rupees, all inventory bytes, and all collect
  bytes matched the converted file. The source save and converted `cking.sav` remained
  unchanged. This integration input was generated from a copy of a local HD save, exported
  to PAL layout with a zero slot counter, French card language, and an accented player name;
  it was not an original GameCube PAL save. No game/save data is included in the repository.
- Not tested: an original PAL memory-card export, PAL output in Dolphin/on GameCube hardware,
  HD saves in Cemu/on Wii U hardware, in-game saving/reloading, or every gameplay stage/name glyph.
  USA HD compatibility was checked through the executable map and existing converter tests;
  this change's gameplay run used EU HD only.

# Brazilian Portuguese (fan translation) — plan

Branch `feature/ptbr-translation` (from main 225a0de3). Status: in progress.

## Why

Brazilian players play SwitchWakerHD with the Triforce-Heroes PT-BR fan translation
(triforce-heroes.com.br, "Tradução para PT-BR v2025.08.05 by Triforce-Heroes (100.0%)", their
"VERSÃO FINAL (3.0)"). Bundles that copy its files over the game crash after the last pearl (#42:
`loadModel`, bad guest pointer). We want Portuguese to work, without hosting anyone's texts:

* the player downloads the translation from Triforce-Heroes' page and picks the download in the
  installer (like the GameCube minimap: "bring your own");
* the installer rebuilds the 2D pack from the player's **own original** game files, laid out like the
  game's (see "The pack"), so what bundle they got no longer matters;
* the port's own menus and the three installers are translated into PT-BR too (our own texts, our
  translation).

Non-goals: hosting or redistributing the translation (no texts of Triforce-Heroes in the repository,
the releases or the installers); translating the upstream overlay's English-only tabs (Saves, Audio,
Mods, Language headings); other fan translations (the code is generic where it costs nothing).

## What the download is

Their "Emulador" download (RAR, ~32 MB): a Cemu graphic pack folder
`TriforceHeroes_WWHD (3.0) - USA - EUROPA/` with `rules.txt` (titleIds 0005000010143500,
0005000010143600), a Cemu tutorial and `content/Common/Pack/permanent_2d_{UsEnglish,UsSpanish,
EuEnglish,EuSpanish}.pack` — four byte-identical copies of one pack. Their "Wii U" download was not
seen yet (probably the same pack in an SDCafiine layout); anything holding a `permanent_2d_*.pack` with
the checks below passing is accepted.

Compared with the USA v0 `permanent_2d_UsEnglish.pack` (221 Yaz0 SARC entries, every entry at a
0x2000 boundary):

* changed: the 64 `*_msbt.szs`, `CKing_msbp.szs`, the six `*_bffnt.szs` (same sizes, glyphs patched),
  `Text.szs` (its `blyt/Text.bflyt` is the original **cut 2 bytes short**: the last `grp1` section
  declares 0x24 bytes and has 0x22 — a tool artefact, nothing translated);
* every MSBT has the same labels, label indices, attributes and text count as v0 (so "made for the
  updated game" does not matter for these files);
* the archive was repacked: entries 16-byte aligned (original 0x2000), the inner MSBT/MSBP SARCs
  have their data at 0x44/0x50 (original 0x2000), and eight Yaz0 headers carry another alignment
  value (`Text.szs` 0x2000 -> 4, `CKing_msbp.szs` 0 -> 4, the fonts 0 -> 0x2000).

The Android port (Nycolazs/WindWakerHD-Android-Plus, `PackPatch.java`) builds its pack from the
player's own one too (entries 0x100-aligned, original Yaz0 headers kept, the translated `Text.szs`
taken whole).

## The pack (tools/switch/ptbr_pack.py, fork-owned)

`ptbr_pack.py --base <game>/content/Common/Pack/permanent_2d_UsEnglish.pack (or EuEnglish)
--translation <download> --out <file>`:

1. input: the `.rar` (extracted with the system's `bsdtar`/`tar`: macOS and Windows 10+ have it,
   Linux needs libarchive-tools; a clear message asks to extract it by hand otherwise), a folder
   (searched for `permanent_2d_*.pack`), or a `.pack`. Several packs: they must be identical, or the
   English one is taken.
2. checks (refuse with a message naming the reason): big-endian SARC; the same 221 entry names as
   the base; each changed entry is an MSBT archive, the MSBP or a font (a font must keep its size);
   every MSBT keeps the base's labels/indices/attributes/text count. Changed layouts (`Text.szs`) are
   left out: the original is kept (and reported).
3. output: the base archive's header and name tables as they are, the entries in the same order at
   the base's alignment (0x2000); a changed entry is the translation's own Yaz0 data (already
   compressed: the pack stays ~12 MB, like the original, which matters for the game's heap) with the
   **base's** Yaz0 header alignment field; unchanged entries are the base's bytes.
4. a `translation.json` next to it: name/version from `rules.txt` (`path` / `name`), the download's
   file name, the SHA-256 of the input pack and of the output, the entries changed / kept.

Since every MSBT, the MSBP and the fonts are the same in UsEnglish and EuEnglish' layouts (the
translation replaces all of them), the result works for either region; it is made from the region
being installed.

## SD layout and make_sd.py

`make_sd.py --ptbr <download>` (installers: the new option) runs `ptbr_pack.py` after the game
check and writes `sdmc:/switch/wwhd/translation/pt-BR/permanent_2d_ptBR.pack` +
`translation.json`. Without `--ptbr` an existing folder is left alone (it was built from the
player's original files; the Language tab can still pick it). `--no-ptbr` removes it.

## Runtime (runtime/src/fan_translation.{h,cpp}, fork-owned; small marked hooks)

* `present()`: `<game folder's parent>/translation/pt-BR/permanent_2d_ptBR.pack` exists and starts
  with `SARC` (looked up once).
* Language code 9 (Wii U "pt") is Portuguese (Brazil) when present:
  * `game_lang::is_available(9)` true (hook in game_languages.cpp), so the Language tab offers it;
    its label there is "Português (Brasil)" (hook in the tab's loop);
  * `console_language()` (hook in coreinit_misc.cpp): wanted 9 + present -> the game gets 1
    (English: its own code path, region and save options as English) and the translation is active;
  * no saved language and no `WWHD_LANGUAGE` + present -> 9 (the player chose it in the installer);
  * `fs.cpp read_path` (hook after the language-source redirect): while active, a read of
    `permanent_2d_UsEnglish.pack` / `permanent_2d_EuEnglish.pack` (any case, any folder) is served
    from the translation's pack; content mods still come first; writers are never redirected;
  * the Language tab's "applies on the next start" logic treats 9 as the start's language.
* `ui_text` (Switch): language 9 with the translation present -> new `kPortuguese` / `kMenuPortuguese`
  tables (every field; the start-up text console has no ã/õ, `to_console` maps them to a/o).
* A desktop build gets the same behaviour if someone puts the folder next to its game folder.

## Installers (Windows WPF, macOS + Linux Avalonia)

* `Str`: a third language. `Str.Lang` (Es, En, Pt) replaces `Str.Es`; a PT-BR table
  (`installer/shared/StringsPt.cs`, and PT entries for the Mac/Linux/Desktop overrides); a key missing
  in PT falls back to English. Starts in PT when the system language is Portuguese; the welcome page's
  language button cycles Español -> English -> Português.
* Game page: a new optional card like the minimap's: "Tradução para português (Brasil) (opcional)",
  a line that it is Triforce-Heroes' fan translation, downloaded by the player from their page (link
  to https://triforce-heroes.com.br/traducoes/the-legend-of-zelda-the-wind-waker-hd, "the
  **Emulador** download"), a button to pick the `.rar` (or the extracted folder), remove. Remembered
  like the minimap's choice. Install passes `--ptbr <path>`; the summary line names it.
* The done page / log show `ptbr_pack.py`'s result; a refusal stops the install with its message
  (the player picked the wrong file) before the slow steps (checked first, like `guest_mods --check`).

## Steps (one commit each)

1. this plan;
2. `ptbr_pack.py` + `test_ptbr_pack.py` (synthetic SARCs; and against the real download when
   `WWHD_TEST_PTBR` points at it);
3. `make_sd.py --ptbr/--no-ptbr`, early check;
4. runtime `fan_translation` + hooks + a CMake test (`fan_translation_test`: redirect, language 9);
5. `ui_text` PT-BR tables;
6. installer `Str` three languages + PT-BR tables;
7. installer option card (Windows, desktop) + engine `--ptbr`;
8. builds: Mac `.app`, Linux tar.gz, Windows `.exe`; Mac install run with the real download;
9. console (ask first): the #42 statue state with the rebuilt pack (and the raw pack, A/B), title,
   file select, dialogues, menus in PT.

## Open

* Console result (2026-11-02, handheld, USA): the title, file select and Minus menus show in
  Portuguese, and the #42 statue state passes the last pearl scene with the rebuilt pack AND with
  Triforce-Heroes' pack as downloaded (served through the same redirect). So the translation itself
  does not crash on this build; #42's crash came from something else in the bundles that reporters
  used (other files copied over the game, or the older port build). The rebuilt layout stays (it is
  the game's own), but it is not the #42 fix.
* The Wii U download's layout (accepted if it holds the pack).

# In-game button prompts that follow your controller

With **Controls → Face buttons** set to **by label** (or **Automatic** on an Xbox-style or
PlayStation pad), the game's own button prompts match the controller in your hands:

- **Xbox look:** dialog icons and the HUD buttons use **green A, red B, blue X, yellow Y**, and the
  HUD's four-button cluster follows the Xbox layout (**A bottom, B right, X left, Y top**). Whole
  buttons move, with their item art, text and A's parry effect.
- **PlayStation look:** the same positions, with **✕ ○ □ △** shapes in place of A, B, X, Y (Cross is
  A, Circle B, Square X, Triangle Y, as by label binds them).
- **Shoulder buttons:** prompts read **LB / RB / LT / RT** with the Xbox look and **L1 / R1 / L2 / R2**
  with the PlayStation look, in the game's own HUD lettering.

The letters themselves never change: *by label* already makes the Xbox A button do the game's A, so
a prompt that says A still means the button printed A. **By position** and **custom** keep the game's
original look, since the game's prompts then already match the buttons' positions. Switching takes
effect immediately, without a restart, and also applies to text the game had already drawn. Nothing
changes while you have never chosen a different look in a session.

## Options

In the settings overlay's **Controls** tab (and the Controls window on macOS), saved in
`controls.json`:

| Option | Choices | Default |
|---|---|---|
| **Button prompts** | Automatic, Original, Xbox, PlayStation | Automatic: PlayStation shapes on a PlayStation controller, the Xbox look on any other |
| **Shoulder names** | Automatic, L / R / ZL / ZR, L1 / R1 / L2 / R2, LB / RB / LT / RT | Automatic: follows the button prompts |
| **Coloured face buttons** | on, off | on; off keeps the game's white buttons (in the Xbox positions with by label) |

The button prompts only change while the face buttons are by label. A fixed **Shoulder names**
choice applies with any face-buttons setting, since the shoulder buttons don't move.

No Nintendo, Microsoft or Sony artwork or font is shipped: the colours are applied while the game
draws, the shoulder captions are composed at runtime from your own copy's HUD font, and the
PlayStation shapes are simple geometry (two strokes, a ring, a square, a triangle) drawn by the
port.

## How it works (technical notes)

Three function hooks (`tools/recomp/hooks_glyphs.txt`), plus wrappers around the existing aspect
(`0x028766CC`) and RTL text (`0x028785C8`) hooks:

- `0x0286E9F0` (`CharWriter::Print`) marks button glyphs (A/B/X/Y: U+E000–E003; L/R/ZL/ZR:
  U+E083–E086) in unused bits of the cached glyph quad's byte `+0x2F`.
- `0x028F7C7C` (TextBox vertex generation) tints marked quads and swaps shoulder captions and
  PlayStation shapes to the composed atlas for the vertex build, then restores everything it changed.
- `0x02874D54` (`Picture::DrawSelf`) tints the HUD button backgrounds (`P_A_00`, `P_B_00`, `P_X_00`,
  `P_Y_00`, `P_ASpecial_00`) and restores their authored vertex colours afterwards.
- `0x028785C8` (`lyt::TextBox::DrawSelf`, the existing RTL wrapper) also recognises
  `CommandA_00`'s floating action hint: `T_A_00` and `P_A_00` are **siblings** under
  `N_Center_01`, unlike the cluster's parent/child pair. The same check supports B/X/Y
  with matching sibling pictures and a single matching letter. `T_A_01` on
  `P_ASpecial_00` covers the special-A prompt in both command layouts. These are plain
  `CKingMain` letters, so `CharWriter::Print` does not mark them as U+E000 icons.
  The scoped face identity feeds the existing `0x028F7C7C` vertex hook (bare PS shape,
  coloured RGBA or plain coverage); Xbox colours still use `Picture::DrawSelf`.
  The HD screen uses `hdParts_c::draw` (`0x026F8B54`) → `drawLayout`
  (`0x026F8D08`) → the NW4F layout traversal → `TextBox::DrawSelf`.
  No new recompiler hook is needed. The wrapper returns directly to the original draw
  before pane recognition while the session gate is closed.
- The text-box draw wrapper recognises the HUD's shoulder captions (`T_L_00` … `T_ZR_00`) and face
  letters (`T_A_00` … `T_Y_00` on their `P_?_00` pictures), which the game prints as plain text.
- The pane-matrix hook moves the complete CommandGuide cluster (`N_All_00`) for the Xbox positions;
  only the computed global matrices keep the new layout, the authored locals are restored.

**Untouched until used:** none of the hooks write to game memory until a different look has been
selected once in the session; the first activation rebuilds each cached text box once so its glyphs
get marked. A loaded save state, or a change from one look to another (colours, shapes, shoulder
names), clears that rebuild list. The look is resolved once per controls change or new controller
family (`input_map::prompt_style`), so drawing costs nothing extra per frame.

**Why the marker bits are safe:** each cached glyph quad is 0x30 bytes, and its last word (`+0x2C`) is
only touched by three functions in the game code: the emitter `0x0286E4FC` writes the 16-bit value at
`+0x2C` and the page byte at `+0x2E`, stores zero to `+0x2F` and then sets only bit 0 (a coverage
glyph of an alpha font, which takes the text colours; the icon font's RGBA glyphs have it clear);
batch grouping `0x028F7A4C` reads `+0x2F` masked with `& 1`; vertex generation `0x028F7C7C` reads
`+0x2C`/`+0x2E` and never `+0x2F`. Bits 1–7 of `+0x2F` are therefore free, and every new build of a
quad clears them.

**Shoulder captions and shapes:** the characters L, R, 1, 2, B, T are read from the player's
`CKingMain` font through the game's `Font::GetGlyph`; its BC4 (or plain alpha) sheets are decoded on
the CPU and composed into a small host-side atlas (128×1024, keyed by font) next to the PlayStation
shapes (`button_glyph_art.h`). If the font isn't available, the original labels and letters are kept.
The atlas is an RGBA sheet like the game's icon font: its sheet byte `+0x0E` and the quads' bit 0
stay clear. With them set, the batch (`0x028F7A4C`) takes the text-colour path, which maps the
texture through the text box's colours: a dialog's white text then drew the framed label as a white
box (#141). Two kinds of HUD quad use a coverage twin of the same sheet (`+0x100`, sheet byte set)
and keep bit 0 set instead: the plain PlayStation shapes and the bare L1/R1/L2/R2 (LB/RB/LT/RT)
captions on the HUD's shoulder plates. The game then draws them in the colours of the letter they
replace, including the dimming of an unassigned button; as RGBA pictures the white captions were
unreadable on the white shoulder plates (#143).

The hook behaviour is covered by `runtime/tools/button_glyphs_test.cpp` (synthetic guest memory: face
and shoulder marking, colour restoration, live switches, the untouched-until-used gate, *Automatic*,
HUD positions including 21:9, font fallback, every option, the PlayStation shapes, CommandA sibling letters for all
four faces in coloured/plain PS and Xbox/Original looks, and special A);
`runtime/tools/input_map_test.cpp` covers the option resolution and the `controls.json` keys
(`button_prompts`, `shoulder_names`, `button_colours`). The EU build maps all three hooks. After a change to the
hook list, regenerate the game code (setup does this on update).

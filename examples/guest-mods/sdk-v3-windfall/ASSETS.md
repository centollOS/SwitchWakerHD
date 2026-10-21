# Original assets

`assets/carved_post.glb` and its reproducible source `assets/make_carved_post.py`
are original geometric artwork by Lukas S, licensed MPL-2.0. The painted wood
texture is generated from geometric patterns by the same source. The model uses
metres, with its origin at the centre of the foot and positive Y upward.

`assets/tide_sprite.glb` and `assets/make_tide_sprite.py` are original geometric
artwork, rig and animation by Lukas S, licensed MPL-2.0. The source generates the
painted texture and three-joint rig, with idle, walk, talk and wave clips. The
model uses metres, positive Y upward, and at most two influences per vertex.

No game models, textures, materials, shaders or other game resources are included.
The player-local converter obtains its rendering template from the player's game.

The manifest places the tide sprite separately from the carved post. Its `body`
bone is an ancestor of `head`; the bounded look-at settings use both. The example
loops along a short ground-level path and its talk action requests `tide-story`,
whose smooth camera and two forward choices select distinct mod-owned end flags.
This data-only package has no guest behaviour or music hook. The new scene
examples remain preview acceptance cases until verified in the running game.

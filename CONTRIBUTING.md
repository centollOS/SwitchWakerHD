# Contributing

## License of contributions

Contributions are accepted under the project's license, the Mozilla Public License 2.0
([LICENSE](LICENSE)), the license of the upstream project this repository forks, except:

- Vendored third-party code under `runtime/third_party/` (Cemu, uam, {fmt}, Dear ImGui, xxHash,
  metal-cpp): changes stay under that component's license ([THIRD_PARTY.md](THIRD_PARTY.md)).
  Record changes to uam in `runtime/third_party/uam/PATCHES.md`.
- Patches to third-party projects (`tools/switch/forwarder/nx-hbloader-forwarder.patch`), which
  follow the license of the project they patch.

By contributing you agree to this for every commit you submit.

## Developer Certificate of Origin

Every commit must be signed off (`git commit -s`), which adds a line

    Signed-off-by: Your Name <you@example.com>

and certifies the [Developer Certificate of Origin 1.1](https://developercertificate.org/):

> By making a contribution to this project, I certify that:
>
> (a) The contribution was created in whole or in part by me and I have the right to submit it
> under the open source license indicated in the file; or
>
> (b) The contribution is based upon previous work that, to the best of my knowledge, is covered
> under an appropriate open source license and I have the right under that license to submit that
> work with modifications, whether created in whole or in part by me, under the same open source
> license (unless I am permitted to submit under a different license), as indicated in the file; or
>
> (c) The contribution was provided directly to me by some other person who certified (a), (b) or
> (c) and I have not modified it.
>
> (d) I understand and agree that this project and the contribution are public and that a record of
> the contribution (including all personal information I submit with it, including my sign-off) is
> maintained indefinitely and may be redistributed consistent with this project or the open source
> license(s) involved.

## No game data

Never commit anything derived from the game or the console: disc images, keys, the extracted game
(`game/`, `Rom/`), the recompiled code (`build/gen/`), shaders or shader caches made from it
(`shadercache_*.bin`, `shaderfail_*.glsl`, DKSH files), frame captures, screenshots, save files,
logs with game content, or builds (`.nro`, `.nsp`, `wwhd`). Generated files stay under `build/`,
which git ignores; keep your dumps outside the repository or in the ignored folders.

## Checks

Before sending a change: build the NRO with `tools/switch/build.sh`; for changes outside the
Switch-only code (`runtime/src/gfx/deko/`, `runtime/src/platform/*_switch.cpp`, `tools/switch/`),
also build the desktop target they touch (the Mac app, or Linux / Windows with Vulkan; see
[docs/upstream-README.md](docs/upstream-README.md)), since upstream's platforms must keep working.
Changes to the renderer or the clocks need a play test on a Switch; say what you tested and on
which profile.

## Names

In the project's own files, names and commit messages, refer to the game as "the game" (or by its
title ID, `00050000-10143500`), never by its title or by trademarks of its publisher. The game's own
identifiers (file, function and stage names the code has to use) are fine where the code needs
them, and so are names inherited from upstream (`WWHD_` options, `wwhd` files and binaries) and the
upstream README kept in `docs/`. Changes that should also go upstream are welcome there too.

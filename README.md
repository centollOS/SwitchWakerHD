# OunceWakerHD

A recomp of The Legend of Zelda: The Wind Waker HD to the Nintendo Switch 2.

OunceWakerHD is a fork of [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD), itself a 
fork of [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp).

## Getting Started

You need:
- A legally obtained US or EU copy of the game
- drako3d (alongside uam2 and drakotools)
- libounce

## Building and installation

  ```sh
  tools/switch/build.sh --switch2
  ```

  ```sh
  cmake -S tools/switch/dksh_cache -B build/dksh_cache-uam2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DDKSH_UAM2=ON
  ninja -C build/dksh_cache-uam2 dksh_cache
  build/dksh_cache-uam2/dksh_cache build <shadercache_gl.bin> build/shadercache_dksh.bin
  ```

  Copy the result to `sdmc:/switch/wwhd/shadercache_dksh.bin`.

  Put `build/switch2-dk/wwhd.nro` in `sdmc:/switch/wwhd/` along with the game files and the shader cache.

## Options

Hold '-' for half a second while playing to open the options menu. This holds enhancements made to the recomp.
Holding still over an option shows a description of it and whether it persists or not.

## Acknowledgements
OunceWakerHD would not exist without these projects:

- **[SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD)**: Initial Switch port
- **[ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp)** (Lukas S and its
  contributors: Sean13128, rhemfur, resadent, arcadematicas and others): the static recompiler, the
  Cafe OS and GX2 runtime, the Metal and Vulkan renderers, the settings overlay, the mods and the
  tools. Almost everything outside the Switch layer is theirs.
- **[Cemu](https://github.com/cemu-project/Cemu)**: its GPU address library and shader decompiler
  (vendored), whose GLSL the deko3d renderer compiles; parts of the OS layer follow it.
- **[zeldaret/tww](https://github.com/zeldaret/tww)**: the GameCube decompilation the function
  names and the 60 fps work are matched against.
- **[devkitPro](https://devkitpro.org)**, **[libnx](https://github.com/switchbrew/libnx)**,
  **[deko3d](https://github.com/devkitPro/deko3d)** and **[uam](https://github.com/devkitPro/uam)**
  (fincs and the devkitPro team): the Switch toolchain, the GPU API and its shader compiler, which
  this port runs on the console too.
- **[nx-hbloader](https://github.com/switchbrew/nx-hbloader)**,
  **[hacBrewPack](https://github.com/TooTallNate/hacBrewPack)** and
  **[hactool](https://github.com/SciresM/hactool)**: the HOME-menu forwarder.
- **[Minimap HD](https://github.com/Chironeyes/The.Wind.Waker.HD-Minimap.HD)** by @Chironeyes
  ([GameBanana](https://gamebanana.com/mods/727687)): the Cemu mod the HD minimap is based on, through upstream's
  `hd-minimap` code mod. Its context rules, timings and floor logic, and its frame, wind arrow and shortcut icons
  (MIT), are his work. Thank you, @Chironeyes, for the mod and for releasing it under a licence that lets it reach
  the Switch.
- **[SaltyNX](https://github.com/masagrator/SaltyNX)** and
  **[Status Monitor Overlay](https://github.com/masagrator/Status-Monitor-Overlay)** (masagrator):
  the shared-memory block this port fills so the overlay shows its frame rate and resolutions.

Ideas and findings from other Switch ports:

- **[NFSMW-NX](https://github.com/StevensND/nfsmw-nx)** (Stevens): a sampling profiler turned on
  by a file on the SD card that records what the threads did in the slow frames
  ([docs/hitch-profiler-plan.md](docs/hitch-profiler-plan.md)), and writing SaltyNX's block from a
  homebrew program ([docs/saltynx-plan.md](docs/saltynx-plan.md)).
- **[renut-nx](https://github.com/ChanseyIsTheBest/renut-nx)** (ChanseyIsTheBest): core 3 through
  the forwarder's NPDM ([docs/core3-plan.md](docs/core3-plan.md)), and the cost of calling
  `mallinfo` on a fragmented heap. Its sister ports
  **[bejeweled3-nx](https://github.com/ChanseyIsTheBest/bejeweled3-nx)** and
  **[sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx)**: inlining the hottest leaf functions
  (#63) and the profiling ideas in #64-#69.

If you enjoy this port, the projects above are where the credit belongs. Every component and its
license: [THIRD_PARTY.md](THIRD_PARTY.md).

Note that while the port to Switch 2 was done by a human, the initial Switch port (and the initial recomp)
were entirely written by AI. I don't condone AI use (and I do feel the result would be much better done by
a human), but I just needed a test for drako3d that didn't need jit-compiled shaders and this worked. Maybe
one day I'll come back to this and make it run better.

## License

- The project's code is licensed under the Mozilla Public License 2.0 ([LICENSE](LICENSE)),
- Vendored third-party code keeps its own license (Cemu MPL-2.0, uam zlib / MIT, {fmt} MIT,
  Dear ImGui MIT, xxHash BSD-2-Clause, metal-cpp Apache-2.0): [THIRD_PARTY.md](THIRD_PARTY.md).
- Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).

## Disclaimer

This is an independent project that does not claim to have any affiliation to Nintendo. No
copyrighted assets are included in this repo.

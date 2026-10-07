# Switch Mesa with a persistent shader cache

devkitPro's `switch-mesa` 20.1 has no disk shader cache and offers no program binaries, so every
program in `shadercache_gl.bin` was compiled and linked again at every start (171-261 s with a full
cache). `build_mesa.sh` rebuilds the same package (devkitPro's recipe, pinned) with the patches in
`patches/`, which come from centollOS (the GameCube Wind Waker port), where they were measured on
hardware: a cached program loads in 11-13 ms instead of 144-209 ms.

- `0001` newlib `timespec_get` clash (toolchain only)
- `0002` compile counters and timers (`include/mesa_switch.h`, read by `runtime/src/platform/mesa_cache_switch.cpp`)
- `0003` Mesa's disk cache on Horizon: one file, `$MESA_SHADER_CACHE_DIR/mesa_shader_cache.bin` (+ `.idx`, `.use`)
- `0004` nvc0's machine code through that cache

Build once (needs podman or docker; the image is built from `Containerfile` the first time):

```sh
WWHD_JOBS=6 tools/switch/mesa/build_mesa.sh   # -> build/switch-mesa/prefix
WWHD_JOBS=5 tools/switch/build.sh              # links it when it exists (WWHD_STOCK_MESA=1: devkitPro's)
```

On the console the cache lives in `sdmc:/switch/wwhd/cache/`. `env.txt`: `WWHD_MESA_CACHE=0` turns it
off, `WWHD_MESA_CACHE=reset` deletes it at start. The log shows `[mesa] shader cache: ...` (file,
entries, open time) and `[mesa] shader compile: ...` (compiles, links from the cache, nvc0 hits)
every 5 s while they change. The first start after installing fills the cache (as slow as before);
the next starts load from it.

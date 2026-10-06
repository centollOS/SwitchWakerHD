# xxHash

Unmodified `xxhash.h` and `LICENSE` from
https://github.com/Cyan4973/xxHash/tree/v0.8.3 (BSD-2-Clause).

Header SHA256: `17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.

The Vulkan renderer uses header-only XXH3 for full guest texture content checks.
These hashes are transient surface-cache state; persistent shader and pipeline
cache formats are unchanged. Every byte and mip remains included in full checks.

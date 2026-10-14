The AppImage runtime
====================

The first part of the Wind Waker HD .AppImage file (the program that mounts or extracts the image
and starts it) is the AppImage project's type2-runtime, used unchanged:

  type2-runtime, commit 8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa (its "continuous" build of
  2026-09-28), files runtime-x86_64 and runtime-aarch64
  https://github.com/AppImage/type2-runtime/tree/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa

It is one statically linked program that contains these components (versions from its build
scripts, scripts/common/install-dependencies.sh and scripts/chroot/build.sh at that commit, and
from the version strings in the binaries; the Alpine packages are those of Alpine Linux 3.21):

  Component                 Version      License         File
  type2-runtime             8f39b89      MIT             AppImage-runtime-type2-runtime-MIT.txt
  libfuse (library)         3.15.0 (1)   LGPL-2.1        AppImage-runtime-libfuse-LGPL-2.1.txt
  squashfuse                0.5.2        BSD-2-Clause    AppImage-runtime-squashfuse-BSD-2-Clause.txt
  Zstandard (libzstd)       1.5.6        BSD-3-Clause    AppImage-runtime-zstd-BSD-3-Clause.txt
                                         (zstd is dual-licensed BSD-3-Clause or GPL-2.0; used
                                          here under BSD-3-Clause)
  zlib                      1.3.2        Zlib            AppImage-runtime-zlib-Zlib.txt
  mimalloc                  2.1.7        MIT             AppImage-runtime-mimalloc-MIT.txt
  musl libc                 1.2.5        MIT             AppImage-runtime-musl-MIT.txt

(1) with the patch patches/libfuse/mount.c.diff of the type2-runtime tree above.

Corresponding source (also for LGPL-2.1 section 6: everything needed to rebuild the runtime, and
to relink it with a modified libfuse):
  - type2-runtime, the runtime's own source, build scripts and the libfuse patch:
    https://github.com/AppImage/type2-runtime/archive/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa.tar.gz
  - libfuse 3.15.0:
    https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz
    (SHA-256 70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0)
  - squashfuse 0.5.2: https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz
    (SHA-256 db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3)
  - zstd, zlib, mimalloc and musl: the Alpine Linux 3.21 source packages (zstd, zlib, mimalloc2,
    musl), https://gitlab.alpinelinux.org/alpine/aports/-/tree/3.21-stable
The runtime files themselves (the exact bytes in this AppImage) are kept at
https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/releases/tag/appimage-runtime-8f39b89
and in the AppImage project's release https://github.com/AppImage/type2-runtime/releases.
To rebuild: scripts/chroot/chroot_build.sh or scripts/docker/ in the type2-runtime tree.

The runtime also prints a short version of this list: run the AppImage with --appimage-help.

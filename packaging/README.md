# Debian package

`build-deb.sh` builds one **arm64** package, `gpu-bridge`, that installs both halves:

| file | what |
|---|---|
| `/usr/libexec/gpu-bridge/gbd` | 64-bit server; the client library starts it on demand |
| `/usr/lib/arm-linux-gnueabihf/libGL.so.1.7.0` | 32-bit (armhf) client library |
| `libGL.so.1`, `libEGL.so.1`, `libGLESv2.so.2`, `libGLESv1_CM.so.1`, `libGLX.so.0`, `libOpenGL.so.0` | symlinks to it, in the same directory |

An arm64 package is allowed to ship files into the armhf library directory, so the package needs no
multiarch setup. It does not depend on `libc6:armhf`: if 32-bit programs are installed, they use the
bridge; if not, the armhf files are inert.

## Build

On the target, after `~/gb/build.sh` has produced the armhf and arm64 builds:

    packaging/build-deb.sh ~/gb/build32 ~/gb/build64 /tmp/debs

(`GB_VERSION=...` sets the version; `STRIP32` / `STRIP64` choose the strip tools.)

## Install / remove

    sudo dpkg -i /tmp/debs/gpu-bridge_*_arm64.deb
    sudo dpkg -r  gpu-bridge

32-bit programs then run with no `LD_LIBRARY_PATH`, e.g. `glxinfo.arm-linux-gnueabihf -B` reports the
NVIDIA renderer. 64-bit programs are untouched.

## How the armhf libglvnd files are replaced

`preinst` uses `dpkg-divert --rename` on the six names above **and on the versioned files they point
to** (`libEGL.so.1.1.0`, ...) and moves them to `/usr/lib/arm-linux-gnueabihf/gpu-bridge-orig/`.
`postrm` removes every diversion the package owns, which moves the originals back.

Two details that matter:

- The originals go into a subdirectory because `ldconfig` reads every `*.so*` file in the library
  directory and would otherwise re-point the SONAME links at them (a `.gpu-bridge-orig` suffix is not enough).
- The versioned files are diverted too, for the same reason. If libglvnd is upgraded to a new
  version number (a new `libEGL.so.1.x.y`), `ldconfig` will prefer that file until the package is
  reinstalled.

Tested on L4T R32.3.1 (Ubuntu 24.04 userland): install, reinstall over itself, remove; after removal
`dpkg -V` is clean and the armhf libglvnd files are back.

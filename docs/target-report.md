# Phase 0 report: the Tegra target

Measured on the real device over SSH. This answers the `[VERIFY]` questions in
[ARCHITECTURE.md](../ARCHITECTURE.md) section 3. Raw outputs were captured
interactively; the commands are listed so they can be re-run.

## The machine

| | |
|---|---|
| SoC / GPU | Tegra X1 (t210, Maxwell), 4x Cortex-A57, `NVIDIA Tegra X1 (nvgpu)/integrated` |
| Board | Nintendo Switch class (`t210ref`), Switchroot Ubuntu |
| L4T | **R32.3.1** (`/etc/nv_tegra_release`), NVIDIA driver `32.3.1` (Dec 2019) |
| Kernel | `4.9.140-l4t`, aarch64, `CONFIG_COMPAT=y` |
| Userland | **Ubuntu 24.04.5** (glibc 2.39, gcc 13, Xorg ABI 24.1) |
| RAM / disk | 4 GB / 21 GB free |
| Display | **X11 only**: SDDM -> `startplasma-x11`; Xorg loads `nvidia_drv.so` 32.3.1 |
| Native GL | OpenGL **4.6.0** (core + compat), ES **3.2**, GLX **1.4**, direct rendering |
| X extensions | GLX, DRI2, Present, Composite, NV-GLX, NV-CONTROL (**no DRI3**) |

A 2019 NVIDIA driver on a 2024 distribution: nothing about the userland is
"L4T-typical", which is exactly why this was measured instead of assumed.

## Answers to the open questions

| # | Question | Answer |
|---|---|---|
| Q1 | Release / kernel / GPU | see table above |
| Q2 | Display server | **X11 only** (confirmed on the device) |
| Q3 | Can the kernel run AArch32? armhf installable? | **Yes.** `lscpu` op-mode "32-bit, 64-bit", `CONFIG_COMPAT=y`, `armhf` already a foreign arch with 179 armhf packages (including `box86-tegrax1:armhf`). Real armhf processes run the bridge client |
| Q4 | Export a renderable buffer as dma-buf | **Not needed so far.** Strategy X presents through the driver's own X path, so no buffer ever crosses processes. Not yet examined |
| Q5 | Import dma-buf into GL | Not needed so far; not yet examined |
| Q6 | mmap of a Tegra dma-buf from AArch32 | Not examined (matters only for the future mapped-buffer / zero-copy work) |
| Q7 | Fences / explicit sync | Not examined (swap is a synchronous call and the X path orders presentation) |
| Q8 | Can a second X connection render into a window another connection created? | **Yes, for both GLX and EGL, on NVIDIA's own driver.** `glx_xconn` mode 1 renders and reads back correctly; the full `x11_b32`/`glx_b32` tests pass. This was the central bet of Strategy X |
| Q9 | Extension surface | native compat profile: 374 GL extensions; through the bridge: 297 (see below) |
| Q10 | Per-call cost | see below |

## What was verified end to end on the device

`ctest` in the aarch64 build with `GB_PEER32_BUILD` pointing at the armhf
build, on the live display (`GB_TEST_DISPLAY=:0`): **13/13 pass**.

| Test | What it runs |
|---|---|
| `ring64`, `ring32` | ring buffer, as AArch64 and AArch32 processes |
| `xabi_*`, `chan_xabi` | an AArch32 and an AArch64 process sharing a memfd ring / control channel |
| `smoke_native` / `_b64` / `_b32` | EGL + GLES2 offscreen, pixel verified |
| `x11_native` / `_b64` / `_b32` | EGL window surface on an X11 window owned by the client |
| `glx_native` / `_b64` / `_b32` | GLX + compat GL + 3.3 core, pixel verified |

The `_b32` variants are ARM EABI5 binaries (`file`: "ELF 32-bit LSB pie
executable, ARM, EABI5") rendering on `NVIDIA Tegra X1 (nvgpu)` via the 64-bit
`gbd`. Visual demos (`gears_b32` GLX, `demo_b32` EGL) hold a steady 60 fps.

## Performance on the Tegra X1 (best of 3)

| Case | native | bridge 64->64 | bridge 32->64 |
|---|---|---|---|
| async `glUniform4f` | 63 ns | 224 ns | 227 ns |
| sync round-trip (`glGetError`) | 43 ns (cached) | 623 ns | 649 ns |
| `glDrawArrays`, VBO | 6018 ns | 6025 ns | 6027 ns |
| `glDrawArrays`, client array | 6026 ns | 11807 ns | 11381 ns |
| `glReadPixels` 256x256 | 169 us (1.55 GB/s) | 1250 us (210 MB/s) | 1233 us (213 MB/s) |

* **Draws cost the same as native**: the nvgpu driver itself is the limit
  (~166k draws/s), and the client enqueues far faster than that.
* Async calls are ~3.5x a native call; still ~4.4M calls/s.
* Sync round trips are ~0.65 us (cross-core wake). On x86 they were ~0.18 us;
  the A-57 and its slower atomics/fences show.
* Client-memory vertex arrays cost ~2x a VBO draw (upload per draw).
* **Readback is the weak spot: ~7x slower**, because a 256 KiB reply exceeds
  the single-record limit and takes the chunked path with several full copies.
  A 720p frame readback would be ~17 ms. A shared arena / dma-buf mapping is
  the fix (Phase 2).

## Extension surface (Q9)

Compat-profile GL extensions: **374 native, 297 through the bridge**. The
bridge hides any extension with an unimplemented entry point so that apps never
`dlsym` a NULL. Dropped, by prefix: 51 `NV_*`, 11 `EXT_*`, 7 `ARB_*`,
2 `NVX_*`, and a few vendor extras. The ARB/KHR/EXT ones that matter:

`GL_ARB_compatibility`, `GL_ARB_debug_output`, `GL_ARB_direct_state_access`,
`GL_ARB_imaging`, `GL_ARB_map_buffer_range`, `GL_ARB_uniform_buffer_object`,
`GL_ARB_vertex_buffer_object`, `GL_EXT_direct_state_access`,
`GL_EXT_secondary_color`, `GL_EXT_fog_coord`, `GL_KHR_debug`, ...

Most of those are missing for the same few reasons: **mapped buffers**
(`glMapBuffer*`, `glMapBufferRange`, named variants), **debug callbacks**,
and a couple of string-array queries. Closing the mapped-buffer gap restores
the bulk of them. Versions are reported as the driver's own (4.6 / ES 3.2), so
applications that gate on the version string rather than extension strings can
still hit the unimplemented entry points; those return a GL error (logged with
`GB_LOG_MISSING=1`) rather than crashing.

## Findings that changed the code

1. **NVIDIA's legacy GL context reports `GL_CONTEXT_PROFILE_MASK = 0`.** The
   core-profile detector treated "no compatibility bit" as core and rejected
   client-memory arrays. Core is now "core bit set and compat bit clear".
2. **The x86 dev loop had hidden a missing `#include <stdio.h>`**
   (`snprintf` in `gb_server_sizes.c`); aarch64's GCC flagged it.
3. The build needed a **32-bit-only mode** (`-DCMAKE_C_COMPILER=
   arm-linux-gnueabihf-gcc`) because the Tegra has no `-m32`; the 64-bit
   build pairs with it through `GB_PEER32_BUILD`.

## Build and run on the device

```
# once: cross toolchain (the headers are already there)
sudo apt-get install gcc-arm-linux-gnueabihf libc6-dev-armhf-cross

cmake -S src -B build64 -G Ninja                      # server + 64-bit side
cmake -S src -B build32 -G Ninja \
      -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
      -DGB_SERVER_PATH=$PWD/build64/gbd               # armhf client side
ninja -C build64 && ninja -C build32
cmake -S src -B build64 -DGB_PEER32_BUILD=$PWD/build32 -DGB_TEST_DISPLAY=:0
DISPLAY=:0 XAUTHORITY=~/.Xauthority ctest --test-dir build64
```

Using the bridge from an armhf application (no system changes):

```
GB_SERVER=$PWD/build64/gbd \
LD_LIBRARY_PATH=$PWD/build32/shim32 ./some-armhf-gl-app     # GLX / desktop GL
# EGL/GLES apps: link or LD_PRELOAD build32/libgb32_gl.so
```

The armhf side currently has only Mesa's software GLVND stack, so 32-bit GL
applications run on llvmpipe today; with the shim they run on the GPU.

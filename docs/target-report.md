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
| Q9 | Extension surface | desktop GL: 370 native, 362 through the bridge; ES 3.2: 193 native, 190 bridged; ES 1.1: 35 / 35; EGL display extensions: 56 native, 49 bridged (see below) |
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

Measured with a small EGL/GL program run natively (arm64) and through the
bridge (armhf), same display, `GL_EXTENSIONS` / `EGL_EXTENSIONS` compared by
name. The bridge hides any extension with an unimplemented entry point (so
apps never `dlsym` a NULL) and extensions the registry does not list for the
APIs it serves.

| List | native | bridge | native-only | bridge-only |
|---|---|---|---|---|
| desktop GL (compat 4.6) | 370 | 362 | 8 | 0 |
| OpenGL ES 3.2 | 193 | 190 | 3 | 0 |
| OpenGL ES 1.1 | 35 | 35 | 0 | 0 |
| EGL display | 56 | 49 | 7 | 0 |

(Measured on the device with `tools/audit/ext_compare.c`, device platform; each
context type lists its own extensions. Earlier in the project the desktop list
was 369 vs 305, ES 192 vs 174 and ES1 had no API at all.)

Still hidden: `GL_NV_path_rendering` (83 entry points), `GL_NV_draw_vulkan_image`,
`GL_EXT_import_sync_object`, `GL_KTX_buffer_region`, `GL_NV_feature_query`,
`GL_NV_timer_query` (ES), `EGL_WL_bind_wayland_display`, `EGL_NV_stream_attrib`
and the extension-string-only items listed in STATUS.md.

(An earlier measurement before the ES generator coverage fix was ES 195 native
vs 112 bridged: the generator emitted only 3 ES extensions, and
`EGL_EXT_platform_base` was hidden because two of its entry points were
missing, which stopped piglit from obtaining a display at all.)

Versions are reported as the driver's own (4.6 / ES 3.2), so applications that
gate on the version string rather than extension strings can still hit
unimplemented entry points; those return a GL error (logged with
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

## Real applications on the Tegra (unmodified distribution binaries)

Without the bridge, the stock armhf stack cannot do GL at all on this X
server: the server's GLX vendor is NVIDIA, there is no 32-bit NVIDIA GLX
library, so GLVND finds no vendor (`glXChooseFBConfig() failed`, `couldn't
find RGB GLX visual`, `eglInitialize() failed`). Through the bridge (one
library under `libGL.so.1`/`libEGL.so.1`/`libGLESv2.so.2`/`libGLX.so.0`/
`libOpenGL.so.0`):

| Binary (armhf, from Ubuntu) | Result |
|---|---|
| `glxinfo -B` | `NVIDIA Tegra X1 (nvgpu)`; core 4.6, compat 4.6, ES 3.2 profiles |
| `glxgears` | 60.0 fps (vsync) |
| `es2gears_x11` | 60.0 fps (EGL + GLES2) |
| `eglgears_x11` | renders (EGL + *desktop GL* in one process; window capture verified) |
| `glxdemo`, `es2tri`, `egltri_x11`, `es2_info` | run; `es2_info` reports `EGL 1.5 NVIDIA`, `ES 3.2` |
| **`glmark2`** (armhf, 33 scenes) | **score 2342 vs 2436 native arm64 = 96.1%** |

glmark2 per scene: most scenes are within a few percent of native (several
are marginally faster: the app logic and the driver run on separate cores).
The slow ones, and why (measured with `GB_STATS=1`):

| Scene | % of native | Cause |
|---|---|---|
| `buffer ... update-method=map` (x2) | 28-33% | each map fetches the buffer's current contents (a round trip + copy), then uploads on unmap |
| `build use-vbo=false` | 37% | a large mesh is copied from client memory every frame; the server idles waiting for the client |
| `desktop shadow` | 68% | many tiny state changes + client-array uploads; the server thread is ~93% busy |
| `ideas` | 76% | very high call rate (~750k calls/s) |

Findings from profiling this:

* **Time was hiding outside the timed native call.** Wall busy time (5.8 s)
  matched CPU time, but only 2.0 s was in the driver; the rest was my
  client-array upload path (context queries, buffer-binding queries and a
  `glBufferData` allocation per attribute per draw). One persistent scratch
  buffer with sub-data at a running offset, a cached context key and
  client-supplied bindings gave desktop/shadow +34%.
* **A futex wake per message was *not* the problem** (hypothesis tested and
  rejected with `strace -c`); wake hysteresis was kept anyway.
* **Spinning longer helps a little** (`GB_SPIN_US=1000`: +6% / +13% on the
  two slowest non-map scenes) at a battery cost on a handheld, so it is an
  opt-in knob, not the default.
* The overall score barely moves from optimising the slow scenes: it is a
  mean of FPS dominated by the fast ones.

Flakiness found on the way (test bug, not a bridge bug): drawing into a window
before the X server has exposed it is undefined by the pixel-ownership rule
and NVIDIA discards the fragments. Tests now wait for MapNotify + Expose
(0 failures in 130 runs afterwards).

### More real applications (2026-10-19)

Unmodified Ubuntu 24.04 armhf games and players, unpacked (not installed)
under `~/gb/apps2/root` with `tools/realapps/getapp.sh`; each is compared with
the same program built for arm64 running natively on the NVIDIA driver (mpv: the
system arm64 mpv), and where useful with the armhf build on llvmpipe (no
bridge). Scripts: `tools/realapps/`.

| Application | What it exercises | Result (bridge vs native arm64) |
|---|---|---|
| **OpenArena** (ioquake3 `renderer_opengl2`, GL 3.2 core, GLSL shaders, FBOs) | demo timedemo, 383 frames | 268 fps vs 290 fps = **92%**; llvmpipe armhf: 4.4 fps. Screenshots at two frames match native except a spinning item (0.3% of pixels) |
| **OpenArena** (`renderer_opengl1`: fixed function, client-memory vertex arrays) | same demo | 231 fps vs 477 fps = **48%** (see below); screenshots match as above |
| **SuperTuxKart 1.4** (GLES 3.2 through EGL, hardware skinning, UBOs, instancing) | 20 s AI race, 4 karts, `--profile-time` | 105.6 fps vs 107.3 fps = **98%**; races render identically (window capture) |
| **neverball 1.6** (fixed-function GL, SDL2; apt `neverball:arm64` then `:armhf`, one at a time) | level intro screen | renders on the NVIDIA GPU, identical content to native (window capture) |
| **Extreme Tux Racer 0.8.3** (fixed-function GL, textures, fog, display lists) | menus and a race (driven with xdotool) | renders identically to native; ran to the end of its 90 s test without a fault |
| **mpv 0.41** (`--vo=gpu --gpu-context=x11egl`, libplacebo OpenGL, lavfi test video) | 900 frames; `--profile=high-quality` first frame | wall time 16.0 s vs 16.0-17.2 s (decode-bound); the high-quality (EWA scaling, debanding, dither) frame is **bit-identical** to native |

The fixed-function path is the one place the bridge is clearly slower: the
legacy ioquake3 renderer makes about 1,100 GL calls per frame (431,884 requests
for the demo) and keeps its vertex data in client memory, so every draw uploads
the used part of each array. The server is idle half the time (client side:
marshalling plus the application). Ideas if this matters: honor
`GL_EXT_compiled_vertex_array` (`glLockArraysEXT`) to upload an array range
once for several draws, and coalesce neighbouring client-array uploads.

neverball and extremetuxracer were installed with apt (arm64 first, then
swapped for armhf so only one architecture was present) and removed again
afterwards. Not tested: GTK4 (GL renderer).

### Performance and CPU cost, unlocked and vsynced (2026-10-19)

Method: `tools/realapps/bench.sh <scenario> <native|bridge|sw>`. Frame rate
from the application's own counter or `fpscount.c` (an `LD_PRELOAD` swap
counter), CPU from `cpuwin.py` (utime+stime of every process sampled twice a
second; 100% = one of the four cores; the sampler itself shows up as about 7%
`python3`). *Native* = the arm64 build on the NVIDIA driver, *bridge* = the
armhf build through the bridge, *sw* = armhf on llvmpipe. Unlocked runs use
`__GL_SYNC_TO_VBLANK=0`, vsynced runs leave the driver default and set each
application's own swap interval to 1.

Frame rate unlocked (CPU as % of one core: application / `gbd`):

| Scenario | Native arm64 | Bridge armhf | Bridge vs native |
|---|---|---|---|
| OpenArena GL3.2 renderer (timedemo, 6,900 frames) | 358 fps; app 93% | 324 fps; app 43% + gbd 69% | 90% |
| OpenArena fixed-function renderer | 669 fps; app 90% | 287 fps; app 48% + gbd 74% | 43% |
| OpenArena GL3.2, armhf software (llvmpipe) | | 3.6 fps; app 266% | 1% |
| SuperTuxKart (40 s race) | 112.9 fps; app 96% | 111.6 fps; app 70% + gbd 52% | 99% |
| mpv `--profile=high-quality`, 1,500 frames | 181 fps; mpv 148% | 139 fps; mpv 97% + gbd 61% | 77% |
| glxgears | 2,790 fps; app 90% | 2,783 fps; client 17% + gbd 94% | 100% |

Presentation locked to the display (60 fps in every case; CPU as above):

| Scenario | Native arm64 | Bridge armhf (app + gbd) | Extra |
|---|---|---|---|
| OpenArena GL3.2 renderer | 38.8% | 16.9% + 22.8% = 39.7% | +1 point |
| OpenArena fixed-function | 24.3% | 21.6% + 34.7% = 56.3% | +32 points |
| SuperTuxKart | 48.4% | 44.8% + 28.9% = 73.7% | +25 points |
| mpv (60 fps video, high-quality) | 50.8% (kwin 15%) | 49.1% + 24.0% = 73.1% (kwin 14%) | +22 points |
| glxgears | 8.1% | gbd 7.9% (client below the top 10) | whole machine 45.9% vs 48.6% |
| es2gears (EGL/GLES2) | 8.4% | gbd 7.7% | whole machine 44.7% vs 47.0% |
| neverball (both presented at 60 fps) | 59.7 fps; app 47.6% | 60 fps; client 24.2% + gbd 83.1% = 107% | +60 points |
| Extreme Tux Racer (2560x1600 fullscreen) | display-bound; app 39.8% (frame counter could not hook SFML) | **about 25 fps**; client 65.7% + gbd 83.1% | |


(The `__GL_SYNC_TO_VBLANK=0` override did not unlock neverball or Extreme Tux
Racer, so they have no separate unlocked row.)

neverball and Extreme Tux Racer are the limit of the design: both use
immediate-mode and matrix calls (neverball about 17,400 requests per frame,
about 1 million per second; Extreme Tux Racer about 18,700 per frame, mostly
`glVertex3f`/`glNormal3f` between `glBegin`/`glEnd`). `gbd` spends about 0.8 us
per request in neverball and sustains it at 60 fps with 83% of a core; Extreme
Tux Racer needs about 1.8 us per request (the armhf client alone is at 66%) and
falls to about 25 fps. Packing `glBegin`..`glEnd` blocks (and runs of matrix
calls) into one request would be the next optimisation.

The full top-10 process list of every run is in `docs/perf-top10.md` (raw
output in `docs/perf-raw/`).

Is the bridge burning CPU waiting? No evidence of it. With almost no GL work
(gears at 60 fps) the whole machine uses 2-3 points more than native, and `gbd`
costs about what the native application did. With real workloads the extra is
proportional to the call rate: SuperTuxKart sends about 82,000 requests per
second (4.1 million in 50 s), and `GB_STATS` shows `gbd` busy 33 s of 52 s but
using 18 s of CPU, the rest blocked inside the driver, and idle (waiting for the
client) 19.5 s without using CPU (the wait spins a bounded number of
iterations, then sleeps on a futex). The client side costs about as much as
the native application, because marshalling takes what the driver call took;
the driver work itself moves to `gbd`. In OpenArena's GL3.2 renderer, which
batches its work, the total is the same as native; the legacy fixed-function
renderer, which uploads client-memory vertex arrays on every draw, pays for it
(see the earlier note about `glLockArraysEXT` and coalescing).

Unlocked mpv is the other gap (77%): the video frames reach the GPU through
mapped buffers, which the bridge emulates with a shadow copy that is read back
and written up around each fence.

### CPU cost of the bridge, per process (glmark2, whole run)

Same 33 scenes, native arm64 vs bridged armhf, on the live display. A sampler
read `/proc/<pid>/stat` (utime+stime) twice a second for kwin, Xorg,
plasmashell, glmark2 and gbd; a 20 s idle window beforehand gave the
compositor/X baseline, which is subtracted ("excess"). Script:
`~/gb/glmark2/cpu_procs.py` + `cpu_cmp.sh` on the device.

| Process | native arm64 | bridged armhf |
|---|---|---|
| glmark2 (client) | 316.6 CPU-s (96% of a core) | 83.3 CPU-s (25%) |
| gbd (server) | - | 293.2 CPU-s (88%) |
| app + driver combined | 316.6 | 376.5 (**+19%**) |
| kwin_x11, excess over idle | 92.7 s (29% of wall) | 123.6 s (38%, **+33%**) |
| Xorg, excess over idle | 35.3 s | 32.1 s |
| plasmashell | ~0 | ~0 |
| total excess CPU | 444.6 s | 532.4 s (**+20%**) |
| glmark2 score | 2425 | 2303 (95%) |
| total CPU per frame | 0.555 ms | 0.700 ms (+26%) |

Reading it:

* The bridge adds ~60 CPU-seconds over the run, ~0.08 ms per frame. The
  client is cheap (it carries the app's logic plus marshalling); `gbd` carries
  the driver work. The two together cost 19% more than the single native
  process.
* Xorg is unchanged, consistent with Strategy X presenting through the same
  driver path.
* **kwin used 33% more CPU with the bridge: unexplained, single run.** Either
  noise, or extra damage/expose traffic from the server presenting over its own
  X connection. To settle it: repeat the pair and compare X event counts.
* Frame counts are approximate (FPS x 10 s per scene); the same method is used
  for both runs, so the ratios are comparable.
* Consequence for triage: general transport CPU overhead is about 20%, so a
  test that is 300x slower (`depth-clear-precision-check`, below under
  "Conformance") is not explained by transport cost; something specific to its
  call pattern is wrong.

## Conformance: piglit (arm64 native baseline vs armhf through the bridge)

Same Ubuntu piglit package for both architectures, `-p x11_egl`, one process
per test, on the live display. Selection of 614 tests (613 after the exclusion below): GL 1.x/2.x, EGL, FBO
and buffer-object groups. The arm64 run is the baseline; only differences
from it are candidates for bridge bugs.

| Run | pass | fail | skip | crash | timeout | differs from native |
|---|---|---|---|---|---|---|
| native arm64 | 519 | 23 | 69 | 1 | 1 | - |
| bridge, first run | 446 | 78 | 80 | 9 | 1 | 92 |
| after fixes, run 2 | 492 | 35 | 80 | 6 | 1 | 46 |
| run 3 | 503 | 26 | 83 | 1 | 1 | 28 |
| run 4 | 511 | 22 | 77 | 1 | 1 | 14 (one new, fixed) |
| run 5 (default vendor choice) | 515 | 22 | 74 | 0 | 1 | 10 (4 not defects) |
| run 6, NVIDIA forced on both sides (final library) | 516 | 19 | 75 | 1 | 1 (+1 warn) | 5 (all known non-defects, below) |

What the sweep found (details and the generalised lessons are in
[STATUS.md](STATUS.md), lessons 10-14):

| Symptom | Cause | Fix |
|---|---|---|
| ~50 `teximage-colors` / `getteximage-*` failures and crashes in `eglMakeCurrent` at teardown | desktop GL context created via EGL classified as GLES; `glGetTexImage` unsizable; driver wrote a full image into a 512-byte scratch, corrupting its state | classify by `GL_VERSION`; unsizable arrays skip the call and raise an error |
| `draw-pixels` server SIGSEGV | NULL pixels passed to the driver for an unknown format/type | size tables extended (`GL_BITMAP`, BGR/BGRA/ABGR, packed types) |
| `draw-elements` INVALID_OPERATION | client vertex arrays + buffer-object indices were rejected | `ElementMax` server op, `DrawClient` kind 2 |
| `gl-1.5-get-array-attribs` | array state queries went to a driver that never saw client pointers | answered from the shadow |
| `color-material-array`, `line-flat-clip-color` server SIGSEGV | `glEnable(GL_VERTEX_ARRAY)` alias not tracked | wrap `glEnable`/`glDisable` |
| `shadersource-errors`, `egl-create-context-invalid-attribute-gl`, `gl-2.1-pbo` client crashes | garbage strings with an invalid shader name; unterminated EGL attrib list; stale map slot after deleting a mapped buffer | known-shader set; page-checked attrib scan; unmap on delete |
| EGL tests "piglit_egl_get_display() failed" | `EGL_EXT_platform_base` hidden | pixmap/platform-pixmap surfaces implemented |
| ES extension list 112 vs 195 | generator emitted 3 ES extensions | OES/EXT/KHR + NV aliases, alias wrappers |

**Native EGL tests may fall back to llvmpipe.** With the default GLVND vendor
choice, EGL tests that use the default display resolve to Mesa (llvmpipe) on
this device, usually because NVIDIA does not support what the test needs. Such
tests say nothing about the layer, so the comparison is made with the vendor
forced to NVIDIA on both sides
(`__EGL_VENDOR_LIBRARY_FILENAMES=.../10_nvidia.json`): native NVIDIA-only 515
pass / 21 fail / 75 skip / 1 crash; bridged NVIDIA-only 511 / 21 / 79 / 0 (+1
timeout). Six tests change result natively when NVIDIA is forced and are
excluded from the evaluation (`egl-blob-cache`, `egl-ext_egl_image_storage`,
`egl-gl_oes_egl_image`, `egl_ext_device_base`, `egl_mesa_device_software`,
`egl_mesa_platform_surfaceless`). Of the remaining 604 tests, 9 differ:
four are not defects (`depth-clear-precision-check` is the armhf test binary
looping forever on its `depth32` case, reproducible with plain Mesa llvmpipe
and no bridge; `ppgtt_memory_alignment` crashes natively;
`egl_ext_platform_device` fails natively and passes bridged;
`egl_khr_fence_sync` forks, natively EGL fails in the child and the test exits
early, while the bridge supports fork and runs checks that are meaningless
across processes). Five are missing features: GLES1 (`egl_khr_gl_colorspace`),
the `GL_KHR_debug` upcall (3 context-flag tests) and dma-buf modifiers (fd
passing). Details are in STATUS.md ("Conformance").

The native baseline was repeated four times (613 tests after excluding
`max-texture-size`): 610 tests give identical results every time; the three
that vary are multisample depth/stencil tests. The earlier "4 tests flipped in
the bridge's favour" were `max-texture-size` (native timeout, see below), the
Intel-only `ppgtt_memory_alignment` (stable native crash) and two of those
flaky multisample tests. Real differences from the stable baseline are listed above (10 in run 5, 4 of which are not defects).

**Operational hazard:** native arm64 `max-texture-size` stalls this 4 GB
device for ~100 s and triggered the OOM killer twice (plasmashell, konsole,
kded5, kwin killed; the invoking process was the test itself, RSS only ~29 MB:
the memory is GPU/nvmap or swap-thrashed, not visible as process RSS). The
bridged armhf run passes it in 7.4 s. The test is excluded, test processes get
`oom_score_adj=1000`, and a `MemAvailable` watchdog kills test binaries.

Triage tooling kept on the device (`~/gb/pg`, `~/gb/gbd_gdb.sh`, `gbd_mc.sh`,
`gbd_reg.sh`, `gbd_sz.sh`): `run.sh <arm64|armhf> <outdir> <regex>` runs the
selection; gdb works on the armhf client from the arm64 gdb (the client
processes run natively as AArch32), and a gdb wrapper as `GB_SERVER` gives
server backtraces.

### Final Tegra result with the complete library (vendor forced to NVIDIA)

Native arm64 515 pass / 21 fail / 75 skip / 1 crash; bridged armhf 516 / 19 /
75 / 1 (+1 timeout). Five differences, none a bridge defect:
`depth-clear-precision-check` (the armhf test binary loops forever on its
`depth32` case, reproducible with plain Mesa llvmpipe and no bridge),
`depthstencil-default_fb-clear` (flaky natively), `gl-1.4-multidrawarrays-errors`
and `egl_ext_platform_device` (fail natively, pass bridged) and
`egl_khr_fence_sync` (the test forks; natively EGL fails in the child). Since
the previous table the GLES1 test, the three `GL_KHR_debug` context-flag tests
and the dma-buf-modifiers test now pass.

### Dev-machine loop (x86_64, Radeon + Mesa)

The whole piglit `quick` profile (44,224 tests, ~15 min per side) runs natively
and bridged on the dev machine's GPU through the DRM render node: 39,202 pass /
4,531 skip / 96 fail / 385 crash / 10 warn on both sides, **0 differences**
(STATUS.md, "Dev-machine conformance loop"). It found the Begin/End, list
compile, shadow-reuse and indexed-getter bugs that the Tegra selection missed.

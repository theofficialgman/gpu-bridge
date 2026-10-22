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
| neverball (both presented at 60 fps) | 59.7 fps; app 47.6% | 60 fps; client 18.9% + gbd 55.6% = 74.5% (was 24.2% + 83.1%) | +27 points (was +60) |
| Extreme Tux Racer (2560x1600 fullscreen) | display-bound; app 39.8% (frame counter could not hook SFML) | **60 fps**, client 52.8% + gbd 60.5% (was 25 fps, 65.7% + 83.1%); with `GB_LAZY_GETERROR=1` **60 fps**, client 38.6% + gbd 45.8% | |


(The `__GL_SYNC_TO_VBLANK=0` override did not unlock neverball or Extreme Tux
Racer, so they have no separate unlocked row.)

neverball and Extreme Tux Racer are the worst case for a remoting layer: both
use immediate-mode and matrix calls (neverball about 17,400 requests per frame,
Extreme Tux Racer about 18,700 plus about 800 client-array draws), and Extreme
Tux Racer also polls `glGetError` after every draw. The first measurements
(25 fps) were not explained by the per-request cost. `GB_STATS` and phase timers
in `gbd` found four causes, now fixed:

1. **Every indexed client-array draw sent the arrays from vertex 0 up to the
   largest index.** Extreme Tux Racer draws its terrain in slices, so each
   draw re-sent everything before its slice: 4 GB in 38 s, 640 KB per draw. The
   client now sends only the vertices from the smallest to the largest index
   and the server draws with a base vertex that moves them back
   (`glDrawElementsBaseVertex`, desktop GL 3.2 and later). 1.9 GB over the same
   run, the rest being genuinely large arrays. This was the largest single gain:
   34 to 59 fps (with the options below already in).
2. **`glBufferSubData` into the scratch buffer cost 4.8 us per call on the
   Tegra** (2.4 million calls). The scratch buffers are now persistent coherent
   mappings (`glBufferStorage`, GL 4.4+) and an upload is a `memcpy` (1.1 us per
   attribute including cache misses). A new store replaces a full buffer.
   `GB_NO_SCRATCH_MAP=1` goes back to `glBufferSubData`.
3. **Small asynchronous requests are batched.** Up to 32 KB of requests of at
   most 1 KB each (everything that does not wait for a reply) are collected in a
   staging area and sent as one `GB_OPX_Batch` ring message, flushed before any
   other kind of request, on `glFlush`, and at thread exit. `gbd` handles each
   record with the same dispatch code and sends debug messages once per batch.
   On the x86 loopback a `glVertex3f` costs the armhf client 28 ns instead of
   65 (`callbench`), and on the Tegra the client-plus-server cost of an
   immediate-mode call fell from 329 ns to 129 ns (native: 70 ns). `GB_NO_BATCH=1` turns it off; a registered debug callback
   turns it off too (the helper thread cannot flush another thread's staging
   area and the callback must see messages promptly).
4. **`glXSwapBuffers` waited for its own reply**, so the client could not build
   the next frame while the server presented the current one. The reply is now
   collected one swap later (`LAG_SYNC` in the generator); any later request that
   waits for a reply collects it on the way. One frame of queueing, as in a
   driver.

`GB_LAZY_GETERROR=1` is an option, off by default: after eight clean
`glGetError` polls in a row, the next 63 are answered `GL_NO_ERROR` without
asking `gbd` and the 64th is real. A GL error is a sticky flag in the driver
so an error in between is not lost, it is reported up to 63 polls late. That
saves the 800 round trips a frame that Extreme Tux Racer makes (54 -> 59 fps)
but changes the exact timing of errors, which conformance suites test, so it
is for applications that only look for "any error" after the fact.

Results on the other applications with the same changes (unlocked, same
scenarios as the tables above): OpenArena GL3.2 renderer 324 -> 415 fps in the
timedemo (native 358), OpenArena fixed-function 287 -> 533 fps (native 669),
glxgears about 2,700 fps (unchanged).

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

## More applications (apt, armhf through the bridge vs arm64 native)

Installed from the Ubuntu archive with apt, one architecture at a time, and
removed afterwards (scripts in `tools/realapps`: `glm.sh`, `mt.sh`, `games.sh`,
`runall.sh`; raw output in `docs/perf-raw/`). Each runs unlocked
(`__GL_SYNC_TO_VBLANK=0`) and vsynced; 100% is one core. Games that limit their
own frame rate (Chromium B.S.U., X-Moto, SuperTux) show the same fps in both
columns. The screenshot tool and the user's `htop` appear in some top-10 lists
and are not part of the application.

| Application (API) | Native arm64 | Bridge armhf (app + gbd) |
|---|---|---|
| glmark2 2023.01, GLX (GL 2.0 scenes, 31 scenes) | score 2262; app 95% | **2240 (99%)** (was 2174); 30% + 87% |
| glmark2-es2, EGL (GLES 2.0) | score 2263; app 96% | **2172 (96%)** (was 2135); 25% + 86% |
| minetest 5.6.1 (Irrlicht, fixed-function with client arrays), unlocked | 179 fps; 112% | **166 fps** (was 55); 101% + 89% |
| minetest, vsynced | 60 fps; 56% | **60 fps** (was 30); 91% + 55% |
| SuperTux 0.6.3 (SDL2, GL 3.3 core), a level, vsynced | 59.3 fps; 41.5% | 58.7 fps; 72% + 63% (see the SuperTux section) |
| Chromium B.S.U. (fixed-function GL, textures, blending) | 49 fps; 6% | 47 fps; 10% + 7% |
| X-Moto 0.6.2 (fixed-function GL), tutorial level | 50 fps; 28% | 50 fps; 23% + 8% |
| Armagetron Advanced 0.2.9 (fixed-function GL) | one core (96%), fps not captured | ~215 fps; 31% + 81% |
| GLtron 0.70 | renders (fps not captured) | ~770 fps; 48% + 72% |
| Godot 3.5 (GLES3 renderer, rotating lit meshes with shadows) | ~650 fps; 96% (kwin 78%) | ~730 fps; 42% + 92% (kwin 86%) |

All of them render on the NVIDIA GPU through the bridge with the same picture
(windows captured with `import`). Findings:

* **The scenes that are slow are the ones that do per-frame work in client
  memory.** glmark2 per scene (`docs/perf-raw/glm_raw_*`): 29 of 31 scenes are at
  92-105% of native in both GL and ES2. `build use-vbo=false` (client arrays
  every frame) is at 53% (GL) and 38% (ES2), `buffer ... update-method=map
  interleave=true` (mapped-buffer writes) at 21% and 19%, and a few 2D scenes
  (`desktop`, `ideas`) at 58-79% of native.
* **minetest was the worst real case, and client arrays were the reason.**
  `GB_STATS` showed 3.5 million indexed client-array draws in 50 s, 8 GB of
  vertex data (160 MB/s), `gbd` busy 95% at 9.8 us per draw, the client blocked
  on a full ring. Five changes (next section) took it to 166 fps unlocked
  (native 179) and a steady 60 fps vsynced.
* **Remaining glmark2 gaps are not client arrays.** `buffer ... update-method=map`
  (mapped-buffer writes) is at 18-34% of native: a `glMapBuffer(WRITE_ONLY)`
  keeps the untouched part of the buffer, so the client downloads the whole
  buffer from the server on every map (a round trip plus the data) and uploads
  it again at unmap. Fixing that needs either a trustworthy mirror of the
  buffer's contents or dirty-range tracking; not done. `desktop` and `ideas`
  on ES2 (72%, 58%) are dominated by the driver's own per-draw cost: a 60-byte
  client-array draw takes 12 us inside the driver there, natively as well.

## More games, played by hand

Games that need menus and real input were played by a person: `tools/realapps/play.sh`
launches the game windowed with the frame counter (`fpscount`), the person clicks
through to gameplay and says when they are in, then `play.sh measure` records 45 s
of fps (a 2 s window each) and the CPU of the top 10 processes. The same was done
with the native arm64 build. Both sides were driven by hand, so the scenes are
similar but not identical (a car in a race, a city of some size): treat the CPU
figures as indicative. Unlocked (`__GL_SYNC_TO_VBLANK=0`) unless the game sets its
own swap interval. 100% is one core; "app + gbd" for the bridge.

| Game (GL use) | Native arm64 | Bridge armhf |
|---|---|---|
| Trigger Rally 0.6.6.1 (SDL2, fixed-function 3D, immediate mode) | 260 fps (233-277); app 60% | **152 fps** (121-179); 47% + 57% = 104% |
| Neverputt 1.6 (fixed-function, game vsyncs itself) | 60 fps; 13.7% | 60 fps; 4.7% + 10.4% = 15.1% |
| OpenCity 0.0.7 (SDL 1.2, fixed-function, display lists) | 23.6 fps (20-33); 51% | 28.0 fps (24-37); 23% + 38% = 61% |
| LinCity-NG 2.10 (SDL, many small textured quads) | 18.7 fps (15-30); 32% | 18.4 fps (12-37); 19% + 49% = 68% |
| Teeworlds 0.7.5 (SDL2, batched 2D quads, local server) | 60 fps; 20.5% | 59.8 fps (56-60); 14.6% + 14.2% = 28.8% |
| SuperTuxKart 1.4 (GL 3.3 core, four-kart race on lighthouse, 1024x576, unlocked) | 110.3 fps (109-111); 82.5% | **106.6 fps** (96-110); 74.2% + 52.5% = 126.7% |

Not run: **AssaultCube** (the armhf build crashes with SIGBUS on an unaligned
`ldrd` in its own code before any GL work, identically with no bridge at all: the
same hardware limitation as the CTS `advanced-bindingUpdate` case), **VLC** (skipped:
the arm64 ffmpeg here is patched for the Tegra's hardware decoders and the armhf
one is not, so a decoder comparison would hide the display path), Crawl (starts and
renders, turn-based so not measured).

* **Trigger Rally is the one with a real gap (59% of native fps).** The server
  statistics of a session (`STATS=1`) show about 11,000 immediate-mode calls per
  frame (`glVertex3fv`, `glNormal3fv`, `glTexCoord2fv`, `glBegin`, ...; 115 million
  messages in the session) and one `glFinish` per frame, a synchronous call that
  waits about 1.9 ms for the GPU. The driver's own cost per vertex call is the same
  as native (0.1-0.2 us); what is added is the per-message handling (about 0.1-0.2
  us each, 11,000 of them a frame) and that `glFinish` serialises client and server
  once per frame, so the client cannot build the next frame while the GPU finishes the
  last, as it does natively. Packing `glBegin`..`glEnd` blocks into one message would
  save part of the per-message cost; I estimate 5-10% of the frame, so it was not done.
* **LinCity-NG at the same frame rate costs about twice the CPU.** A micro-pattern
  of its sprite drawing (`tests/loopback/call_bench.c`: enable, bind, colour, a
  four-vertex quad, disable) takes 6.5 us per sprite natively and 8.8 us through the
  bridge (1.36 times), nearly all of it the driver's texture-state cost (native too);
  its frames hold about 9,700 such blocks. The remaining difference is the per-call
  handling and waiting that every immediate-mode call pays.
* SuperTuxKart, driven by hand through a race (the earlier scripted runs in this
  report predate the batching, client-array and synchronous-path work): 97% of
  native frame rate at 1.5 times the CPU (the extra is `gbd`'s core).
* The rest (Neverputt, Teeworlds, OpenCity) are at native frame rates with a similar
  or lower total CPU.

## x86 games under box86 (32-bit x86 -> armhf -> the bridge)

box86 0.3.9 (dynarec) runs 32-bit x86 binaries on the armhf side; it loads the
armhf `libGL.so.1`, so with the bridge's client library first on the library
path every GL call of an emulated game goes through it. The reference is the
x86_64 build of the same game under box64 on the arm64 side with the real
NVIDIA driver (no bridge). The games live on the external drive
(`/media/gman/.../Applications_x86_64`); they were run in place with a scratch
`HOME` so no saves or configs were touched. Both sides were played by hand with
`tools/realapps/play.sh`. Frames are counted inside the bridge client
(`GB_FPS=1`, same `FPSCOUNT` lines as the preload counter) because box86 reaches
`glXSwapBuffers` through its own wrappers, which an `LD_PRELOAD` counter cannot
see; the box64 runs therefore have no frame counter and are compared on CPU.

| Game (engine) | box86 through the bridge | box64 native (no bridge) |
|---|---|---|
| vektor2089 (SDL2 + GL, 480x272 pixel art, 60 fps) | 60.0 fps; game 98% + gbd 51% | game 97% |
| SkyRogue (Unity, 1024x600, title screen with the game running) | 49 fps (22-78); game 160% + gbd 46% | game 181% |

* Both games render correctly through the bridge (title screens, the games
  themselves). The emulated game is the CPU ceiling in both setups: vektor2089's
  process is at 97-98% with and without the bridge (the bridge's addition is the
  second core for `gbd`), and SkyRogue's Unity threads are at 160-180%.
* Not run: **Celeste** (the folder does not have the libraries box86 needs:
  FAudio and others). The other titles on that drive (Baba Is You, Forget the
  Brakes, Mixolumia, Cook Serve Delicious) only have x86_64 builds, which need
  box64 on the arm64 side and so never reach the bridge.

## SuperTux: native vs the layer

SuperTux 0.6.3 (SDL2, GL 3.3 core renderer), level `23rd_airborne` with Tux
standing at the start (a steady scene; running right kills him within seconds
and the fades make the runs incomparable), 45 s, window 1280x800. 100% = one
core. The game limits itself to about 64 logic steps per second whatever the
swap interval, so "unlocked" means vsync off in its own config, where it
renders about 66 fps; it cannot be pushed higher (an `SDL_Delay` that returns
at once changed nothing). Measured with `tools/realapps/stux_run.sh`, which
checks the screenshot of every run (the first measurements in this session
were partly title-card runs: the game needs window focus for its key presses).

(Results at the start of the analysis; the current ones are below.)

| | native arm64 | bridge, exact errors (client + gbd) | bridge, `GB_LAZY_GETERROR=1` |
|---|---|---|---|
| vsync off (game-limited) | 66.0 fps; 41.5% | 65.5 fps; 78% + 69% = 147% | 65.8 fps; 40% + 38% = 78% |
| vsync on | 59.3 fps; 41.5% | 58.7 fps; 72% + 63% = 135% | 59.2 fps; 34% + 33% = 68% |

Repeat runs vary by about 5 points. Native SuperTux uses 41% of a core even
vsynced (its own main loop), so the comparison is 41% against 135%/147%.

**Why.** `GB_STATS` shows the game calls `glGetError` after every GL call,
about 2,000 times per frame (2.97 million in 38 s), plus about 250
`glGetAttribLocation` and 100 `glGetUniformLocation` per frame, all
synchronous; the client waits on replies for 80% of its time. A sound
`glGetError` costs the bridge 0.6-0.8 us when the server is idle (native
25 ns), but here each one waits for the driver work queued before it
(`glBufferData` 10 us, draws 5 us, binds 1 us). CPU profiles (`prof.c`, a
SIGPROF preload, since perf does not work on this kernel) put 55% of the
client's samples and 33% of gbd's in ring-wait spinning (`gb_ring_wait_data`,
`sched_yield`); the rest of the client is the game itself (zlib/PNG/vorbis for
loading and music, `floorl`, ...) and almost none of it is marshalling. So
the excess is two cores taking turns: one works while the other spins.

**Where a synchronous call's time goes, and what was done about it.**
Measured on the Tegra with `tests/bench/pingpong.c` (two processes, a
simulated driver call of N us on the server) and `tests/bench/ring_pingpong.c`:

| way of waiting | idle round trip | CPU per wait of 30 us |
|---|---|---|
| spin with the CPU yield hint | 0.18 us | 30 us |
| spin with `sched_yield` (the ring's default) | 0.31 us | 31 us |
| `wfe` (user space can use it here) | 0.20 us | 30 us |
| futex (sleep at once) | 10.5 us | 7 us + 7 us on the other side, 12 us more latency |

So a spin costs exactly the wait, a sleep costs a fixed 14 us of CPU and 11 us
of latency, and the break-even is around 15-20 us. SuperTux's waits are 1-10
us, so spinning is right (the earlier `GB_SPIN_US` experiment says the same:
sleeping sooner gave the same CPU and fewer frames). `wfe` would only lower
power, not the CPU percentage (the process is still running; the battery
gauge cannot show it while the Switch is charging, so it was not measured).
The half of the cost that can go is the protocol's own: a `glGetError` took
0.72-0.83 us bridged, the ring layer alone is 0.45-0.54 us, a raw shared-word
handoff is 0.2 us. Changes, in order:

* *Upcall queue lock*: the server took a mutex on every request (twice for a
  synchronous one) to look at an upcall queue that is almost always empty; now
  an atomic flag is checked first.
* *Ring handshakes on AArch64*: the publish-then-look-at-the-peer's-sleep-flag
  steps (four per round trip) used a full barrier; a sequentially consistent
  store followed by a sequentially consistent load (`stlr`/`ldar`) orders them
  at a fraction of the cost (ARMv8 is multi-copy atomic, so it also holds
  against the 32-bit client's barrier). `glGetError` 830 -> 770 ns.
* *Mailbox for `glGetError`*: a request and a reply cell, one cache line each
  way, in the rings' control blocks, instead of a record and a tail counter
  each way. The request is ordered behind everything already in the ring (the
  server consumes the ring up to the post first). Used unless a debug
  callback is registered, inside an upcall, or `GB_NO_FAST_SYNC` is set; it
  keeps answering upcalls that arrive while it waits. `glGetError` 770 ->
  345 ns (32-bit client; 715 -> 350 ns 64-bit); `glEnable` + `glGetError`
  610 -> 410 ns (native: 25 ns).
* *A lost wake-up, found by SuperTux and not by the loopback tests*: the first
  version woke a sleeping peer with a bare futex wake. A wake that arrives
  after the peer announced it will sleep but before it reaches `futex_wait`
  is lost when the futex word did not change, and the mailbox does not touch
  the word; both sides slept until the 100 ms watchdog (10 fps, then a hang).
  The fix: when the peer's sleep flag is seen, push a 16-byte no-op record,
  which moves the tail and wakes it the ordinary way. `tests/unit/test_mbox.c`
  (two threads that sleep and wake at random, ordering and worst-wait checks)
  reproduces the bug if the old wake is put back (it hangs) and passes now on
  both architectures, on the dev machine and the Tegra.
* *A second bug in the same change, found by the GLX CTS groups*: after
  answering a mailbox request the server pushes the wake-up record if it still
  sees the client's sleep flag, and that flag can be stale (the client has woken
  and not yet cleared it). The record stayed in the response ring until the
  client's next ordinary reply wait, which took it for an out-of-step reply and
  aborted (`reply desync: op 65551 ... seq 0`). EGL runs did not hit the timing;
  four GLX groups did. The client now recognises and skips these records
  wherever it reads the response ring (ordinary waits, the swap-reply wait, the
  helper thread). `tests/loopback/sync_mix.c` (every kind of synchronous call,
  random sleeps) fails on the Tegra without the filter and passes with it; the
  four groups pass and match native again.

**Pinning the two sides to one core.** Sharing a core removes the cache-line
transfers (about 90 ns each way) but needs a context switch between the two
processes each way instead, and gives up running them in parallel. Measured
on the Tegra with `tests/bench/pingpong.c` (`PIN_CLIENT` / `PIN_SERVER`), per
synchronous call, with N us of server work (the driver call); CPU is client +
server, including the work itself:

| server work | cross-core, spin: round trip / CPU | same core, futex: round trip / CPU | same core, `sched_yield` handoff |
|---|---|---|---|
| 0 us | 0.18 / 0.36 us | 8.7 / 8.7 us | 3.3 / 3.3 us |
| 2 us | 2.3 / 4.7 us | 7.7 / 7.6 us | 7.5 / 7.5 us |
| 5 us | 5.4 / 10.7 us | 10.9 / 10.8 us | 13.4 / 13.4 us |
| 10 us | 10.4 / 20.6 us | 16.0 / 15.9 us | 23.6 / 23.4 us |
| 30 us | 30.3 / 60.4 us | 35.8 / 35.6 us | 63.4 / 63.0 us |
| 100 us | 100.3 / 200 us | 106 / 105 us | 203 / 202 us |

Below about 5 us of work two spinning cores are both faster and no more
expensive; around 10 us a shared core saves a quarter of the CPU for half again
the latency; at 30 us and above it saves 40-50% for 6-18% more latency. But
that saving is also available without pinning: a wait that is known to be long
can sleep (the ring already does after about 25 us). SuperTux's waits are mostly
1-2 us (a check after a cheap call), with a few 5-10 us ones, so for it the
cross-core spin is the better choice, and an application that overlaps client
and server work (Extreme Tux Racer: 53% + 61% of a core) would be cut to half
its frame rate on one core. The four cores of the Tegra X1 are one cluster with
a shared L2, so the cross-core handoff measured here (0.18 us) is already the
cheapest the hardware offers; there is no closer pair to pick. Spinning with
the yield hint on a *shared* core is the one pathological combination (it
starves the peer until the time slice ends; the ring's `sched_yield` phase is
what prevents that if the scheduler ever puts both on one core).

**Results** (steady scene, repeat runs vary by about 5 points):

| | native arm64 | bridge exact errors, at the start of this analysis | bridge exact errors, now | bridge, `GB_LAZY_GETERROR=1`, now |
|---|---|---|---|---|
| vsync off (game-limited) | 65.9 fps; 41-44% | 65.5 fps; 147% (78% + 69%) | **65.8 fps; 110% (60.0% + 49.7%)** | 65.9 fps; 78% (38.8% + 38.8%) |
| vsync on | 59.3-59.6 fps; about 40% | 58.7 fps; 135% (72% + 63%) | **59.1 fps; 97% (53.1% + 43.7%)** | 59.2 fps; 71% (35.9% + 34.8%) |

(The native vsync-on CPU is from the earlier verified runs, 39.7-41.5%; a
later native vsync-on run failed its gameplay check and was discarded.)
Exact-error mode went from 3.5 times native CPU to 2.5 times, with the same
frame rates and the same error semantics; lazy mode, which does not wait at
all, is unchanged at 1.7-1.9 times.

**What else was tried or considered.**

* *Location caching* (done, earlier in this section's history):
  `glGetUniformLocation` / `glGetAttribLocation` answered from a per-context
  cache once the driver has returned a non-negative location, forgotten on
  `glLinkProgram`, `glDeleteProgram`, `glProgramBinary` and context
  destruction (any context sharing the program name is invalidated too); -1
  is never cached. It removes the 350 round trips a frame; A/B
  (`GB_NO_LOC_CACHE=1`): 5-8 points less CPU, fps unchanged.
  Conformance: `dEQP-GLES3.functional.attribute_location` (298),
  `dEQP-GLES3.functional.uniform_api` (2182), `dEQP-GLES2.functional.uniform_api`
  (1267), `dEQP-GLES31.functional.program_uniform` (1014) and
  `dEQP-GLES3.functional.shaders.linkage` (119) pass in full, bridged and
  native, over EGL and GLX. `state_smoke` relinks with moved attribute
  locations, deletes the program and checks unknown names.
* *Not done: skipping `glGetError` round trips by proving the preceding calls
  cannot have failed.* Possible for a few calls (`glActiveTexture`,
  `glBindTexture`, `glEnableVertexAttribArray`, `glVertexAttribPointer`
  against tracked state), not for the ones that matter most here
  (`glBufferData` can run out of memory, draws and uniforms depend on a lot
  of state), and each of those still has to be waited for.
* *Timeline view of the remaining cost.* In exact mode the client and the
  server take turns, so each is busy (working or spinning) for about the whole
  critical path: client work + server work + (calls x round trip). Every
  microsecond taken off a round trip comes off both processes' CPU: 2,000
  checks a frame x 0.45 us saved = 0.9 ms of a 15.6 ms frame, about 11 points
  of the total.

## Client arrays (minetest, glmark2 `build`, ETR)

Changes, in the order they paid off (`tests/loopback/array_bench.c` measures
the per-draw cost; Tegra numbers, armhf client, 600 vertices x 36 bytes
interleaved with 1800 indices / 30 vertices with 90 indices; native 21 us / 2.4 us):

| | 600 vtx | 30 vtx |
|---|---|---|
| before this work | 91 us | 11.7 us |
| attributes that share memory sent once (an interleaved array was sent once per attribute) | 60 us | 9.4 us |
| `glFinish` every 50 draws removed from the benchmark (it was half of the small-draw time, native too) | 44 us | 5.5 us |
| the driver reads the request's own memory (no scratch upload, no binds), min/max index scan in one pass under a single fault guard, inline messages up to half a ring | 33 us | 4.9 us |

* **One copy of shared memory.** The client merges attribute ranges that
  overlap into blobs; each attribute names its blob and offset. A four-attribute
  interleaved mesh went from 4x the bytes to 1x (minetest: 8 GB to about 2 GB).
* **No upload.** The server calls `gl*Pointer` with pointers into the request
  (the data is there already, validated against the indices, and a GL consumes
  client arrays during the draw call), with `GL_ARRAY_BUFFER` at 0 for the
  calls (restored afterwards only if the application had one bound). That
  removes the scratch-buffer copy, four `glBindBuffer` calls and the two
  `glIsBuffer` checks per draw (the latter are now every 64th draw as a
  safety net for scratch names the server cannot see deleted).
  `GB_ARRAYS_VIA_VBO=1` goes back to the scratch buffer.
* **Indices are not uploaded either**: client indices come with the request
  and are passed on as a client pointer.
* **Vertex range.** For indexed draws only the vertices from the smallest to
  the largest index are sent. Folded into the attribute pointers on the server
  (the Tegra driver mishandles a negative base vertex combined with client
  pointers: it indexes at 2^32 and faults, which is what the first version of
  this change hit in `glx_smoke`).
* **Client index scan**: min/max over the indices in one pass under one
  `sigsetjmp` guard (previously 256-byte chunks with a per-index branch).
* **Messages up to half a ring are one record** the server reads in place
  (they were chunked and reassembled above a quarter): glmark2's 492 KB mesh
  per frame. A larger request ring was slower for medium messages (the 1 MB
  ring stays in the L2 cache), so the ring stays 1 MB.

Result: minetest 55 -> 166 fps (unlocked), 30 -> 60 (vsynced); glmark2 score
2174 -> 2240 (GL), 2135 -> 2172 (ES2), `build use-vbo=false` 53% -> 85% (GL),
38% -> 63% (ES2); Extreme Tux Racer holds 60 fps with exact `glGetError`
(client 53% + gbd 61% instead of 60% + 70%).

What is left per draw is one driver call per attribute plus the draw. The
driver calls could only be skipped by keeping the data at the same address
between draws and tracking the driver's pointer state, which was not
attempted. Conformance: the GLES2/3/3.1 vertex-array, draw, draw-indirect,
primitive-restart and vertex-attribute-binding groups plus `KHR-GL46.draw_indirect`
give the same results natively and bridged over EGL and GLX (the only
difference is the existing armhf SIGBUS in `vertex_attrib_binding.advanced-bindingUpdate`);
`state_smoke` gained range and interleaved client-array tests.

* **SuperTux** matches native frame rates but costs about 3 times the CPU in exact-error mode; analysed in the SuperTux section below.
* **Native arm64 GLtron hangs when it is asked to quit**: after SIGTERM it
  spins at 98% in `sprintf` with its window closed (16 minutes before I killed
  it). Not related to the bridge; the armhf build exits normally. The harness
  now uses `timeout -k`.
* Godot 3 runs its GLES3 renderer (OpenGL ES 3.0 context on the NVIDIA driver)
  correctly through the bridge. The bridged frame rate (730) is a little above
  native (650); I did not investigate why, treat it as equal.
* Stellarium and Warzone 2100 have no armhf data package in the archive,
  0 A.D. needs 3.2 GB of data; they were not tested.

# gpu-bridge: implementation status

Last updated after the dev-machine conformance loop, the full version /
extension generation, the upcall and fd-passing mechanisms (see "Coverage",
"Dev-machine conformance loop" and "Conformance" below). Read together with
[ARCHITECTURE.md](../ARCHITECTURE.md) (the design) and
[target-report.md](target-report.md) (measurements on the real device) — this
file records what actually exists, what was measured, and what is known to be
missing.

## Build and test

```
sudo apt install gcc-multilib libc6-dev-i386     # once, for the -m32 builds
cmake -S . -B build -G Ninja && ninja -C build
ctest --test-dir build                            # 14 tests
```

On the dev machine "32-bit client" is **i386** and "64-bit server" is
**x86_64**. The bridge logic depends on pointer width, not the ISA, so this
validates the marshalling that will run as arm32 -> arm64 on the Tegra.
What it cannot validate is anything NVIDIA-specific (see "Phase 0" below).

| Test | What it proves |
|---|---|
| `ring64`, `ring32` | SPSC ring: wrap/pad, full, 400k-msg two-thread stress, blocking/timeout, close-drain, hostile input rejection, cross-process (ASan/UBSan clean) |
| `xabi_32prod_64cons`, `xabi_64prod_32cons` | An i386 and an x86_64 process share one memfd ring, both directions, 300k records |
| `chan_xabi` | Control socket handshake, memfd passing via SCM_RIGHTS, messages 0 B .. 30 MB (chunked), server-death detection without hanging |
| `smoke_native` / `smoke_b64` / `smoke_b32` | EGL pbuffer + GLES2: clears, shaders, client-memory vertex arrays and indices, VBO/IBO, texture upload, error reporting, 4 MB readback, extension filtering. **Same source linked three ways**: native driver (reference), bridge 64->64, bridge 32->64 |
| `x11_native` / `x11_b64` / `x11_b32` | Strategy X: the app owns an X11 window; the server renders into it over its own X connection using only the XID; pixels verified via XGetImage; client-driven resize. Run on headless Xvfb |
| `glx_native` / `glx_b64` / `glx_b32` | GLX + desktop GL, pixel-verified: immediate mode, display lists, `glCallLists`, matrix/light/material queries, client-memory arrays and indices, `glInterleavedArrays`, `glArrayElement`, range/multi draws, textures, `glGetTexImage`, pixel unpack/pack **buffer objects**, then a **3.3 core** context via `glXCreateContextAttribsARB` (VAO, VBO, GLSL, instancing, `glGetUniformfv`, FBO, core-profile error behaviour). Same app linked three ways. Also run on a real display (see below) |

## What exists

| Component | Files | Notes |
|---|---|---|
| Ring buffer | `src/proto/gb_ring.[ch]` | futex wake, pause -> yield -> sleep wait strategy, peer-death detection via control socket POLLHUP |
| Channel + messages | `src/proto/gb_chan.[ch]` | request+response ring in one memfd; transparent BIG/CHUNK for messages > ring/4 |
| Control protocol | `src/proto/gb_ctl.[ch]` | socketpair + SCM_RIGHTS; server lifetime tied to client; version + op-table hash check at HELLO |
| Codec | `src/proto/gb_codec.h` | bounds-checked reader/writer, fixed-width only |
| Size rules | `src/proto/gb_gl_sizes.h` | pname counts, pixel rectangle sizes, EGL attrib list validation; unknown -> -1, never a guess |
| Generator | `tools/gb-gen/` | One library, one opcode space: EGL 1.0-1.5, GLES 1.0, 2.0-3.2, GLX 1.0-1.4, GL 1.0-4.6 compat/core, plus **every extension the Khronos registries list** for those APIs. 3,593 commands: 3,204 generated, 94 wrapped (generated encoder + hand-written shadow-state wrapper), 94 manual, 34 aliases (vendor spellings forwarding to the core wrapper), 167 unsupported (explicit list via `gb_gen.py --report`) |
| Server `gbd` | `src/server/` | loads real libEGL/libGLESv2/**libGL**, one worker thread per client thread, handle tables, scratch buffers for client arrays, own X connection(s) with `XInitThreads` + an error handler, server-side size rules that query native GL state |
| Client library | `src/client/` | **one** library exporting EGL + GLES + GLX + desktop GL, built as `libGL.so.1` in `build/shim{32,64}/` with symlinks for `libEGL.so.1`, `libGLESv2.so.2`, `libGLESv1_CM.so.1`, `libGLX.so.0`, `libOpenGL.so.0` (one bridge connection per process, so mixed-API apps such as eglgears work) |

## GLX and desktop OpenGL

`libGL.so.1` is built for both ABIs (`build/shim32/`, `build/shim64/`). Put
that directory first in `LD_LIBRARY_PATH` (or install it in place of the
system libGL on a machine that has none for that ABI) and an application
links against the bridge without modification.

Verified with **unmodified distribution binaries** on a real X server
(KDE/XWayland, AMD Radeon RX 9060 XT):

* `glxinfo -B`: reports the GPU in the core 4.6, compatibility 4.6 and
  ES 3.2 profiles, including the `GL_ATI_meminfo` / `GL_NVX_gpu_memory_info`
  queries.
* `glxgears` (distro binary, `libGL.so.1` swapped): runs.
* 32-bit `gears_b32` / `glx_b32`: `GL_RENDERER` is the Radeon (radeonsi),
  vsynced 60 fps.

**Hardware vs software.** A headless Xvfb has no GPU path, so everything run
there is Mesa's llvmpipe in *both* the native reference and the bridged run.
Those runs validate marshalling, not acceleration. Acceleration is only
evidenced by the runs on the real display (the `GL_RENDERER` line in the
output); always read `GL_RENDERER` before drawing conclusions.

Notable behaviour/fidelity decisions:

* **Core profiles reject client-memory arrays at the pointer call**, as the
  driver does (profile detected lazily, without provoking errors).
* **Extension strings are filtered** to commands the bridge implements, and
  core contexts enumerate extensions with `glGetStringi`; the server builds
  the legacy string from it without ever calling the legacy query in a core
  context (it would raise `INVALID_ENUM` the app would later see).
* **Server-sized queries** (`glGetUniform*`, `glGetTexImage`,
  `glGetCompressedTexImage`, `glGetPixelMap*`, ...) compute the length from
  real GL state on the server and return it in the reply.
* **Pixel buffer objects work**: encoders consult the client-tracked
  `PIXEL_PACK/UNPACK_BUFFER` binding and send an offset instead of data.

Not supported (stub + log with `GB_LOG_MISSING=1`, never a crash): see
"Known limitations"; `gb_gen.py --report` lists every unsupported command with
its reason. An extension is advertised only if *all* of its entry points are
implemented.

## Coverage: API versions and extensions

The generator takes every command of GL 1.0-4.6 (compat and core), GLES 1.0 and
2.0-3.2, EGL 1.0-1.5 and GLX 1.0-1.4 plus every extension in the Khronos
registries for those APIs; commands it cannot marshal safely are classified
`unsupported`, and their extensions are hidden from the advertised strings.

Measured by `tools/audit/ext_compare.c` (one line per advertised
extension/version, native vs bridged; `tools/audit/why_missing.py` explains
each difference):

| Driver | List | native | bridge | native-only |
|---|---|---|---|---|
| NVIDIA / Tegra X1 (arm64 vs armhf) | desktop GL compat | 370 | 362 | 8 |
| | ES 3.2 | 193 | 190 | 3 |
| | ES 1.1 | 35 | 35 | 0 |
| | EGL display | 56 | 49 | 7 |
| Mesa radeonsi (x86_64 native vs bridge) | desktop GL compat | 322 | 322 | 0 |
| | ES 3.2 | 165 | 165 | 0 |

What is still hidden on NVIDIA, and why: `GL_NV_path_rendering` (83 entry
points with COMPSIZE rules that need verifying on the NVIDIA driver),
`GL_NV_draw_vulkan_image` (`glGetVkProcAddrNV` returns a function pointer),
`GL_EXT_import_sync_object` / `GL_KTX_buffer_region` / `GL_NV_feature_query`
(their entry points are not in the shipped registry),
`EGL_WL_bind_wayland_display` (needs a `wl_display` from the client) and
`EGL_NV_stream_attrib`.

Counts that depend on driver state are *measured*: `tools/audit/pname_audit.c`
probes every enum against the native driver with a sentinel buffer and
`tools/audit/merge.py` turns the results (`tools/audit/data/*.txt`) into
`src/proto/gb_pname_measured.h`, which `glGet*v` / `glGet*i_v` sizing consults
before the hand-written rules (the hand tables were wrong in 21 places).

## Upcalls and file descriptors

* **Server-to-client upcalls** (`GB_CMD_F_UPCALL`, ids in `gb_opx.h`): driver
  callbacks (which may run on driver threads) are queued per client thread and
  written to the response ring by the worker thread before every reply; the
  client delivers them in the calling thread, at the start of the next call or
  while it waits. `GL_DEBUG_OUTPUT_SYNCHRONOUS` turns asynchronous calls into
  round trips so the callback runs before the offending call returns. Blocking
  upcalls (EGL blob-cache `get`) are only issued while the client waits for a
  synchronous reply, and requests that were already queued are deferred until
  the callback returns. Used by `KHR_debug`, `ARB_debug_output`, `EGL_KHR_debug`
  and `EGL_ANDROID_blob_cache`.
* **File descriptors**: an fd named in a request travels on the control socket
  (`GB_CTL_FD_PUT`, SCM_RIGHTS) under a token that the request carries; the
  server adopts it (`gb_sv_fd_take`) and closes it after the request unless the
  driver took ownership. fds the server returns go back the same way
  (`GB_CTL_FD_GIVE`). Used by dma-buf import/export, native fences, the DRM
  master fd, `EXT_memory_object_fd`, `EXT_semaphore_fd` and EGL stream fds.
  Tested by `fd_{native,b64,b32}` (export a dma-buf from one image, import it as
  another, sample it; duplicate a native fence and wait on the copy).

## Dev-machine conformance loop (piglit on x86_64)

`~/piglit-dev` (outside the repo; recipe below) runs the Ubuntu piglit package
natively and through the bridge against the Radeon, headless, on the DRM render
node (`surfaceless_egl`, no windows, nothing on the desktop). The whole `quick`
profile is 44,224 tests and takes ~15 minutes per side, against ~5 minutes for
613 tests on the Tegra.

| Run (current library) | pass | skip | fail | crash | warn |
|---|---|---|---|---|---|
| native x86_64 | 39,202 | 4,531 | 96 | 385 | 10 |
| bridged | 39,202 | 4,531 | 96 | 385 | 10 |
| **differences** | | | | | **0** |

Recipe: `apt-get download piglit libwaffle-1-0 waffle-utils`, `dpkg -x` into a
private root, then `PIGLIT_PLATFORM=surfaceless_egl EGL_PLATFORM=surfaceless`
with `LD_LIBRARY_PATH=build/shim64 GB_SERVER=build/gbd` for the bridged side
(snapshot the library and server first so a rebuild cannot mix versions).
**`waffle-utils` (`wflinfo`) is required**: without it piglit assumes GL 0.0 and
skips ~37,000 shader tests (an earlier "0 differences" over 44k tests was
mostly skips). An i386 piglit is not available in Ubuntu, so the dev loop is a
64-bit bridge; 32-bit-specific behaviour (widening, `EGLAttrib`, long) is
covered by the loopback tests and by the Tegra runs.

Bugs this loop found that the Tegra runs had not (all fixed): calls inside
`glBegin`/`glEnd` did not raise `INVALID_OPERATION` when the shadow handled
them; client pointers could be forwarded as "server addresses" in list-compile
and Begin states (server crash); client-array draws inside display lists;
negative counts in multi-draws; context shadow state surviving
`eglDestroyContext` (a reused context id inherited the old arrays, bindings and
*extension list*: every later context saw the first one's strings); indexed
getters sized with the non-indexed table (stack overwrite in the application).

## Measured (dev machine: fast desktop CPU, Radeon RX 9060 XT, Mesa)

`tests/bench/call_overhead.c`, best of 3. These are *per-call transport
costs on trivial GPU work*, not application frame rates.

| Case | native | bridge 64->64 | bridge 32->64 |
|---|---|---|---|
| async `glUniform4f` | 34 ns | 56 ns | 48 ns |
| sync round-trip (`glGetError`) | 9 ns (cached) | 154 ns | 183 ns |
| `glDrawArrays`, VBO | 177 ns | 178 ns | 174 ns |
| `glDrawArrays`, client array | 178 ns | 861 ns | 872 ns |
| `glReadPixels` 256x256 | 7 us | 76 us (3.4 GB/s) | 81 us |

Reading the table:
* **Draw throughput is not reduced**: the server, not the transport, is the
  bottleneck, and the client enqueues faster than the server executes.
* **Sync round-trips cost ~150-200 ns** when the peer is on another core.
  When client and server share one core they cost ~8.7 us (two context
  switches). The first implementation pure-spun and cost 145 us here — see
  "Lessons".
* **Client-array draws are ~5x slower** than VBO draws (upload per draw).
* **Readback is a double copy** through the response ring (~3.4 GB/s).

None of this is a Tegra number. Re-run `bench_b32` on the device in Phase 0.

## Lessons learned (all fixed; kept here because they will recur)

1. **Spin strategy.** Pure `pause` spinning made same-core sync calls ~800x
   slower (145 us) because the waiter burned its whole time slice while the
   peer that had to answer could not run. Now: 64 pauses, 64 `sched_yield`s,
   then futex sleep. Matters on a small SoC.
2. **`EGL_PLATFORM` is inherited.** The server inherits the app's
   environment; legacy `eglGetDisplay(Display*)` *guesses* the platform and
   honours `EGL_PLATFORM`. A Plasma Wayland session exports
   `EGL_PLATFORM=wayland`, which sent an Xlib `Display*` into Mesa's Wayland
   backend and crashed the server. The server now asks for the X11 platform
   explicitly (`eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, ...)`) and the
   bridged X11 tests deliberately run with `EGL_PLATFORM=wayland` set.
3. **Size arithmetic includes alignment.** Blobs are 8-aligned; a hand-written
   size that forgot the padding was rejected by the server's bounds check (the
   check worked as designed). The generator computes layouts by simulation.
4. **A harness can lie.** An early transport test "passed" because the server
   thread called `_exit(0)`. Verify the verdict path, not just the exit code.

5. **The server's X connection is not ordered with the app's.** Xlib
   buffers `XCreateWindow`; the IPC message that makes the server render into
   that window can overtake it, giving `BadDrawable`. The client now `XSync`s
   (only when something is outstanding) before any call that names an X
   resource. Found because the real `glxgears` does not call `XSync` itself.
6. **GLVND's `glXGetProcAddress` returns a stub for any name**, so asking it
   about `eglGetPlatformDisplayEXT` yields a bogus pointer. Symbol lookup in
   the server is namespace-aware.
7. **The server must never load the bridge as its "native" GL.** With the
   `libGL.so.1` shim on `LD_LIBRARY_PATH` the server loaded *us* and spawned a
   server, recursively. The client strips its own directory from the server's
   environment and the server refuses any library exporting
   `gb_bridge_client_marker`.
9. **Draw only into an exposed window.** Rendering before the X server has
   mapped/exposed a window is undefined by the pixel-ownership rule and
   NVIDIA discards the fragments (everything reads back black). My tests
   mapped and drew immediately: ~15% flaky on the Tegra, *natively too*. They
   now wait for MapNotify + Expose and poll window readback instead of
   sleeping. 0 failures in 130 runs afterwards. Real applications process
   events first, so this never showed in glmark2/glxgears.
8. **A "passing" run on the wrong renderer proves little.** The first GLX
   results were all llvmpipe (Xvfb); only the real-display runs say anything
   about acceleration.
10. **Never clamp an unknown size to zero and still call the driver.** For
    server-sized outputs (`glGetTexImage`) a failed size query was clamped to
    0 and the driver still wrote the whole image into a 512-byte scratch,
    corrupting driver state; the crash surfaced much later in an unrelated
    `eglMakeCurrent`. Unsizable arrays now skip the call and raise a GL error.
    (Found by piglit; the faulting address was a deterministic garbage pointer
    in the driver, which is the signature of earlier corruption.)
11. **An EGL context is not necessarily GLES.** `EGL_OPENGL_API` contexts are
    desktop GL; classify the flavor by `GL_VERSION`, not by "an EGL context is
    current".
12. **Shadow state must see every spelling of a call.** `glEnable(GL_VERTEX_ARRAY)`
    is a legal alias of `glEnableClientState`; ES `EXT`/`OES`/`NV` spellings of
    draw, map and divisor calls must reach the same wrappers (now generated as
    aliases). Anything that bypasses the shadow silently desynchronises it.
13. **A driver validates names before reading arrays; a marshalling layer must
    not.** `glShaderSource(garbage_name, 1, garbage_strings, NULL)` is legal
    and yields an error natively. The client now remembers which shader names
    `glCreateShader` returned and sends an empty source for others; EGL
    attribute lists are scanned page-checked so an unterminated list raises
    `EGL_BAD_ATTRIBUTE` instead of faulting.
15. **Transfers have dimensionality.** `GL_UNPACK_SKIP_IMAGES` and
    `GL_UNPACK_IMAGE_HEIGHT` apply only to 3D uploads; a size rule that applied
    them to a 2D `glTexSubImage2D` over-read the application's buffer. The
    over-read "worked" until memory layout changed and then crashed (found as a
    regression in a later sweep: `texsubimage-unpack`). Size rules take an
    explicit "2D" marker now (`GB_DEPTH_2D`).
16. **A conformance runner can be blind.** Without `wflinfo` piglit assumed
    GL 0.0 and *skipped* ~37,000 tests, which then "matched" the bridge. Check
    the skip reasons of any suspiciously clean result.
17. **Indexed getters are not the plain getters.** `glGetIntegeri_v(
    MAX_COMPUTE_WORK_GROUP_SIZE, i)` returns one value; sizing it with the
    non-indexed table (3) made the client overwrite the application's stack.
    Counts that depend on the driver are now measured from the driver
    (pname audit), not remembered.
18. **Destroyed contexts must take their shadow with them.** Server ids are
    reused; a new context silently inherited the old one's arrays, mapped
    buffers, profile and *extension strings*.
19. **Never let a client pointer fall through to the generated forwarder.**
    The wire carries it as a "buffer offset"; the server dereferences it as
    its own address. Every state that cannot be handled client-side (inside
    Begin/End, list compilation) must raise the error or drop the call
    itself.
20. **Replies are aligned.** `gb_w_blob` pads to 8 and the reply size has to
    include the padding; a short buffer fails only for lengths that happen to
    cross the boundary (select hit lists).
14. **A hidden extension hides a whole feature, silently.** The generator only
    emitted 3 GLES extensions, so ES contexts advertised 112 of the native
    195 and `EGL_EXT_platform_base` was dropped because two of its three
    entry points were missing, which made piglit unable to obtain a display.
    Compare the advertised lists against the native driver's whenever a
    conformance test "requires" something.

## Conformance: piglit on the Tegra

Rule (from the project owner): **always run the arm64 build against the native
driver first** and treat only differences from that baseline as bridge bugs.
Selection: 613 piglit tests (`-p x11_egl`, GL 1.x/2.x, EGL, FBO,
buffer-object groups; armhf and arm64 builds of the same Ubuntu piglit
package; the armhf run goes through the bridge). Runner and triage scripts
live on the device in `~/gb/pg`.

**Hazard: native `max-texture-size` thrashes the 4 GB device** (101 s, then
the OOM killer took out plasmashell/konsole/kded5/kwin, twice). The bridged
armhf run passes it in 7.4 s (probably because the 32-bit process cannot
allocate the huge host buffers; not verified). It is excluded from every run so
that results compare, the runner makes test processes the preferred OOM victims
(`oom_score_adj=1000`), and a watchdog SIGKILLs test binaries when
`MemAvailable` < 350 MB. Run heavyweight conformance suites with these guards.

### Native baseline (4 runs of the same 613 tests)

| Run | pass | fail | skip | crash | warn |
|---|---|---|---|---|---|
| native 0 | 519 | 23 | 69 | 1 | 1 |
| native 1 | 521 | 21 | 69 | 1 | 1 |
| native 2 | 521 | 21 | 69 | 1 | 1 |
| native 3 | 520 | 22 | 69 | 1 | 1 |

610 of 613 tests give the same result in all four runs. The three that vary
are all multisample depth/stencil: `depthstencil-default_fb-clear`,
`...-clear samples=4`, `...-copypixels samples=8` (flaky natively; ignore any
bridge difference there). One stable native crash: `ppgtt_memory_alignment`
(an Intel-specific test; the bridge "passes" it, so it is not informative).

### Bridge vs the stable native result

| Bridge run | pass | fail | skip | crash | timeout | differs from native |
|---|---|---|---|---|---|---|
| first run | 446 | 78 | 80 | 9 | 1 | 92 |
| after the fixes below, run 2 | 492 | 35 | 80 | 6 | 1 | 46 |
| run 3 | 503 | 26 | 83 | 1 | 1 | 24 real |
| run 4 | 511 | 22 | 77 | 1 | 1 | 14 (one new: `texsubimage-unpack`, fixed) |
| run 5 (default vendor choice) | 515 | 22 | 74 | 0 | 1 | 10 (4 of them not defects) |

Bugs found and fixed by this sweep: see lessons 10-14, plus
`GL_BITMAP`/BGR/packed-type size tables, array state queries answered from the
shadow, client arrays with buffer-object indices, deleting mapped buffers,
`eglCreatePixmapSurface` and platform pixmap surfaces, `eglCreateImage`, and the
ES extension coverage.

**A 32-bit-only failure is not automatically a bridge bug.** Reproduce it with
the same armhf binary on software Mesa (no bridge, no NVIDIA):
`PIGLIT_PLATFORM=surfaceless_egl EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1`
with `shim32` removed from `LD_LIBRARY_PATH`. This is how
`depth-clear-precision-check` was classified (below).

### Vendor-forced comparison (current method)

Tests that fall back to Mesa/llvmpipe natively say nothing about the layer
(they usually do so because NVIDIA does not support what the test needs), so
the evaluation is done with the EGL vendor forced to NVIDIA on **both** sides:
`__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json`
(GLVND; the server inherits it). Script: `~/gb/pg/vendors.sh`.

* Native arm64, NVIDIA only: 515 pass / 21 fail / 75 skip / 1 crash / 1 warn.
* Bridged armhf, NVIDIA only: 511 pass / 21 fail / 79 skip / 1 warn / 1 timeout.
* **Excluded as native Mesa/llvmpipe fallbacks (6)**: results change when
  NVIDIA is forced: `egl-blob-cache`, `egl-ext_egl_image_storage`,
  `egl-gl_oes_egl_image` (all need `EGL_MESA_configless_context`),
  `egl_ext_device_base`, `egl_mesa_device_software`,
  `egl_mesa_platform_surfaceless`.
* **Evaluation set: 604 tests; 9 differences**, below.

Remaining 9 differences, in groups:

* **Not a defect (4):**
  * `depth-clear-precision-check`: the armhf build of the test never
    terminates. It passes `depth16` and `depth24`, then loops forever on
    `depth32` (the depth values go to garbage like -83886080.0, consistent
    with a 32-bit integer loop/overflow in the test program). The same armhf
    binary does the same on plain Mesa llvmpipe with no bridge; the arm64
    build finishes in 1.2 s. (Found by measuring: the bridged run executed ~5k
    iterations/s, i.e. the transport was fine; it was running far more
    iterations than the native 1,536.)
  * `ppgtt_memory_alignment`: crashes natively every time (Intel-specific
    test); passes through the bridge.
  * `egl_ext_platform_device`: fails natively (NVIDIA returns success where
    the test expects `EGL_BAD_NATIVE_PIXMAP`); passes through the bridge.
  * `egl_khr_fence_sync`: the test forks. Natively NVIDIA's EGL fails to
    initialize in the forked child, so the test passes through the "failed to
    initialize a second EGLDisplay" path. The bridge supports fork (the child
    reconnects to its own server), so it continues into checks that only make
    sense within one process and reports a failure.
* **Missing feature (5):**
  * `egl_khr_gl_colorspace linear` needs GLES1 (`glOrthof` not found in
    libGLESv1_CM): the bridge does not implement the GLES 1.x API (only the
    entry points it shares with desktop GL).
  * 3x `egl_khr_create_context valid debug flag gles1/2/3`: ES needs
    `GL_KHR_debug`, i.e. debug-message callbacks (needs a server-to-client
    upcall mechanism).
  * `egl_ext_image_dma_buf_import_modifiers`: dma-buf fd passing.

Fixed since the first sweep, in addition to the list above: GL_BITMAP and
packed pixel sizes, 2D transfers ignoring `GL_UNPACK_SKIP_IMAGES`, select and
feedback mode (`glSelectBuffer`/`glFeedbackBuffer`/`glRenderMode` with
server-retained buffers), EGL device enumeration/query and the device platform,
EGL wait_sync/sync_reuse/swap-with-damage/partial-update/post_sub_buffer/
system_time, the EXT_vertex_array/fog_coord/secondary_color pointer spellings
and `glGetUniformIndices`.

Not yet covered: other piglit groups (GL 3.x/4.x, GLES2/3 on the ES API,
shaders), glew-utils, the xscreensaver GL hacks, and large applications
(each needs its own arm64 baseline first).

### Full piglit profile on the Tegra (44,224 tests)

The complete `quick`-profile test list (the same list the x86_64 dev loop
runs), live on the X server `:0`, EGL vendor forced to NVIDIA on both sides,
33 resumable chunks (`~/gb/pg/full_run.sh`), arm64 native first, then armhf
bridged.

| | pass | fail | crash | skip | other |
|---|---|---|---|---|---|
| native arm64 | 37,513 | 2,852 | 12 | 3,837 | 10 warn |
| bridged armhf (first sweep) | 37,077 | 2,934 | 358 | 3,838 | 6 timeout, 1 incomplete, 10 warn |

First sweep: **492 differences (1.1%)**. Of those, 338 are not bridge
defects: all the `vs_in` double-attribute tests
(`arb_vertex_attrib_64bit`, `glsl-4.10`, `glsl-4.20`) crash with SIGBUS inside
piglit's own parser (`vertex_attrib_description::parse_datum` stores a double
through an odd address; ARM faults on the unaligned `strd`, x86 does not). They
crash the same way on plain Mesa llvmpipe with no bridge. That left about 154
real differences, which the re-runs below worked through (~one re-run of just
the difference set takes ~10 minutes: `/tmp/rerun_diff.sh <name>`).

After the fixes below, the sequential (-j1, no overlapping windows) check of
the remaining differences is: persistent buffer mappings (about 21 tests),
`max-ssbo-size` (native crashes, bridged times out: driver memory behaviour),
the 32-bit-only `depth-clear-precision-check` hang, and four open items
(`egl_khr_fence_sync` display-per-thread semantics, `ext_direct_state_access`
`named-buffers` map subtests and `vao` (native fails that one too; bridged
aborts), plus a group of tests whose result changes run to run on *both*
sides (`arb_sample_shading`, multisample default-framebuffer depthstencil,
`arb_cull_distance`, the pixel-buffer-object readbacks when two test windows
overlap under `-j2`).

What the differences turned out to be (all fixed, all with a regression check
in `tests/loopback/state_smoke.c` where it can run on the Radeon):

* **Element counts of getters/setters.** The registry gives no length for many
  DSA/EXT/OES spellings, so the count defaulted to 1: `glGetTextureParameter*`
  swizzle/border colour, `glGetVertexAttribI*iv`/`L*dv` current value,
  `glPatchParameterfv` (tessellation levels were silently 0, nothing drawn),
  `glGetMultisamplefv`, fixed-point (`x`) light/material/texenv/fog arrays,
  `glDepthRangeArrayfvOES`, sample locations, `GL_TEXTURE_CROP_RECT_OES`,
  `OES_matrix_get`. Rule now: a command with a `pname` and no/1-element length
  is sized by its pname class (`PNAME_CLASSES`).
* **Outputs written that the driver did not write.** A string getter copied the
  whole `bufSize` (overran an exactly-sized buffer: heap corruption in
  `glsl-getactiveuniform-length`); a getter that raised an error returned
  zeros over the application's variables (UBO negative tests); a readback with
  `GL_PACK_ROW_LENGTH`/skip overwrote the bytes between rows. Now: strings
  copy up to the terminator; a getter that raised an error returns no data
  (the server pops/restores the error queue around the call); sparse pixel
  packs send the application's current bytes along (`GB_PREFILL_BIT`).
* **Client-memory data the driver would never have read.** Error-path tests
  pass bad pointers on purpose; copies of application memory are now fault
  tolerant (`gb_cl_copy_in`, volatile `jmp` slot: a plain store was optimised
  away). Enabled arrays with a NULL pointer are not uploaded.
* **State the shadow keeps that the driver also needs.** Client-memory
  `*Pointer` calls are now also sent (with a NULL pointer) so the driver
  validates size/type/stride/error and tracks the state; pointer queries ask
  the driver unless the pointer is client memory. Primitive restart
  (`NV`/`3.1`/fixed index) is tracked so the restart index does not count as a
  vertex when sizing client-array uploads.
* **Indirect draws.** Compatibility-profile `glDrawArraysIndirect` etc. with
  the command structure in client memory, and indirect draws with client
  vertex arrays, are expanded into ordinary draws on the client.
* **Mapped buffers.** Mapping pointers are 256-byte aligned relative to the
  buffer start (`GL_MIN_MAP_BUFFER_ALIGNMENT`), value errors precede state
  errors, and `glBufferSubData`/`glCopyBufferSubData`/`glClearBuffer*`/
  `glGet*BufferSubData` on a mapped buffer raise `INVALID_OPERATION`;
  `BUFFER_MAPPED`-style queries work for the DSA spellings.
* **Error codes.** Unreadable/oversized uniform arrays and uniform getters with
  a bad location are `INVALID_OPERATION`, unknown sync names `INVALID_VALUE`,
  `glGetVertexAttribPointerv` checks the driver's `MAX_VERTEX_ATTRIBS`,
  indexed pointer queries accept only the indexed pnames.
* **ES contexts.** A context older than the ES version that made a function
  core only has it under its extension spelling; the server picks the
  `OES`/`EXT`/`KHR` entry point from a generated introduction table.
* **Smaller things.** `GL_BGRA` vertex arrays from client memory,
  `ARB_shading_language_include` negative lengths, `LUMINANCE_ALPHA_INTEGER`
  and friends in the pixel-size table, NULL entries in string arrays,
  `GL_QUERY_BUFFER` result pointers (an offset while a query buffer is bound),
  evaluator getters (`glGetMap*v`, `glGetnMap*v`, `glGetnPixelMap*v`).

Debugging aids added: `GB_TRACE_ERRORS=1` (server names the request after
which a GL error appeared; the error is put back), and the client prints why a
client-array draw was refused under `GB_TRACE_OPS=1`.

### Build time

The generated sources are split into 16 translation units each
(`GEN_CHUNKS`). One 4.3 MB unit took 75 s on the fast dev machine even at
`-O0` (93% of it parsing function bodies; the cost per function grows with the
size of the unit: 200 functions 0.4 s, 800 3.5 s, 1,600 24 s) and ~8 minutes on
the Tegra under load, and one unit cannot use more than one core. With the
split a full rebuild is ~22 s on the dev machine and ~3 minutes cold on the
Tegra.

## Real applications

See `docs/target-report.md` for the armhf `glmark2` (96.1% of native, all 33
scenes) and `mesa-utils-bin` results on the Tegra, and the profiling that
found and fixed the client-array upload path. `GB_STATS=1` prints per-opcode
client call/stall counts and the server's busy/idle/native-driver-time split.
`GB_SPIN_US=<n>` sets a time-bounded spin before sleeping on a reply.
`GB_TRACE_OPS=1` makes the server print one line per request (op name) on
stderr: useful for comparing a bridged call sequence with what the app does
natively.

## Known limitations / not yet done

Correctness-affecting (the app gets an error or a logged "unsupported", never
a crash or silent wrong answer):

* **Mapped buffers are shadow copies** (`gb_client_map.c`): correct for
  map/write/unmap, flush-explicit and read maps; **persistent/coherent
  mappings are refused** (they need GPU-shared memory). Deleting a mapped
  buffer unmaps it. Full list of unsupported commands: `gb_gen.py --report`.
* **Client-memory vertex arrays with a buffer-object index buffer** work but
  cost one synchronous round trip per draw (`ElementMax`: the server reads the
  index range to learn how much vertex data to upload).
* **Select/feedback mode** works (server-retained buffers returned at
  `glRenderMode`); **debug callbacks/labels**, **EGL blob cache**,
  **EGLImage incl. dma-buf import/export**, **EGL devices and the device
  platform**, **native fences** and **memory-object / semaphore fds** work
  (see "Upcalls and file descriptors").
* **`glXUseXFont`**, evaluator getters (`glGetMap*`), `GL_NV_path_rendering`,
  `glGetVkProcAddrNV` and a tail of obsolete SGIX/IBM/APPLE/INTEL/QCOM entry
  points remain unsupported (`gb_gen.py --report`).
* **Display lists compile client arrays as immediate-mode vertex data**
  (`glBegin`/`glArrayElement`/`glEnd`); a draw inside `glBegin` inside a
  `GL_COMPILE` list is dropped silently, as the drivers accept it.
* **Forked children** reconnect to their own server (a context is not shared
  across `fork`); NVIDIA's own EGL fails after fork, so the bridge is the more
  capable of the two there.
* **Only default-VAO client arrays** are shadowed (ES3 non-default VAOs pass
  straight through, which is correct for ES3).
* **Out-array writeback copies the whole capacity** (e.g. `glGetShaderInfoLog`
  overwrites the app's entire buffer with zero-padded data). Apps that
  pre-fill a buffer and rely on untouched bytes would notice.
* **Vulkan not started.**
* **GLX events** (`glXSelectEvent`), GLX pixmaps/pbuffers are passed through
  untested; **swap-control extensions** are passed through (the server's
  driver applies them).
* Extension strings are filtered to entry points we implement; extensions with
  no entry points are passed through only if listed in `EXT_NO_COMMANDS`
  (pure enums/behaviour known to be safe).

Performance (by design, not yet optimised):

* One shared-memory publish (`seq_cst` fence) per call; batching is Phase 2.
* GL object names are server-allocated (each `glGen*` is a round trip);
  client-allocated names are Phase 2.
* (Done: client-array draws now use a persistent scratch buffer with
  sub-data at a running offset.)
* Large reads/writes copy through the rings; a shared arena / dma-buf mapping
  removes a copy.

Robustness:

* `eglDestroyContext` frees the handle id immediately even if the context is
  still current (spec says deletion is deferred).
* Server crash is detected (sync calls fail fast, `EPIPE`) but contexts are
  not individually marked lost; no automatic restart.
* No fuzzing of the server decoders yet (Phase 6).

## Phase 0 checklist for the real Tegra (done; answers in target-report.md)

Items 1-4 and 6 are answered and recorded in
[target-report.md](target-report.md): NVIDIA's EGL and GLX both accept a
second X connection rendering into another connection's window, AArch32
processes run the bridge, and per-call costs were measured. Items 5 (dma-buf
export/import, mmap from AArch32) remain open and are only needed for
zero-copy work. Kept for reference — in priority order, answers decide the
design choices marked `[VERIFY]` in ARCHITECTURE.md:

1. Does `eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, ...)` exist in the
   NVIDIA EGL, and does `eglCreateWindowSurface` accept a window XID owned by
   *another* X connection? (**Q8, the Strategy X bet** — validated on Mesa
   only.) If not: GLX path, or Strategy W-X11 (DRI3 `PixmapFromBuffers`).
2. Does `/usr/lib/aarch64-linux-gnu/libEGL.so.1` (GLVND) resolve to NVIDIA's
   vendor lib when `gbd` runs, and which of `GB_NATIVE_EGL` /
   `GB_NATIVE_GLES2` need pointing at `tegra-egl`?
3. AArch32 execution: `armhf` multiarch, a static hello world, then
   `ctest` with `gb32_gl` built natively with `-mfpu=neon` on the device.
4. Re-run `bench_b32`/`bench_native`; expect much higher per-call cost on
   slower cores. The single-core number (~9 us per sync call) is the one to
   watch.
5. dma-buf export/import and mmap-from-AArch32 behaviour (Q4-Q6) before any
   zero-copy work.
6. Full extension dump (`eglinfo`, `glxinfo -B`) to size the generator's
   extension list against what apps actually probe.

## Next steps (suggested order)

1. `GL_NV_path_rendering` (83 entry points): path-name types incl. UTF-8/16
   (sized by parsing), glyph metric/spacing arrays, `COMPSIZE` rules; verify
   each count on the NVIDIA driver with an audit program like
   `tools/audit/pname_audit.c`.
2. 32-bit coverage on the dev machine: an i386 build of piglit (from source)
   would exercise the widening paths in the 44k-test loop.
3. Persistent buffer mappings and a shared arena for bulk transfers (readback
   is ~7x slower than native; mapped buffers are shadow copies).
4. Decide how armhf applications are meant to pick up the bridge system-wide
   (soname shims in `LD_LIBRARY_PATH` work today; an install story is open).
5. Performance pass (Phase 2): client-allocated names, batched publish.
6. Larger real applications (each with a native arm64 baseline first).
7. Vulkan (Phase 4): evaluate adopting the Venus protocol.

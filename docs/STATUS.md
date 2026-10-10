# gpu-bridge: implementation status

Last updated with the first working vertical slice. Read together with
[ARCHITECTURE.md](../ARCHITECTURE.md) (the design) — this file records what
actually exists, what was measured, and what is known to be missing.

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
| Generator | `tools/gb-gen/` | Two client libraries over one opcode space. `gles`: EGL 1.0-1.5 + GLES 2.0/3.0 (263 generated, 22 wrapped, 12 manual, 9 unsupported). `gl`: GLX 1.0-1.4 + GL 1.0-4.6 compat/core + ARB/KHR/EXT (~1,860 generated, 55 wrapped, 18 manual, ~97 unsupported). Explicit list via `gb_gen.py --report` |
| Server `gbd` | `src/server/` | loads real libEGL/libGLESv2/**libGL**, one worker thread per client thread, handle tables, scratch buffers for client arrays, own X connection(s) with `XInitThreads` + an error handler, server-side size rules that query native GL state |
| Client libs | `src/client/` | `libgb{32,64}_gl.so` = EGL + GLES; **`libGL.so.1` (built into `build/shim{32,64}/`) = GLX + desktop GL**, a drop-in replacement for the system libGL |

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
output). `GB_DEBUG=1`-free: just read `GL_RENDERER`.

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

Not supported (stub + log with `GB_LOG_MISSING=1`, never a crash): mapped
buffers (`glMapBuffer*`), debug-message callbacks/labels, `glGetBufferPointerv`,
evaluator getters (`glGetMap*`), `glFeedbackBuffer`/`glSelectBuffer` (retained
client pointers), `glXUseXFont`, vendor-specific extensions beyond
ARB/KHR/EXT, and the 3.0 imaging-subset getters.

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
8. **A "passing" run on the wrong renderer proves little.** The first GLX
   results were all llvmpipe (Xvfb); only the real-display runs say anything
   about acceleration.

## Known limitations / not yet done

Correctness-affecting (the app gets an error or a logged "unsupported", never
a crash or silent wrong answer):

* **No `glMapBufferRange`/`glMapBuffer`** (mapped buffers need shared
  memory with write-back semantics; Phase 2), **no client arrays combined with
  buffer-backed indices**. Full list: `gb_gen.py --report`.
* **`glXUseXFont`**, **GL debug callbacks/labels**, evaluator getters,
  vendor extensions beyond ARB/KHR/EXT.
* **No EGLImage / dma-buf import** (`eglCreateImage*` is unsupported), so no
  `GL_OES_EGL_image*`. Needed for video/zero-copy; Phase 2.
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
  no entry points (pure enums/behaviour) pass through unreviewed.

Performance (by design, not yet optimised):

* One shared-memory publish (`seq_cst` fence) per call; batching is Phase 2.
* GL object names are server-allocated (each `glGen*` is a round trip);
  client-allocated names are Phase 2.
* Client-array draws recreate buffer data every draw; should use a persistent
  ring of scratch space.
* Large reads/writes copy through the rings; a shared arena / dma-buf mapping
  removes a copy.

Robustness:

* `eglDestroyContext` frees the handle id immediately even if the context is
  still current (spec says deletion is deferred).
* Server crash is detected (sync calls fail fast, `EPIPE`) but contexts are
  not individually marked lost; no automatic restart.
* No fuzzing of the server decoders yet (Phase 6).

## Phase 0 checklist for the real Tegra (not doable on the dev machine)

In priority order — answers decide the design choices marked `[VERIFY]` in
ARCHITECTURE.md:

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

1. Phase 0 on the Tegra (see the checklist above): the GLX path adds one
   more question — does NVIDIA's GLX accept a second connection rendering to
   a window another connection created (it works on Mesa/Xvfb and Mesa/
   XWayland)? Test with `glx_xconn`-style code before trusting it.
2. Performance pass (Phase 2): client-allocated names, batched publish,
   persistent client-array scratch, shared-arena bulk transfers.
4. Vulkan (Phase 4): evaluate adopting the Venus protocol.

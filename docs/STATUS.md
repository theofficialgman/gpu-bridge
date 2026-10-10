# gpu-bridge: implementation status

Last updated with the first working vertical slice. Read together with
[ARCHITECTURE.md](../ARCHITECTURE.md) (the design) — this file records what
actually exists, what was measured, and what is known to be missing.

## Build and test

```
sudo apt install gcc-multilib libc6-dev-i386     # once, for the -m32 builds
cmake -S . -B build -G Ninja && ninja -C build
ctest --test-dir build                            # 11 tests
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

## What exists

| Component | Files | Notes |
|---|---|---|
| Ring buffer | `src/proto/gb_ring.[ch]` | futex wake, pause -> yield -> sleep wait strategy, peer-death detection via control socket POLLHUP |
| Channel + messages | `src/proto/gb_chan.[ch]` | request+response ring in one memfd; transparent BIG/CHUNK for messages > ring/4 |
| Control protocol | `src/proto/gb_ctl.[ch]` | socketpair + SCM_RIGHTS; server lifetime tied to client; version + op-table hash check at HELLO |
| Codec | `src/proto/gb_codec.h` | bounds-checked reader/writer, fixed-width only |
| Size rules | `src/proto/gb_gl_sizes.h` | pname counts, pixel rectangle sizes, EGL attrib list validation; unknown -> -1, never a guess |
| Generator | `tools/gb-gen/` | EGL 1.0-1.5 + GLES 2.0/3.0 + a few extensions from the Khronos XML. 248 generated, 19 wrapped, 14 manual, 25 unsupported (explicit list via `gb_gen.py --report`) |
| Server `gbd` | `src/server/` | loads real libEGL/libGLESv2, one worker thread per client thread, handle tables, scratch buffers for client arrays |
| Client lib | `src/client/` | `libgb32_gl.so` / `libgb64_gl.so` export EGL + GLES2/3 |

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

## Known limitations / not yet done

Correctness-affecting (the app gets an error or a logged "unsupported", never
a crash or silent wrong answer):

* **No PBOs** (`GL_PIXEL_PACK/UNPACK_BUFFER`): detected client-side, logged,
  `GL_INVALID_OPERATION`. A pixel-buffer offset is not a pointer.
* **No `glMapBufferRange`/`glMapBuffer`**, **no `glGetUniform*v`**,
  **no 3D textures**, **no client arrays + buffer-backed indices**,
  **no instanced draws with client arrays**. Full list: `gb_gen.py --report`.
* **No EGLImage / dma-buf import** (`eglCreateImage*` is unsupported), so no
  `GL_OES_EGL_image*`. Needed for video/zero-copy; Phase 2.
* **Only default-VAO client arrays** are shadowed (ES3 non-default VAOs pass
  straight through, which is correct for ES3).
* **Out-array writeback copies the whole capacity** (e.g. `glGetShaderInfoLog`
  overwrites the app's entire buffer with zero-padded data). Apps that
  pre-fill a buffer and rely on untouched bytes would notice.
* **Desktop GL / GLX not started.** Only EGL + GLES.
* **Vulkan not started.**
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

1. GLVND vendor split (`libEGL_gb32`, `libGLX_gb32`) so apps need no
   `LD_LIBRARY_PATH`; today the client lib exports EGL+GLES directly.
2. Desktop GL + GLX (Phase 3): `glxgears`-class apps are the likely real
   workload on an X11-only Tegra.
3. Performance pass (Phase 2): client-allocated names, batched publish,
   persistent client-array scratch, shared-arena bulk transfers.
4. Vulkan (Phase 4): evaluate adopting the Venus protocol.

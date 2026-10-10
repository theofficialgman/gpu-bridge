# arm32 → arm64 GPU API Remoting Layer (Tegra)

Working name: **`gpu-bridge`** (client: `gb32`, server: `gbd`).
Status: **first vertical slice implemented and passing** — see
[docs/STATUS.md](docs/STATUS.md) for what exists, measurements, lessons and
known gaps. This document remains the design; where the code deliberately
differs from it, the difference is listed under "Deviations from this
design" at the end.

---

## 0. How to read this document

Claims are tagged by confidence:

| Tag | Meaning |
|---|---|
| **[FACT]** | Follows from the architecture of Linux/arm64/the Khronos APIs. Safe to rely on. |
| **[ASSUMED]** | Believed true for NVIDIA Tegra/L4T but not checked on the target. Verify in Phase 0. |
| **[VERIFY]** | Unknown and decision-relevant. Phase 0 must answer it; the design has a fallback. |

The target hardware is **not** the machine this document was written on. Do
not assume you can run Tegra code locally. Section 9 describes a dev loop that
works on any x86-64 Linux box, and Phase 0 lists what must be measured on the target.

---

## 1. Goal and scope

### Goal
Run unmodified **arm32 (AArch32/armhf) Linux applications** with **hardware
accelerated** OpenGL / OpenGL ES / EGL (and later Vulkan) on a system whose
only GPU userspace driver is the **closed-source NVIDIA Tegra (L4T) arm64**
stack, running under a 64-bit kernel that can execute AArch32 binaries.

### Non-goals (v1)
- CUDA, NVENC/NVDEC, VIC, NvMedia, V4L2 multimedia remoting.
- Running arm32 apps that talk to NVIDIA device nodes directly (`/dev/nvmap`, `/dev/nvgpu/*`).
- Modifying, patching, or redistributing NVIDIA binaries.
- Multi-user / sandbox-grade security isolation (same-user trust model; see §5.10).
- Network transparency (local machine only).
- **Wayland** (the target runs X11 only — user-confirmed). Wayland/Strategy W
  is kept in the doc as a deferred option, not a v1 deliverable.

### Success criteria
1. `es2gears`, `glxgears`, `glmark2-es2` (armhf builds) render correctly on the target using the Tegra GPU.
2. `glmark2-es2` score ≥ **70 %** of the same benchmark built natively arm64 on the same device (target; tune later).
3. A 32-bit app never loads or opens any NVIDIA library or device node, and everything it needs is provided by the stub libs + generic kernel interfaces.
4. `vkcube` (armhf) runs through the Vulkan path (Phase 4).
5. Server crash or app crash cleans up with no leaked GPU memory, shm, or fds.

---

## 2. Why this has to be out-of-process  [FACT]

1. An AArch32 process cannot execute AArch64 instructions or `dlopen` an
   AArch64 `.so`. Execution state only changes via exception-level transitions
   through the kernel; there is no EL0 "far jump" (unlike x86 Heaven's Gate).
   **No in-process thunking is possible** while the app runs natively as AArch32.
2. The kernel side is not the problem: a 64-bit kernel already serves 32-bit
   processes. The missing piece is the 32-bit *userspace* driver, which we
   can't build because the source is closed.
3. Therefore: put the real driver in a 64-bit process and forward API calls to it
   over IPC. Cut at the **API boundary** (EGL/GLES/GL/GLX/Vulkan), because it is
   a stable, public, well-specified interface. The ioctl boundary is undocumented
   and proprietary.

**Invariant I-1:** the 32-bit side uses only *generic* kernel interfaces:
`memfd`, `mmap` of dma-buf fds, Unix sockets with `SCM_RIGHTS`, `futex`/`eventfd`,
`sync_file` fds, plus its own 32-bit libwayland/libxcb. It never opens `/dev/nvmap`,
`/dev/nvgpu/*`, `/dev/dri/*` for rendering, or loads `libnv*.so`.

---

## 3. Target platform facts

All items below are **[ASSUMED]** unless stated; confirm in Phase 0.

- Jetson Linux (L4T) arm64 userspace; NVIDIA libs live under
  `/usr/lib/aarch64-linux-gnu/tegra/` and `/usr/lib/aarch64-linux-gnu/tegra-egl/`.
  Typical contents: `libnvrm_gpu.so`, `libnvrm_mem.so`, `libnvos.so`,
  `libnvidia-eglcore.so`, `libnvidia-glcore.so`, `libnvidia-glsi.so`,
  `libEGL_nvidia.so.0`, `libGLESv2_nvidia.so.2`, `libGLX_nvidia.so.0`,
  `libnvidia-egl-wayland.so`, Vulkan ICD JSON + `libnvidia-vulkan-*.so`.
- **GLVND** is used: vendor libs register through
  `/usr/share/glvnd/egl_vendor.d/*.json` and GLX vendor name `nvidia`.
- Kernel: `nvgpu` (GPU), `nvmap` (memory; dma-buf exportable), `nvhost`/host1x,
  `tegra-udrm`/`nvidia-drm` (display, presence varies by L4T release).
- **Integrated GPU with unified memory**; no discrete VRAM. This is very favorable
  for zero-copy sharing: GPU buffers can be dma-buf exported and CPU-mapped by
  any process.
- GL: desktop GL 4.x and GLES 3.x; EGL with NVIDIA-specific extensions (EGLStream
  family, `EGL_EXT_device_*`, `EGL_EXT_platform_device`); Vulkan 1.x on newer L4T.
- **Display stack is X11 only** (user-confirmed; NVIDIA X driver + GLX). This
  makes Strategy X (§5.7) the primary presentation path and removes Wayland
  from v1. (For reference: Wayland on Tegra goes through `egl-wayland`,
  EGLStreams historically.) Q2 is therefore answered; Q8 (a second X connection
  rendering to a window owned by the client connection) is now the critical
  Phase 0 check.

### Phase 0 questions the target must answer  [VERIFY]

| # | Question | Why it matters |
|---|---|---|
| Q1 | Exact L4T release, kernel version, GPU (TX1/Xavier/Orin…), 64-bit userspace distro/version | Determines GL/Vulkan versions and available extensions |
| Q2 | Is the display server X11, Wayland (which compositor), or none (KMS/EGLOutput)? | Selects presentation strategy (§5.7) |
| Q3 | Can the kernel run AArch32 (`CONFIG_COMPAT`, `CONFIG_ARM64_AARCH32`, check `/proc/sys/abi`, run a static armhf binary)? Is `armhf` multiarch installable (libc6:armhf, libglvnd:armhf, libx11:armhf, libwayland:armhf, libvulkan1:armhf)? | Baseline feasibility |
| Q4 | Can the NVIDIA EGL export a renderable buffer as a **dma-buf fd** (`EGL_MESA_image_dma_buf_export`, GBM via `libnvidia-allocator`, `NvBufSurface`, or Vulkan `VK_EXT_external_memory_dma_buf`)? | Decides zero-copy vs. readback presentation |
| Q5 | Can the NVIDIA EGL/GL **import** a dma-buf (`EGL_EXT_image_dma_buf_import`, `EGL_EXT_image_dma_buf_import_modifiers`, `NvEGLImageFromFd`)? | Needed for uploads/zero-copy textures |
| Q6 | Does CPU `mmap` of a Tegra dma-buf work from an **AArch32** process, and what are its cache semantics (WC vs cached; is `DMA_BUF_IOCTL_SYNC` honored)? | Decides mapped-buffer design (§5.6) |
| Q7 | Does `sync_file` / `EGL_ANDROID_native_fence_sync` / `EGL_KHR_fence_sync` + `EGL_ANDROID_native_fence_sync` exist? Vulkan external semaphore/fence fd support? | Decides synchronization strategy (§5.8) |
| Q8 | Which X11 path works for a second Xlib connection rendering to a window created by another connection (XID sharing, §5.7)? GLX and EGL-on-X11 both? | Validates the primary presentation trick |
| Q9 | Full list of EGL/GL/GLES/Vulkan extensions the native driver exposes (`eglinfo`, `glxinfo -B`, `vulkaninfo`) | Defines the advertise-list (§6) |
| Q10 | Per-call native overhead baseline: `glmark2-es2` and an empty-draw-call microbench built arm64 | Needed to quantify the 70 % target |

Phase 0 deliverable: `docs/target-report.md` with raw command outputs and answers
to Q1–Q10. **Do not start Phase 1 without Q1–Q3 answered.**

---

## 4. Architecture overview

```
 arm32 process (AArch32)                         arm64 process (AArch64)
┌─────────────────────────────────┐            ┌──────────────────────────────────┐
│ unmodified app                  │            │ gbd  (render server, 1 per app)  │
│   │                             │            │                                  │
│   ├ libglvnd:armhf (distro)     │            │  ┌ decoder (generated)           │
│   │   ├ EGL vendor: libEGL_gb32 │  shm ring  │  ├ object/handle tables          │
│   │   ├ GLX vendor: libGLX_gb32 │◄──────────►│  ├ memory manager (dma-buf/shm)  │
│   │   └ libGLESv2/OpenGL stubs  │  + eventfd │  ├ sync manager                  │
│   ├ Vulkan loader:armhf         │            │  └ presentation helper           │
│   │   └ ICD: libvulkan_gb32     │ unix sock  │        │                         │
│   ├ libX11/xcb:armhf (distro)   │ SCM_RIGHTS │        ▼                         │
│   └ libwayland-client:armhf     │ (dma-buf,  │  real NVIDIA arm64 libs          │
│                                 │  sync fds) │  (libEGL_nvidia, libGLX_nvidia,  │
└───────────┬─────────────────────┘            │   Vulkan ICD) via dlopen         │
            │ X11 / Wayland protocol           └───────────────┬──────────────────┘
            ▼ (arch-neutral)                                   │ nvmap / nvgpu ioctls
   X server / compositor (64-bit)                   Tegra kernel driver (arm64)
```

### Components

| Component | Arch | Role |
|---|---|---|
| `libEGL_gb32.so`, `libGLX_gb32.so`, `libGLESv2_gb32` / `libOpenGL_gb32` | arm32 | GLVND vendor implementations. Serialize calls, own client-side state. |
| `libvulkan_gb32.so` + ICD JSON | arm32 | Vulkan ICD loaded by the stock 32-bit Vulkan loader. |
| `gbd` | arm64 | One server per client app. Links nothing from NVIDIA at build time; `dlopen`s the native vendor libs at run time. |
| `gb-launch` | arm64 | Spawns `gbd` on demand (socket activation / fork-exec by the stub on first `eglGetDisplay`). |
| `gb-gen` | host | Code generators (Python) producing encoders, decoders, and handle plumbing from Khronos XML. |
| `libgb-proto` | both | Shared wire format, ring buffer, framing. **Compiled for both archs from the same source.** |

### Process model
- **One `gbd` per client process** (fork-exec on first use; exits when the
  control socket closes). Simplest isolation: no cross-app GL state leaks, a crash
  of one app does not kill others, and GPU resources die with the process.
- The client can fork: child processes must re-establish their own `gbd`
  connection (`pthread_atfork` handler invalidates inherited state).

---

## 5. Component specifications

### 5.1 Client-side integration points

Do **not** reimplement top-level dispatch libraries. Plug into the standard ones:

- **EGL / OpenGL / GLES:** install the distro's `libglvnd:armhf`
  (`libEGL.so.1`, `libGLdispatch.so.0`, `libGLX.so.0`, `libOpenGL.so.0`,
  `libGLESv2.so.2`, `libGL.so.1`). Provide an **EGL vendor** JSON
  (`/usr/share/glvnd/egl_vendor.d/` — armhf path) pointing to `libEGL_gb32.so.0`,
  and a **GLX vendor library** named `gb32` (selected via
  `__GLX_VENDOR_LIBRARY_NAME=gb32` or by the server's GLX vendor string).
  Implement the GLVND vendor interfaces (`__eglMustCastToProperFunctionPointerType`
  / `__egl_Main`, `__glx_Main`, `GLdispatch` entry-point table).
- **Vulkan:** implement the **ICD** interface
  (`vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`,
  `vk_icdGetPhysicalDeviceProcAddr`), manifest in the armhf ICD directory. The
  stock 32-bit loader handles layers and instance dispatch.
  Dispatchable objects (`VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`,
  `VkCommandBuffer`) must be client-allocated structs whose first word is the
  loader dispatch pointer (`set_loader_magic_value`); they additionally hold the
  remote object ID.
- **Windowing:** use the distro's 32-bit `libX11`/`libxcb`/`libwayland-client`
  unmodified. Window-system protocol is architecture-neutral on the wire.

### 5.2 Wire protocol and transport

**Channels** (per client app):

1. **Control socket** (`AF_UNIX`, `SOCK_SEQPACKET`): handshake, fd passing
   (`SCM_RIGHTS`), teardown, rare/large requests. Path:
   `$XDG_RUNTIME_DIR/gpu-bridge/<pid>-<nonce>.sock` mode `0600`.
2. **Command ring** (shared memory, one per client thread): single-producer
   (client) / single-consumer (server) lock-free ring in a `memfd`, wakeups by
   `eventfd` (or `futex` on the ring head/tail words). Default 4 MiB; configurable.
3. **Reply ring** (same memfd, second region): server → client. Client spins
   briefly (~10–20 µs), then futex-waits.

**Framing:**

```
struct gb_cmd_hdr {        // 16 bytes, all little-endian, naturally aligned
    uint32_t opcode;       // generated enum, stable across versions via registry
    uint32_t size;         // total bytes incl. header, multiple of 8
    uint32_t seq;          // monotonically increasing per-thread
    uint32_t flags;        // bit0: WANT_REPLY, bit1: HAS_FDS, bit2: BARRIER ...
};
// payload: fixed-width fields only; variable data appended, 8-byte padded
```

**Rules (Invariant I-2):** the wire format contains **no** `long`, `size_t`, `void*`,
`intptr_t`, or `ptrdiff_t`. Every pointer-width quantity is marshalled as
`uint64_t` (sizes, offsets, `GLsizeiptr`, `GLintptr`, `EGLAttrib`). Generators
must emit `static_assert`s on struct sizes and offsets, and the same
`libgb-proto` headers must compile identically for 32- and 64-bit.

**Handshake:** magic, protocol version, client arch/ABI (`armhf`), feature bitmap,
server's advertised API versions. Mismatch ⇒ fail EGL initialization with
`EGL_NOT_INITIALIZED`.

**Call classes** (assigned per entry point by the generator, overridable in a
hand-maintained `overrides.yaml`):

| Class | Behavior | Examples |
|---|---|---|
| `ASYNC` | Written to ring, no wait | `glDrawArrays`, `glUniform*`, `glBindTexture`, `vkCmd*` |
| `SYNC` | Flush ring, wait for reply | `glGetIntegerv`, `glReadPixels`, `eglMakeCurrent`, `vkCreateDevice` |
| `LAZY_RET` | Client-side generated ID/handle, async creation | `glGenTextures` (client allocates names; see §5.5) |
| `LOCAL` | Answered from client cache, no IPC | `glGetError` (after cache), `glGetString`, `eglQueryString`, `glGetIntegerv` for constant caps |

Errors: GL errors raised by async calls are recorded server-side in the thread's
state and fetched on `glGetError` (SYNC). Optionally, in debug mode, make every
call SYNC to pinpoint failing calls.

### 5.3 Code generation  (`gb-gen`)

Inputs: Khronos `gl.xml`, `egl.xml`, `glx.xml`, `vk.xml` (vendor a pinned copy under
`registry/`, record commit hashes). Outputs, for each API:

- Client encoder stubs (arm32) and dispatch tables.
- Server decoder + dispatcher (arm64), resolving the native function via
  `dlsym` / `eglGetProcAddress` / `vkGet*ProcAddr`.
- Wire struct definitions (+ static asserts), opcode enum, type-width table.
- For Vulkan: `pNext`-chain (de)serializers keyed on `sType`, handle-translation
  code, and array length expressions from `len=` / `altlen=` attributes.

Generator rules:
- `len="COMPSIZE(...)"` entries in `gl.xml` need hand-written length functions in
  `src/gen/gl_compsize.py`. Unknown ⇒ generator **fails loudly**; never guess a size.
- Entry points not on the allow-list (§6) are generated as stubs returning
  `GL_INVALID_OPERATION` / `VK_ERROR_EXTENSION_NOT_PRESENT` and removed from the
  advertised extension string.
- **Reuse prior art for Vulkan:** Mesa's *Venus* protocol
  (`virglrenderer/venus-protocol`, Mesa `src/virtio/vulkan`) already defines
  a complete generated Vulkan serialization and has a server-side decoder
  (`virglrenderer` `vkr`). Evaluate (Phase 4a) whether to **adopt the Venus wire
  protocol + generated code** (replacing our own for Vulkan) and write only our
  transport/WSI/memory glue. Licenses are MIT.

### 5.4 Server (`gbd`)

- Loads native libs with `dlopen` using paths discovered at startup
  (`/usr/lib/aarch64-linux-gnu/tegra*/`, via `ldconfig -p` / GLVND JSON). Must
  not require the 64-bit GLVND to be the one the app would choose; call the
  vendor `libEGL_nvidia.so.0` / `libGLX_nvidia.so.0` directly if that is more
  robust **[VERIFY]**.
- Threads: one server worker thread per client thread that has a ring (§5.9).
- Owns all native objects: `EGLDisplay`, contexts, surfaces, GL object names,
  Vulkan instance/device, dma-buf allocations.
- Per-client resource accounting; on connection close, destroy everything in
  reverse dependency order (`eglTerminate`, `vkDestroyDevice`, close fds).
- Logging: ring-buffered trace (`GB_TRACE=1`) that can dump a replayable
  call stream (useful for debugging without the app).

### 5.5 Object and handle model

- **GL names** (`GLuint`): client may allocate names itself (cheap `glGen*`)
  from a per-context namespace and tell the server to bind them; or allocate
  names via a batched `glGen*` that returns a pre-fetched block. Choose the
  client-allocated, server-validated scheme: no round-trip per `glGenTextures`.
  The server maintains a map `client_name → native_name`; never trust that they
  are identical.
- **EGL/GLX/GL opaque pointers** (`EGLDisplay`, `EGLContext`, `EGLSurface`,
  `EGLConfig`, `EGLImage`, `EGLSync`, `GLsync`, `GLXContext`, …): the client sees
  32-bit **opaque IDs** (non-zero, allocated by the server, tagged with an
  object-type nibble to catch misuse). Server maintains `id → native pointer`.
- **Vulkan dispatchable handles:** client structs with loader magic + remote ID.
  **Non-dispatchable handles** are `uint64_t` on both sides: use a server-minted
  64-bit ID (not the raw native handle, to keep the server free to validate).
- Lifetime: reference-counted per ID; destroy messages remove entries; double
  destroy / unknown ID ⇒ API error, never a server crash.

### 5.6 Memory model

Everything that crosses the boundary falls into one of these cases:

| Case | Strategy |
|---|---|
| **Small inline data** (uniforms, small `glBufferSubData`) | Copy into the command ring |
| **Client-memory vertex arrays / indices** (`glVertexAttribPointer` w/o VBO, `glDrawElements` w/ pointer) | Client tracks array state; at draw time computes the used range (`first+count`, or scan indices for `max`) and uploads just that range inline, with a per-draw cache keyed on pointer+range+content hash for repeat draws |
| **Large uploads** (`glTexImage2D`, `glBufferData`, `vkCmdCopy*` from staging) | Write into a shared staging region (memfd arena) then send `(offset,len)`; server issues the native upload from the same pages (no second copy) |
| **Mapped buffers** (`glMapBufferRange`, `vkMapMemory`) | Preferred: server allocates the backing store as an **exportable dma-buf**, passes the fd, client `mmap`s it (zero-copy). Fallback: client-side shadow buffer + flush/invalidate ranges (virgl-style). `DMA_BUF_IOCTL_SYNC` start/end around CPU access if Q6 shows non-coherent caching |
| **Readback** (`glReadPixels`, `glGetBufferSubData`, `vkCmdCopyImageToBuffer` + map) | Server writes into the shared arena; client reads it after the reply. Avoid sending large data through the reply ring |
| **Shared images** (EGLImage, dma-buf import/export, window buffers) | dma-buf fds over `SCM_RIGHTS`; both sides import natively |

Constraints:
- The arm32 client has a 3–4 GiB virtual address space. Cap total shared mappings
  (default: 1 GiB per client) and fail with `GL_OUT_OF_MEMORY` / `VK_ERROR_OUT_OF_HOST_MEMORY`
  rather than crash. Prefer sparse/lazy mapping and unmap aggressively.
- Alignment and padding must be identical on both sides (`libgb-proto`).
- Use `madvise`/`MAP_POPULATE` only where benchmarks justify it.

### 5.7 Presentation (windowing system integration)

This is the most Tegra-specific part. Design for **two strategies** selected by
Phase 0 results (Q2, Q4, Q8) and a universal fallback.

#### Strategy X: server renders directly to X11 windows (primary for X11)

Key insight **[FACT]**: an X11 `Window` is a 32-bit XID valid on the X server,
not a per-connection pointer. Another connection to the same display can use
that XID.

1. Client stub resolves `Display*` → connection string (`DisplayString()`),
   sends it in `eglGetPlatformDisplay`/`glXChooseVisual` etc.
2. `gbd` opens its **own** 64-bit Xlib/xcb connection to the same display.
3. `eglCreateWindowSurface(dpy, cfg, XID)` / `glXCreateWindow` run in the server
   with the XID from the client. Native driver presents using its own X path
   (GLX/NVIDIA private protocol or DRI3/Present).
4. Client app continues to own the window (events, resize, input) through its
   own 32-bit Xlib. Resize: client notifies server on `ConfigureNotify` (the stub
   can select input on the window itself via its own connection, or intercept
   `eglSwapBuffers`/`glXSwapBuffers` and query the window size via `XGetGeometry`).
5. `glXSwapBuffers`/`eglSwapBuffers` = SYNC call with optional fence for pacing.

Pitfalls: visual/colormap/depth must match between client-created window and
the server's `EGLConfig` / `GLXFBConfig`. Stub must translate the client's
`XVisualInfo*` / `GLXFBConfig` choice into the right native config ID and keep
client-visible `visualid`s consistent. Test both GLX and EGL-on-X11.

#### Strategy W: client presents via dma-buf (Wayland, and X11 fallback)

Needed when the server cannot attach to the client's `wl_surface` (Wayland object
IDs are per-connection) **[FACT]**.

1. Stub implements the **EGL Wayland platform** itself (like Mesa
   `platform_wayland`): it wraps `wl_egl_window`, and creates buffer queues.
2. Server allocates N swapchain images as exportable buffers (Q4 path) and sends
   dma-buf fds with format/modifier/stride.
3. Client creates `wl_buffer`s with `zwp_linux_dmabuf_v1`, attaches/commits on its
   own `wl_surface`; honors `wl_buffer.release` and (if available)
   `linux-drm-syncobj` explicit sync or implicit sync via dma-buf fences.
4. Frame callbacks drive swap interval; `eglSwapBuffers` = server finishes
   rendering into image *i* → client commits it → returns when next image free.
5. For X11 fallback: same, but present with DRI3 `PixmapFromBuffers` + Present.

Compositors on Tegra may not support dma-buf with NVIDIA modifiers **[VERIFY]**.
If the compositor does not accept the buffer, fall back to the next strategy.

#### Strategy R: readback present (universal fallback; Phase 1 baseline)

Server renders to an offscreen FBO/pbuffer and copies pixels into a shared memfd
arena; the client presents them with `wl_shm` or `XPutImage`/`xcb_shm_put_image`
(MIT-SHM). Slow (one GPU→CPU and CPU→compositor copy per frame) but works
everywhere. **Build this first**: it proves the rest of the stack.

#### Strategy K: no window system (KMS / EGLOutput)

If the app uses GBM/KMS directly, the DRM fd is generic and 32-bit-compatible
at the kernel level, but the buffers are driver-allocated. Out of scope for v1;
revisit after X11/Wayland.

### 5.8 Synchronization

- Within one client thread, ring order == execution order on the server thread.
- **Cross-thread / cross-context:** `glFlush`/`glFinish`, `eglWaitClient`,
  `eglWaitGL`, `glFenceSync` are mapped to server-side equivalents; sync objects
  are server-owned with client IDs.
- **Cross-process (client ↔ compositor):** export `sync_file` fds from the
  server (`EGL_ANDROID_native_fence_sync` / Vulkan `VK_KHR_external_*_fd` with
  `SYNC_FD`) and pass to the client for Wayland explicit sync or Present
  fences **[VERIFY Q7]**. Fallback: server does `glFinish`/`vkQueueWaitIdle`
  before returning from swap (correct, slower).
- **Timeline:** `vkWaitForFences` etc. blocks the *client* thread on a reply; use
  server-side wait with timeout, never busy-wait over IPC.

### 5.9 Threading

- Each client thread that makes an API call gets its own ring pair + server worker.
  Native GL contexts are thread-affine; `eglMakeCurrent` on thread T binds on the
  paired worker. A context moved between threads ⇒ server migrates it by issuing
  `eglMakeCurrent(NO_CONTEXT)` on the old worker then binding on the new
  (client stub serializes the handoff with a barrier).
- Thread-local state (current context, last error, Vulkan per-thread command
  pools) lives in client TLS **and** is mirrored in the server worker.
- Vulkan: command buffers recorded on any client thread; serialized into that
  thread's ring; `vkQueueSubmit` is a barrier-ordered SYNC/ASYNC (configurable).

### 5.10 Failure, lifecycle, security

- **Server dies:** client detects EOF on control socket / ring timeouts → marks
  all contexts lost (`EGL_CONTEXT_LOST`, `VK_ERROR_DEVICE_LOST`), subsequent
  calls return errors quickly; never block forever. Optional automatic server
  restart is a non-goal for v1.
- **Client dies:** `gbd` sees EOF, tears down native objects, exits.
- **Trust model:** same UID, same user session. The server still **validates
  every client-supplied length, offset, ID and enum** (it is the more
  privileged-in-GPU-access side). Fuzz the decoder (Phase 6). No setuid, no
  capabilities.
- Socket directory `0700`; reject peers with a different `SO_PEERCRED` UID.
- Resource limits: max ring size, max shared mappings, max objects per client.

---

## 6. API coverage plan

Advertise **only** what is implemented and tested. Everything else is hidden from
`eglQueryString`, `glGetString(GL_EXTENSIONS)`, `vkEnumerate*ExtensionProperties`.

| Tier | Scope | Phase |
|---|---|---|
| T0 | EGL 1.4 core, GLES 2.0 core, pbuffer/offscreen, `eglSwapBuffers` via Strategy R | 1 |
| T1 | GLES 3.0–3.2 core, VAO/FBO/UBO/TF/instancing, `EGL_KHR_fence_sync`, `EGL_KHR_image_base` | 2 |
| T2 | GLX 1.4 + `GLX_ARB_create_context*`, desktop GL 3.3/4.x core + compat as needed by target apps | 3 |
| T3 | Vulkan 1.1–1.3 core, WSI (`VK_KHR_surface`, `_xcb_surface`/`_xlib_surface`/`_wayland_surface`, `_swapchain`) | 4 |
| T4 | Extension long tail: `EGL_EXT_image_dma_buf_import`, `GL_OES_EGL_image`, `GL_OES_EGL_image_external`, `GL_EXT_*` demanded by real apps | ongoing |

Selection method for T4: run target apps with `GB_LOG_MISSING=1`, which logs
every unimplemented entry point/extension queried or called, then implement in
order of frequency.

---

## 7. Performance design rules

1. **Don't round-trip.** Any SYNC call on a hot path is a bug until proven necessary.
2. **Batch.** Ring writes are plain stores; one doorbell per flush, not per call.
   Flush on: SYNC call, swap, ring ≥ 75 % full, `glFlush`, idle timer (≈ 100 µs).
3. **Cache constants client-side.** `glGetString`, `glGetIntegerv(GL_MAX_*)`,
   `eglQueryString`, config attribs, `glGetShaderPrecisionFormat`, etc.
4. **Client-side state shadow** for pointer/array state, active texture,
   bound buffers, viewport, enables: needed for client-array uploads and to
   drop redundant calls (and to answer `glGet*` locally).
5. **Zero-copy for bulk data** via memfd/dma-buf arenas (§5.6).
6. **Wake-up cost:** spin-then-futex; pin `gbd` worker threads' priority equal to
   the client's; measure `eventfd` vs `futex` on the target.
7. **Shader/program compilation latency:** make `glCompileShader`/`glLinkProgram`
   ASYNC with status fetched lazily on `glGetShaderiv/GetProgramiv` (as most
   remoting systems do). Forward `glGetProgramBinary` as SYNC.
8. Build an internal profiler: per-opcode counters, bytes, stalls, wait time
   (`GB_STATS=1`).

---

## 8. Repository layout (proposed)

```
gpu-bridge/
  ARCHITECTURE.md              # this file
  docs/
    target-report.md           # Phase 0 output
    protocol.md                # wire format spec (generated + hand-written parts)
    decisions/NNNN-*.md        # ADRs
  registry/                    # pinned Khronos XML + overrides.yaml
  tools/gb-gen/                # Python generators
  src/
    proto/                     # libgb-proto (ring, framing, types) — both archs
    client/{egl,glx,gles,gl,vk,wsi}/
    server/{core,egl,gl,vk,mem,sync,wsi}/
    launch/                    # gb-launch / socket activation
  tests/
    unit/                      # ring buffer, codec, handle tables (host-runnable)
    loopback/                  # x86-32 client <-> x86-64 server (see §9)
    target/                    # run on Tegra: conformance subsets, benchmarks
  packaging/                   # .deb for armhf client libs and arm64 server
  CMakeLists.txt or meson.build
```

Build:
- Language: **C11** for client and `libgb-proto` (minimal deps, easy to compile
  for any ABI); server may be C or C++17. Python 3 for generators.
- Build system: Meson (preferred, matches Mesa/Venus code style) or CMake.
- Cross-toolchains: `arm-linux-gnueabihf-gcc` (client libs),
  `aarch64-linux-gnu-gcc` (server), or build natively on the target using
  multiarch (`dpkg --add-architecture armhf`).
- Client libs must link only against: `libc`, `libpthread`, `libdl`, and
  (optionally) `libX11`/`libxcb`/`libwayland-client`. **No libstdc++ requirement**
  on the client side, to avoid ABI conflicts inside arbitrary apps.
- Flags: `-fvisibility=hidden`, export only the required vendor entry points.

---

## 9. Development and test strategy

### 9.1 Host loopback dev loop (no Tegra required)  — recommended first

Dev machine (confirmed): x86_64, KDE neon / Ubuntu 24.04, AMD Radeon RX 9060 XT
(Mesa 26.2 radeonsi, GL 4.6), gcc 13, clang, cmake, ninja, Xvfb. i386 runtime
libs (libc6, libX11, libxcb, libEGL, libGL, libvulkan, libGLdispatch…) are
installed; `gcc-multilib` + `libc6-dev-i386` are required for `-m32` links.
Build system for this project: **CMake + Ninja** (no meson).
Because the dev GPU is not the Tegra, treat the backend as a "reference"
driver: correctness of marshalling, not NVIDIA behavior.

The bridge logic is **word-size dependent, not ISA dependent**. Therefore:

- Build the client for **i386 (x86-32)**: `-m32`, requires `gcc-multilib`
  and `libglvnd`/`libx11`/`libwayland` `:i386` dev packages.
- Build the server for **x86-64**.
- Backend: any native GL/Vulkan driver on the dev host (Mesa llvmpipe, Intel,
  AMD, or NVIDIA desktop).
- This exercises the exact 32↔64 marshalling, handle mapping, ring buffer,
  `pNext` serialization, and memory-mapping logic.

What it does **not** validate: Tegra-specific driver behavior, dma-buf
caching semantics on Tegra, extension availability, performance numbers.
Those need the target (`tests/target/`).

### 9.2 Test layers

1. **Unit:** ring buffer (multi-thread stress, wrap-around, full/empty),
   codec round-trips (random-valued generated fuzz per opcode), handle-table
   lifetimes, sizeof/offsetof asserts under `-m32` and `-m64`.
2. **Loopback functional:** `es2gears`, `glxgears`, `eglinfo`, `glxinfo`,
   `vkcube`, `vulkaninfo`, `glmark2-es2`, Khronos `deqp-gles2/gles3` subsets,
   piglit subset. Use `xvfb`/Xwayland/headless where needed.
3. **Golden-image:** render deterministic scenes natively (64-bit direct) and
   through the bridge (32-bit client); compare pixel-exact (or within a
   tolerance for float-precision variance).
4. **Target conformance:** same suites on Tegra; compare against native arm64
   runs on the same device (bug in bridge vs bug in driver).
5. **Soak/robustness:** kill `gbd` mid-frame, kill client mid-frame, 1000
   context create/destroy cycles, resize storms, thread migration of contexts,
   fork after init.
6. **Fuzz:** libFuzzer/AFL on the server decoder with ASan/UBSan (Phase 6).

### 9.3 Observability
- `GB_TRACE=<file>`: binary call trace; `gb-replay` replays it against the
  server with no client (for server bug repro).
- `GB_STATS=1`: periodic per-opcode histogram, stall time, bytes shipped.
- `GB_SYNC_ALL=1`: force every call SYNC with error checking.

---

## 10. Phased roadmap with exit criteria

### Phase 0 — Recon (target access required)
Answer Q1–Q10 in `docs/target-report.md`. Install armhf multiarch; run a hello-world
armhf binary; build a trivial arm64 EGL program that creates a pbuffer context
and renders; confirm dma-buf export/import behavior with a small test.
**Exit:** report complete; Strategy X/W/R choice documented in an ADR.

### Phase 1 — Skeleton + software-visible rendering (T0, Strategy R)
- `libgb-proto` ring + framing + handshake; `gb-gen` for EGL 1.4 + GLES2.
- `libEGL_gb32` + GLES2 stubs, `gbd` with real Tegra EGL via `dlopen`.
- Present via readback into MIT-SHM / `wl_shm`.
**Exit:** `es2gears` (armhf) renders on screen from the Tegra GPU; loopback
test suite green on x86; unit tests in CI.

### Phase 2 — GLES3, performance, zero-copy
- T1 coverage; client array uploads; shadow state; async shader compile;
  dma-buf mapped buffers; Strategy X (X11 direct) **or** W (dma-buf present).
- Profiling pass against Q10 baseline.
**Exit:** `glmark2-es2` ≥ 70 % of native arm64; no per-frame copies on the
primary presentation path (verify with `GB_STATS`).

### Phase 3 — Desktop GL / GLX
- GLX vendor library, `GLX_ARB_create_context_profile`, GL 3.3/4.x core + needed
  compat entry points, `glXSwapIntervalEXT`, `GLX_EXT_texture_from_pixmap` if apps need it.
**Exit:** `glxgears`, `glmark2` (GL), and a representative real 32-bit GL app work.

### Phase 4 — Vulkan
- 4a: decide **adopt Venus protocol vs own generator** (ADR). Prototype one
  triangle with whichever is picked.
- 4b: ICD, instance/device/queue, memory (dma-buf export), WSI (xcb first, then Wayland),
  semaphores/fences with fd export.
**Exit:** `vkcube`, `vkcube --present_mode …`, `vulkaninfo`, a deqp-vk smoke subset.

### Phase 5 — (DEFERRED) Wayland + explicit sync
Target is X11-only; do not start unless the requirement changes. If it does:
Strategy W (dma-buf + modifiers + explicit sync).
**Replacement Phase 5 for X11:** zero-copy-ish presentation polish — vsync /
swap interval correctness, `GLX_EXT_swap_control`, `EGL_EXT_buffer_age`,
multi-window/multi-context apps, window resize and reparenting edge cases, and
DRI3/Present fallback (Strategy W-X11) only if Strategy X proves unusable.

### Phase 6 — Hardening and packaging
- Fuzzing, leak checks (fd, shm, GPU memory), fork handling, crash recovery.
- `.deb` packages (armhf client libs; arm64 server) and install/uninstall scripts
  that never touch NVIDIA's own files.
- Docs: user guide, env-var reference, troubleshooting.
**Exit:** soak tests pass 24 h; clean uninstall.

---

## 11. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| Tegra EGL cannot export render targets as dma-buf (Q4) | No zero-copy Wayland present | Strategy X on X11; Strategy R readback; investigate `NvBufSurface`/GBM allocator path |
| Wayland on Tegra requires EGLStream semantics | Standard dma-buf `wl_buffer` path rejected | Implement EGLStream producer/consumer glue in server+client, or run Xwayland and use Strategy X |
| Second X connection rendering to client's window misbehaves (Q8) | Strategy X invalid | Strategy W/R |
| Non-coherent dma-buf mappings from AArch32 (Q6) | Corruption on mapped buffers | Mandatory `DMA_BUF_IOCTL_SYNC`, or shadow-buffer fallback |
| Vulkan on Tegra lacks `VK_EXT_external_memory_dma_buf` (blocks Venus-style host mapping and WSI zero-copy) | Slower Vulkan path | Custom shadow-copy memory path; opaque-fd memory sharing if `VK_KHR_external_memory_fd` exists |
| Application depends on driver-specific extensions (`EGL_NV_*`, `GL_NV_*`) | App fails or misrenders | Gate by frequency from `GB_LOG_MISSING`; support commonly used ones individually |
| arm32 VA exhaustion | Crashes in large apps | Mapping budgets (§5.6), lazy unmap |
| Latency/regressions in tight loops (many tiny SYNC calls) | Poor perf | Client caching, shadow state, per-app tuning knobs |
| Closed driver updates change library layout | Server fails to find libs | Discovery via ldconfig + env override `GB_NATIVE_LIB_DIR` |

---

## 12. Alternatives considered and rejected

| Alternative | Why not (for this project) |
|---|---|
| Build a 32-bit NVIDIA userspace driver | Closed source; impossible |
| Translate `ioctl`s in a compat kernel shim | Would require reimplementing the closed userspace driver's logic and ABI; undocumented |
| User-mode CPU translation of the arm32 app inside a 64-bit process (qemu-user / FEX-style) and thunk natively | Viable and potentially faster for GPU calls (direct pointers), but costs CPU translation overhead and a substantial binary-translator investment. Revisit only if remoting overhead proves unfixable (**Approach C**) |
| `virgl` + `vtest` as the production solution | Good for a prototype/benchmark baseline, not a production path: VM-oriented, extra copies, GL-feature-limited, weak presentation. Use for comparison only |
| Share the app's Wayland connection fd with the server | Object IDs are per-connection state in libwayland; two parties would corrupt ID allocation |
| One shared `gbd` for all clients | More complex isolation; no benefit for v1 |

---

## 13. Rules for the implementing agent

1. **Start with Phase 0 and the host loopback dev loop (§9.1).** Do not write
   Tegra-specific code before the generic bridge works on x86-32→x86-64.
2. **Never guess sizes or lengths** when generating marshalling code. Fail the
   generator and add an explicit rule/override.
3. **No `long`/`size_t`/`void*` in wire types.** Enforce with static asserts and a
   CI check that compiles `libgb-proto` for `-m32` and `-m64` and compares
   generated layout dumps.
4. **Do not touch or redistribute NVIDIA files.** Reference them at runtime only.
5. **Keep each phase's exit criteria measurable**; record numbers
   (frame time, calls/s, bytes/frame, SYNC calls/frame) in `docs/perf/`.
6. **Write an ADR for each [VERIFY] decision** once answered.
7. **Prefer reusing existing generated protocol/decoder code** (Venus) if it is
   license-compatible and demonstrably saves effort, but only after the Phase 4a
   evaluation.
8. **Ask the user** (don't assume) about: which apps matter most (drives
   extension priority), which display server is in use, acceptable perf
   target, and whether a target device is reachable for Phase 0.

---

## Appendix A: pointer-width type map

| API type | arm32 client | arm64 server | Wire |
|---|---|---|---|
| `GLsizeiptr`, `GLintptr` | 32-bit | 64-bit | `uint64_t` |
| `size_t`, `ssize_t`, `long` | 32-bit | 64-bit | `uint64_t` / `int64_t` |
| `EGLAttrib` (`intptr_t`) | 32-bit | 64-bit | `int64_t` |
| `EGLint`, `GLint`, `GLuint`, `GLenum` | 32-bit | 32-bit | `uint32_t` / `int32_t` |
| `EGLDisplay`/`EGLContext`/`EGLSurface`/`EGLConfig`/`EGLImage`/`EGLSync`, `GLsync`, `GLXContext` | `void*` (32-bit) | `void*` (64-bit) | `uint32_t` opaque ID |
| `Display*`, `wl_display*`, `wl_surface*`, `xcb_connection_t*` | pointer | — | **never sent**; replaced by connection string / XID / client-side only |
| `Window`, `Pixmap`, `XID` | `unsigned long` (32-bit) | `unsigned long` (64-bit) | `uint32_t` (XIDs are 29-bit) |
| Vulkan dispatchable handles | pointer to client struct | native pointer | `uint64_t` server-minted ID |
| Vulkan non-dispatchable handles | `uint64_t` | `uint64_t` | `uint64_t` server-minted ID |
| `VkDeviceSize`, `VkDeviceAddress` | `uint64_t` | `uint64_t` | `uint64_t` |
| `PFN_*` / callback pointers (`vkAllocationCallbacks`, debug callbacks) | pointer | — | **never sent**; implemented client-side (debug messages relayed as events) |

## Appendix B: reference projects (read before designing)

| Project | Relevance |
|---|---|
| Mesa **Venus** (`src/virtio/vulkan`) + virglrenderer `vkr` | Complete Vulkan serialization + host decoder; protocol XML generator (`venus-protocol`) |
| Mesa **virgl** / `virgl_test_server` (vtest) | GL remoting prototype; baseline for comparison |
| **gfxstream** (Android emulator) | Production GL/Vulkan API remoting, ring/transport design |
| Mesa `platform_wayland.c`, `platform_x11_dri3.c` | Reference for EGL window-system integration (Strategy W) |
| **egl-wayland** (NVIDIA, open) | How NVIDIA's EGL integrates with Wayland on their stack |
| **FEX-Emu thunks**, **Box64 wrapped libs**, Wine WoW64 | Pointer-width translation techniques, `GLX`/`EGL` thunk generation |
| **Khronos registries** (`gl.xml`, `egl.xml`, `glx.xml`, `vk.xml`) | Source of truth for generators |
| Khronos **Vulkan Loader — ICD interface** docs | Required for `libvulkan_gb32` |
| **libglvnd** docs (EGL/GLX vendor interface) | Required for the GLVND vendor libraries |

---

## 14. Deviations from this design (as built)

Decisions made while implementing the first slice. Each one is intentional;
the "target" column is where the design is meant to end up.

| Topic | Design said | As built | Why / target |
|---|---|---|---|
| GL object names | client-allocated, server-validated (5.5) | server-allocated; every `glGen*` is a sync round trip | simpler first; client-allocated names are a Phase 2 optimisation |
| Ring publish | batch, one doorbell per flush (5.2/7) | publish after every record (wake only if the consumer sleeps) | avoids "trailing async calls never start" bugs; batching needs an idle-flush timer |
| Wait strategy | spin then futex (7.6) | 64 pauses, 64 `sched_yield`s, then futex | same-core sync call was 145 us with pure spinning; now ~9 us. See STATUS.md |
| Bulk data | shared staging arena (5.6) | chunked BIG messages through the rings (one extra copy each way) | correct for any size; arena/dma-buf mapping is Phase 2 |
| Display selection | `eglGetDisplay(Display*)` | server calls `eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, ...)` | the legacy call guesses the platform from the pointer *and the inherited `EGL_PLATFORM`* env var and crashed under a Wayland session |
| Window-system libs | client uses distro libs | same; client resolves `XDisplayString` via `dlsym` so libgb has no libX11 link dependency | keeps the client lib free of ABI-sensitive deps |
| Server discovery | `gb-launch` socket activation | client `posix_spawn`s `gbd` with a socketpair end; lifetime tied to the client | no filesystem socket, nothing to clean up, no cross-user surface |
| Extension strings | advertise only what is implemented (6) | generator emits extension -> commands table; client filters `EGL_EXTENSIONS` / `GL_EXTENSIONS` to extensions whose commands are all implemented; unknown extensions dropped | mechanical, no hand-maintained allow-list |
| GLVND vendor split | EGL/GLX vendor libs (5.1) | single `libgb32_gl.so` exporting EGL + GLES directly | first slice; vendor split is the next packaging step |
| GLX/GL library form | GLVND vendor library (5.1) | `libGL.so.1` drop-in replacement exporting GLX + GL directly | the target has no armhf GL at all, so there is nothing to coexist with; a GLVND vendor split is only needed if one is installed |
| Window-system ordering | not considered | client `XSync`s before calls that name X resources | the server's separate X connection is not ordered with the app's (found by running the real glxgears) |
| Pixel transfers with PBOs | guard + error | encoder sends a buffer offset when the matching binding is non-zero | works for ES3 and desktop alike |
| Core-profile client arrays | n/a | rejected at the pointer call, like the driver | fidelity: do not be more permissive than the real driver |

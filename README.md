# gpu-bridge

Hardware-accelerated OpenGL, OpenGL ES, EGL and GLX for **32-bit (armhf) programs on a 64-bit-only GPU driver**.

## The problem

NVIDIA's Tegra GPU drivers have been **arm64-only** since L4T R24 on the Tegra X1; on newer SoCs they
have been arm64-only from the start. Earlier Tegra releases shipped 32-bit libraries, so the large
body of 32-bit ARM software (games, emulators, legacy and closed-source binaries) used the GPU
directly. On current L4T it can't: the kernel will still run 32-bit ARM programs, but a 32-bit
process can't load a 64-bit library or call into 64-bit code, and NVIDIA provides no 32-bit
replacement. Those programs fall back to software rendering, or don't run at all.

The same gap applies to anything that ends up as a 32-bit process, such as x86 programs run under
`box86` on an arm64 Tegra.

This project was developed and tested on a Tegra X1 (L4T R32.3.1). The approach isn't tied to that
chip, but nothing else has been tried.

## The solution

Split the graphics library in two and pass the calls between the halves:

```
 32-bit app (armhf)                                 64-bit side (arm64)
┌──────────────────────┐   shared-memory rings   ┌─────────────────────────┐
│ libGL / libEGL /     │ ──────────────────────▶ │ gbd (server)            │
│ libGLESv2 / libGLX   │   GL calls, as messages │   decodes the calls and │
│ (one client library) │ ◀────────────────────── │   runs them on the real │
│                      │   return values, errors │   NVIDIA driver         │
└──────────────────────┘                         └─────────────────────────┘
```

- **Client library (32-bit):** one `libGL.so.1.7.0`, also installed under the names `libEGL.so.1`,
  `libGLESv2.so.2`, `libGLESv1_CM.so.1`, `libGLX.so.0` and `libOpenGL.so.0`. The app links against it
  as it would against a normal GL stack. Each GL call is encoded into a compact message.
- **Server (`gbd`, 64-bit):** starts on demand the first time a 32-bit program uses the bridge. It
  decodes the messages and makes the same calls on the real driver, which draws into the app's own X11
  window. The 32-bit app never opens a GPU device or loads an NVIDIA library.
- **Transport:** a pair of lock-free ring buffers in shared memory (a `memfd`, mapped by both sides),
  with futex wake-ups when a side goes idle. Calls that return nothing are queued and sent in
  batches, so the app only waits for the server when it needs an answer (for example `glGetError` or
  a query).
- **Generated code:** most of the encoders and decoders are generated from the Khronos API registry
  (`tools/gb-gen`), so the GL surface is wide without being written by hand. The trickier calls
  (client-side vertex arrays, buffer mapping, swaps) are hand-written.

Scope: X11 only (no Wayland); OpenGL up to 4.6, GLES 1/2/3.x, EGL and GLX. Vulkan is not implemented.
Applications running under `box86` (x86 games on armhf) go through the bridge too.

## Status

Checked against the Khronos conformance test suites by comparing each result with the same test run natively
on the 64-bit driver, and runs a range of real games and applications. Results, measurements and known gaps are in
[docs/STATUS.md](docs/STATUS.md) and [docs/target-report.md](docs/target-report.md).

## Install

A single arm64 Debian package installs both halves and replaces the system's armhf libglvnd
libraries (kept as dpkg diversions and restored on removal). See [packaging/README.md](packaging/README.md).

## More

- [ARCHITECTURE.md](ARCHITECTURE.md): the full design and the decisions behind it
- [docs/](docs/): status, test and performance reports
- `src/client`, `src/server`, `src/proto`: client library, server, and the ring/message layer
- `tests/`: unit, loopback (runs on any x86-64 Linux box) and benchmark programs

## License

MIT, see [LICENSE](LICENSE). The Khronos registry files under `registry/` keep their own
(Apache-2.0) license; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

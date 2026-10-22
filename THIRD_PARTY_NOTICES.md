# Third-party notices

This project is licensed under the MIT License (see `LICENSE`). The following
third-party material is included or used.

## Khronos API registry files (`registry/`)

`gl.xml`, `egl.xml`, `glx.xml` and `vk.xml` are the Khronos Group's machine-readable
API registry files, included unmodified. They are the input of the code generator
(`tools/gb-gen`), which produces the client and server stubs at build time.

| File | Copyright | License |
|---|---|---|
| `registry/gl.xml` | The Khronos Group Inc. | Apache-2.0 |
| `registry/egl.xml` | The Khronos Group Inc. | Apache-2.0 |
| `registry/glx.xml` | The Khronos Group Inc. | Apache-2.0 |
| `registry/vk.xml` | The Khronos Group Inc. | Apache-2.0 OR MIT |

Each file carries its own copyright and SPDX notice at the top, which must be kept
if the file is redistributed. The full Apache-2.0 text is in
`registry/LICENSE-Apache-2.0`. The files come from
<https://github.com/KhronosGroup/OpenGL-Registry>, <https://github.com/KhronosGroup/EGL-Registry>
and <https://github.com/KhronosGroup/Vulkan-Docs>.

The generated sources (never committed; written to the build directory) contain only
factual interface information derived from these files (command names, parameter
lists, enumerant values) and say so in their header comment.

## System headers and libraries (not included)

The sources include the system's `GL/gl.h`, `EGL/egl.h`, `GLES2/gl2.h`, `X11/*.h` and
link against libX11, libpthread and libc. These are not part of this repository and
are not redistributed by it.

The server (`gbd`) loads the vendor's own GL/EGL/GLX libraries at run time with `dlopen`
(on the Tegra, NVIDIA's driver). No vendor code is included in, or linked into, this
project, and the vendor's license terms apply to the vendor's libraries only.

## Measured data (`tools/audit/data/`)

`mesa-radeonsi.txt` is the output of this project's own audit tool run against a
driver (how many values a GL query returns). It is measurement data, not third-party
code.

## Trademarks

OpenGL is a registered trademark of Hewlett Packard Enterprise, used under license by
Khronos. OpenGL ES, EGL and Vulkan are trademarks of the Khronos Group Inc. This project
implements those APIs; it has not gone through Khronos's conformance process and does not
claim conformance. Its test results are reported in `docs/STATUS.md` as test results only.

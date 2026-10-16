# Audit: hand-written client entry points (2026-10-16)

Audit only; nothing here has been changed. Question asked: how much of the
hand-written client code could the generator produce instead?

Source of the numbers: `gb_gen.py --report` (3,593 commands: 3,181 generated,
122 wrapped, 94 manual, 34 alias, 162 unsupported; `docs/STATUS.md` still says
94 wrapped, which is stale) and the body sizes of the 216 wrapped + manual
entry points in `src/client/*.c`.

## Scope: what this does and does not cover

The 216 public entry points are about 2,400 lines. The bulk of the hand-written
code is not in them: it is the helper subsystems they call
(`gb_draw_client`, `gb_arr_pointer`, restart/pack logic, the persistent-mapping
engine, `array_state_query`, shadow VAO/binding tables, fault-tolerant copies).
Those need the process boundary and cannot be derived from the registry. They
stay. This audit only looks at the thin layer on top.

Roughly 100 of the 216 entry points, about 1,000 of the 2,400 lines, could
become generator output.

## Groups

| Group | Fns | Lines | Verdict |
|---|---|---|---|
| A. vendor-suffix copies of a core wrapper | 47 | ~300 | config only (ALIASES), see below |
| B. observe-then-forward shims (update shadow, call `gbx_*`) | 33 | ~330 | generate from a hook table |
| C. getter intercepts (answer from shadow, else `gbx_*`) | 10 | ~110 | generate: short-circuit hook |
| D. mapped-buffer guards (sync mapping, then forward) | 10 | ~70 | generate: pre-hook |
| E. fixed-slot pointer setters (`glVertexPointer` etc.) | 19 | ~150 | table: slot + argument mapping |
| F. map-API adapters (1-line calls into `gb_map_*`) | 10 | ~45 | table: kind/direct/named |
| G. debug / label / callback entry points | 10 | ~140 | partly (labels, insert); callbacks stay |
| H. string queries (cached/server-owned strings) | 16 | ~100 | partly: GLX/EGL string queries are one pattern |
| Rest (draws, legacy, EGL/X11 plumbing, shader source) | 61 | ~1,150 | stays, with notes below |

### A. Vendor-suffix copies (best candidates, lowest risk)

`ALIASES` in `gen_config.py` already forwards a vendor spelling to the core
wrapper. These were written before it or by habit and are copy-pasted bodies:

- `glBindBufferARB`, `glDeleteBuffersARB`, `glEnable/DisableVertexAttribArrayARB`,
  `glVertexAttribDivisorARB`, `glBindVertexArrayOES`, `glDeleteVertexArraysOES`,
  `glPrimitiveRestartIndexNV`, `glClientActiveTextureARB`
- `glDrawArraysInstancedARB/EXT`, `glDrawElementsInstancedARB/EXT`,
  `glDrawRangeElementsEXT`, the `*IndirectCountARB` pair, `glMultiDrawElementsEXT`
- `glVertexAttribPointerARB`, `glVertexAttribI/LPointerEXT`,
  `glWeightPointerARB/OES`, `glMatrixIndexPointerARB/OES`
- `glMapBufferARB`, `glUnmapBufferARB`, `glGetBufferPointervARB`, the
  `*NamedBuffer*EXT` map/unmap/flush/pointer set,
  `glGetVertexAttribPointervARB`, `glGetVertexArrayPointervEXT`,
  `glGetPointerIndexedvEXT`, `glShaderSourceARB`, `glCreateShaderObjectARB`
- KHR/ARB spellings of the debug calls (callback, insert, label, push group,
  object-ptr label)

Verified duplication: `glBindVertexArray` and `glBindVertexArrayOES` have
identical bodies except for the `gbx_` name; same for
`glDrawArraysInstanced` / `glDrawArraysInstancedEXT` and
`glPrimitiveRestartIndex` / `glPrimitiveRestartIndexNV`. A fix to one has to
be remembered in the other. That is the real cost, not the line count.

Caveats before doing it:

- An alias sends the core opcode, so the server calls the core function. Fine
  on the NVIDIA driver (both spellings exist); check each pair where the core
  function may be absent in a context type (OES names in ES2).
- Not aliasable as-is: `glVertexPointerEXT` family (extra `count` argument),
  `glVertexAttribPointerNV`, `glGetVertexAttribPointervNV` (different
  signatures). `glCreateShaderObjectARB` and `glShaderSourceARB` use
  `GLhandleARB`; same width on Linux, so aliasable, but the shader-name tracking
  in `glCreateShader` must stay shared.
- Alias generation must keep GetProcAddress working (it already does for the
  34 existing aliases).

### B/C/D. Hook table

The pattern in about 53 functions is: run a C function over the call's
arguments, then forward to `gbx_<name>()`. Examples: `glBindBuffer`
(`bind_buffer`), `glDeleteBuffers` (`delete_buffers`), `glEnableVertexAttribArray`
(`gb_arr_enable`), `glBindVertexArray` (`vao_get`), `glBufferSubData` (mapping
guard). Proposed generator input:

    PRE_HOOK = {"glBindBuffer": "gbh_bind_buffer", ...}   # void, runs first
    SHORT_CIRCUIT = {"glGetIntegerv": "gbh_get_state", ...} # nonzero = handled

The hook functions keep the actual logic; the wrapper boilerplate and the ARB
copies disappear (an alias inherits the hook). The generator already has the
same idea for persistent mappings (`PERSIST_UP_REGEX` / `PERSIST_DOWN_REGEX`),
so the mechanism exists.

Needs care:

- `glEnable/glDisable` and `glPixelStorei` are switch statements keyed on
  `cap` / `pname`; the hook holds the switch, which is fine.
- `glGetError`, `eglGetError`, `eglMakeCurrent`, `glXMakeCurrent`,
  `eglReleaseThread` run code after the call (context switch bookkeeping);
  need a `POST_HOOK` too, or stay manual.
- Order-sensitive: `glBegin`/`glEnd`/`glNewList`/`glEndList` decide whether
  later shadow updates are legal.

### E/F. Tables

- Pointer setters differ only by slot and by which arguments exist
  (`glNormalPointer` has no size, `glEdgeFlagPointer` has no size or type,
  `glTexCoordPointer` uses the client-active unit, generic attributes take an
  index and normalised flag). A table of `name -> (slot, size?, type?, index?,
  normalized?, integer?)` could emit all 19.
- Map adapters (`glMapBuffer`, `glMapNamedBuffer`, `...Range`, unmap, flush,
  pointer query) are 1-line calls into the same engine, parameterised by
  target-vs-name and access-vs-range.

### G/H. Partly

- Label/insert/push-group calls are 1-line forwards with an explicit length
  rule; generator-friendly.
- The callback entry points (`glDebugMessageCallback*`,
  `eglDebugMessageControlKHR`, `eglSetBlobCacheFuncsANDROID`) are upcall
  registration; keep.
- `glXQueryExtensionsString`, `glXGetClientString`, `glXQueryServerString`,
  `eglQueryString`, the EGL device/output string queries are one pattern
  ("server-owned string, cached per key"): one table.

### Stays hand-written (and why)

- Draws (27): they decide whether client memory must be uploaded, which needs
  the shadow arrays and index scan. The entry points are 8-20 lines each but
  mostly differ in which of base-vertex / instance / base-instance /
  range-end they carry. A generated front-end calling one `gb_draw_client()`
  with named parameters is possible (the signature already takes all of them),
  but that is a bigger step with real regression risk; do last, if at all.
- Legacy compatibility-only code: `glArrayElement` (146 lines),
  `glInterleavedArrays` (80), `glRenderMode`/`glSelectBuffer`/`glFeedbackBuffer`
  (72), `glPush/PopClientAttrib`, `glListDrawCommandsStatesClientNV`. Rarely
  used, complex, correct; leave.
- Pointer-query family (`glGetPointerv`, `glGetVertexAttribPointerv`,
  `glGetPointeri_vEXT`): answered from the shadow, with special cases.
- `glShaderSource` (52): validates against names the client saw created before
  reading the string array.
- EGL/X11 plumbing (display, platform surfaces, EGLImage/sync attribute
  conversion, dma-buf export, device queries, `eglGetProcAddress`): translate
  X11 handles and file descriptors, which is the real work of the bridge.

## Suggested order, if done

1. Move group A to `ALIASES` (about 45 config lines; remove ~300 lines of C).
   Mechanical, verifiable with `state_smoke` plus the piglit/CTS differences
   lists, since behaviour must not change.
2. Add `PRE_HOOK` / `SHORT_CIRCUIT` / `POST_HOOK` to the generator; convert B,
   C, D one family at a time (buffers, VAOs, enables, getters).
3. Pointer-setter and map-adapter tables (E, F).
4. Reconsider the draw front-ends only after the above, with the full CTS
   comparison as the safety net.

Expected result: about 100 fewer hand-written entry points and roughly 1,000
fewer lines, with the ARB/EXT/OES copies unable to drift from the core code.
The shadow-state engines, the draw upload path and the mapping logic do not
shrink.

Risks: the CTS and piglit comparisons take hours, so each step needs the
loopback `state_smoke` suite plus a targeted Tegra re-run of the affected
groups rather than a full sweep. Generated hooks also move logic one level
further from where a reader looks first; the hook function names should make
the intent obvious in the generated code.

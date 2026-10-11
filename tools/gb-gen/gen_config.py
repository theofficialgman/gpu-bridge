"""
gen_config.py - hand-maintained policy for gb_gen.py.

Everything here is a *decision*, not derived from the registry. Keep each
entry commented with why. A command that is neither generable nor listed in
MANUAL for a library is reported as unsupported (stub that logs + errors).
"""

# --------------------------------------------------------------------------
# Libraries (one client .so each; they share the server and opcode space)
# --------------------------------------------------------------------------
EGL_VERSIONS = {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5"}
GLES_VERSIONS = {"2.0", "3.0", "3.1", "3.2"}
GLES1_VERSIONS = {"1.0"}
GL_VERSIONS = {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "2.0", "2.1", "3.0",
               "3.1", "3.2", "3.3", "4.0", "4.1", "4.2", "4.3", "4.4", "4.5",
               "4.6"}
GLX_VERSIONS = {"1.0", "1.1", "1.2", "1.3", "1.4"}

EGL_EXTENSIONS = {
    "EGL_EXT_platform_base",
    "EGL_KHR_fence_sync",
    "EGL_KHR_reusable_sync",
    "EGL_ANDROID_native_fence_sync",
    "EGL_KHR_image_base",
    "EGL_EXT_device_base", "EGL_EXT_device_enumeration",
    "EGL_EXT_device_query", "EGL_KHR_display_reference",
    "EGL_KHR_wait_sync", "EGL_EXT_sync_reuse",
    "EGL_KHR_swap_buffers_with_damage", "EGL_EXT_swap_buffers_with_damage",
    "EGL_KHR_partial_update", "EGL_NV_post_sub_buffer", "EGL_NV_system_time",
}
GLES_EXTENSIONS = {
    "GL_OES_EGL_image",
    "GL_OES_vertex_array_object",
    "GL_EXT_discard_framebuffer",
}
# NVIDIA's ES builds expose NV-suffixed spellings of core entry points; apps
# written for Tegra use them. All are plain aliases of well-annotated core
# commands (no retained pointers).
GLES_NV_ALIASES = {
    "GL_NV_draw_buffers", "GL_NV_framebuffer_blit",
    "GL_NV_framebuffer_multisample", "GL_NV_instanced_arrays",
    "GL_NV_draw_instanced", "GL_NV_read_buffer", "GL_NV_copy_buffer",
    "GL_NV_non_square_matrices", "GL_NV_viewport_array",
    "GL_NV_polygon_mode", "GL_NV_texture_barrier",
    "GL_NV_conditional_render", "GL_NV_copy_image",
    "GL_NV_blend_equation_advanced", "GL_NV_internalformat_sample_query",
    "GL_NV_timer_query",
}

GLX_EXTENSIONS = {
    "GLX_ARB_create_context", "GLX_ARB_create_context_profile",
    "GLX_ARB_create_context_robustness", "GLX_ARB_create_context_no_error",
    "GLX_ARB_get_proc_address", "GLX_ARB_multisample",
    "GLX_ARB_framebuffer_sRGB", "GLX_ARB_fbconfig_float",
    "GLX_EXT_framebuffer_sRGB", "GLX_EXT_swap_control",
    "GLX_EXT_swap_control_tear", "GLX_MESA_swap_control",
    "GLX_SGI_swap_control", "GLX_EXT_visual_info", "GLX_EXT_visual_rating",
    "GLX_EXT_fbconfig_packed_float", "GLX_EXT_buffer_age",
    "GLX_EXT_get_drawable_type", "GLX_EXT_texture_from_pixmap",
    "GLX_EXT_create_context_es_profile",
    "GLX_EXT_create_context_es2_profile", "GLX_MESA_copy_sub_buffer",
}

# One client library exports every API (EGL, GLES, GLX, desktop GL): apps
# routinely mix them (eglgears = EGL + desktop GL, SDL, Wine, ...), and a
# context made current through one API must be usable through another, which
# needs a single bridge connection per process. It is installed under all of
# the usual sonames (libGL.so.1, libEGL.so.1, libGLESv2.so.2, ...).
# (registry, feature api, versions) and (registry, ext apis, (names, prefixes))
LIBS = {
    "all": {
        "features": [("egl", "egl", EGL_VERSIONS),
                     ("gl", "gles1", GLES1_VERSIONS),
                     ("gl", "gles2", GLES_VERSIONS),
                     ("glx", "glx", GLX_VERSIONS),
                     ("gl", "gl", GL_VERSIONS)],
        # Everything the registries list; what cannot be marshalled is
        # classified unsupported and its extensions are hidden from the
        # advertised strings, so over-selecting is safe.
        "extensions": [("egl", ["egl"], (set(), ("EGL_",))),
                       ("gl", ["gles1", "gles2"], (set(), ("GL_",))),
                       ("glx", ["glx"], (set(), ("GLX_",))),
                       ("gl", ["gl", "glcore"], (set(), ("GL_",)))],
        # extension -> commands table used to filter advertised strings
        "ext_filter": [("egl", ["egl"], None), ("gl", ["gles1", "gles2"], None),
                       ("glx", ["glx"], None),
                       ("gl", ["gl", "glcore"], None)],
    },
}

# --------------------------------------------------------------------------
# Hand-written entry points per library
# --------------------------------------------------------------------------
# Public symbol is written by hand (client) and, where needed, a hand-written
# server op (see gb_opx.h).
_MANUAL = {
    "gles": {
        "eglGetDisplay", "eglGetPlatformDisplay", "eglGetPlatformDisplayEXT",
        "eglCreateWindowSurface", "eglCreatePlatformWindowSurface",
        "eglCreatePlatformWindowSurfaceEXT",
        "eglCreatePlatformPixmapSurface", "eglCreatePlatformPixmapSurfaceEXT",
        # EGL_ANDROID_blob_cache: the driver's callbacks run on the server and
        # reach the application's functions as upcalls
        "eglSetBlobCacheFuncsANDROID",
        # EGL_KHR_debug: callbacks are upcalls; object arguments are handles
        "eglDebugMessageControlKHR", "eglQueryDebugKHR", "eglLabelObjectKHR",
        "glGetObjectPtrLabel", "glGetObjectPtrLabelKHR",
        # EGL_EXT_device_*: device handles are bridge ids; see OPX_EglDevice
        "eglQueryDevicesEXT", "eglQueryDeviceStringEXT",
        "eglQueryDeviceAttribEXT", "eglQueryDisplayAttribEXT",
        "eglQueryString", "eglGetProcAddress",
        "glGetString", "glGetStringi",
        "glShaderSource", "glGetVertexAttribPointerv",
        # mapped buffers: client-side shadow copy (gb_client_map.c)
        "glMapBufferRange", "glUnmapBuffer", "glFlushMappedBufferRange",
        "glGetBufferPointerv",
    },
    "gl": {
        "glXQueryExtensionsString", "glXGetClientString",
        "glXQueryServerString", "glXGetProcAddress", "glXGetProcAddressARB",
        "glXChooseFBConfig", "glXGetFBConfigs", "glXGetCurrentDisplay",
        "glGetString", "glGetStringi",
        "glShaderSource", "glShaderSourceARB",
        # the driver retains the buffer pointer and writes at glRenderMode:
        # the server keeps a persistent buffer and returns its contents then
        "glSelectBuffer", "glFeedbackBuffer", "glRenderMode",
        # IBM_multimode_draw_arrays: a mode per draw (loops over glDraw*)
        "glMultiModeDrawArraysIBM", "glMultiModeDrawElementsIBM",
        # EGL_MESA_query_driver (strings owned by the server)
        "eglGetDisplayDriverName", "eglGetDisplayDriverConfig",
        # KHR_debug / ARB_debug_output: callbacks are upcalls; the string
        # calls take explicit lengths (gb_client_debug.c)
        "glDebugMessageCallback", "glDebugMessageCallbackKHR",
        "glDebugMessageCallbackARB", "glDebugMessageInsert",
        "glDebugMessageInsertKHR", "glDebugMessageInsertARB", "glObjectLabel",
        "glObjectLabelKHR", "glPushDebugGroup", "glPushDebugGroupKHR",
        "glObjectPtrLabel", "glObjectPtrLabelKHR",
        # EXT_vertex_array: the extra `count` argument is not needed here
        "glVertexPointerEXT", "glNormalPointerEXT", "glColorPointerEXT",
        "glIndexPointerEXT", "glTexCoordPointerEXT", "glEdgeFlagPointerEXT",
        "glInterleavedArrays", "glGetPointerv",
        "glGetVertexAttribPointerv", "glGetVertexAttribPointervARB",
        "glMultiDrawElements", "glMultiDrawElementsBaseVertex",
        "glMultiDrawElementsEXT",
        # mapped buffers: client-side shadow copy (gb_client_map.c)
        "glMapBuffer", "glMapBufferARB", "glMapBufferRange", "glUnmapBuffer",
        "glUnmapBufferARB", "glFlushMappedBufferRange",
        "glGetBufferPointerv", "glGetBufferPointervARB",
        "glMapNamedBuffer", "glMapNamedBufferEXT", "glMapNamedBufferRange",
        "glMapNamedBufferRangeEXT", "glUnmapNamedBuffer",
        "glUnmapNamedBufferEXT", "glFlushMappedNamedBufferRange",
        "glFlushMappedNamedBufferRangeEXT", "glGetNamedBufferPointerv",
        "glGetNamedBufferPointervEXT",
    },
}
MANUAL = {"all": set().union(*_MANUAL.values())}
ALL_MANUAL = MANUAL["all"]

# Generated encoders/decoders, but the public symbol is hand-written
# (client-side shadow state); the wrapper calls gbx_<name>().
_ARRAY_STATE = {
    "glBindBuffer", "glBindBufferARB", "glDeleteBuffers", "glDeleteBuffersARB",
    "glEnableVertexAttribArray", "glDisableVertexAttribArray",
    "glEnableVertexAttribArrayARB", "glDisableVertexAttribArrayARB",
    "glBindVertexArray", "glBindVertexArrayOES", "glDeleteVertexArrays",
    "glDeleteVertexArraysOES", "glVertexAttribDivisor",
    "glVertexAttribDivisorARB",
    "glPixelStorei", "glGetError", "glGetIntegerv",
    # client-array state (size/type/stride/binding) lives in the shadow
    "glGetFloatv", "glGetDoublev", "glGetBooleanv", "glGetInteger64v",
    # glEnable(GL_VERTEX_ARRAY) etc. is a legal alias of glEnableClientState
    "glEnable", "glDisable",
    # begin/end and display-list nesting decide whether shadow state may change
    "glBegin", "glEnd", "glNewList", "glEndList",
    # a destroyed context's shadow state must go (the id is reused)
    "eglDestroyContext", "glXDestroyContext",
    # shader names the client has seen created (glShaderSource must not
    # read its string array for a name the driver would reject first)
    "glCreateShader", "glCreateShaderObjectARB",
    # pixmap XIDs belong to the application's X connection (ordered by XSync)
    "eglCreatePixmapSurface",
    # EGLImage sources: GL object names / X pixmaps cross as integers; fds
    # (dma-buf) are per-process and must not
    "eglCreateImage", "eglCreateImageKHR",
    # generic buffer bindings (needed to resolve glMapBuffer(target))
    "glBindBufferBase", "glBindBufferRange",
    # BUFFER_MAPPED / ACCESS / MAP_* describe the client-side mapping
    "glGetBufferParameteriv", "glGetBufferParameteri64v",
}
_POINTER_FUNCS = {
    "glVertexAttribPointer", "glVertexAttribPointerARB",
    "glVertexAttribIPointer", "glVertexAttribLPointer",
    "glVertexPointer", "glNormalPointer", "glColorPointer",
    "glSecondaryColorPointer", "glFogCoordPointer", "glIndexPointer",
    "glEdgeFlagPointer", "glTexCoordPointer",
    "glVertexAttribIPointerEXT", "glVertexAttribLPointerEXT",
}
_DRAW_FUNCS = {
    "glDrawArrays", "glDrawArraysInstanced", "glDrawArraysInstancedARB",
    "glDrawArraysInstancedBaseInstance", "glDrawElements",
    "glDrawRangeElements", "glDrawElementsInstanced",
    "glDrawElementsInstancedARB", "glDrawElementsBaseVertex",
    "glDrawRangeElementsBaseVertex", "glDrawElementsInstancedBaseVertex",
    "glDrawElementsInstancedBaseInstance",
    "glDrawElementsInstancedBaseVertexBaseInstance", "glMultiDrawArrays",
    "glDrawElementsInstancedEXT", "glDrawRangeElementsEXT",
    "glDrawArraysInstancedEXT",
}
_WRAPPED = {
    "gles": _ARRAY_STATE | _POINTER_FUNCS | _DRAW_FUNCS | {
        "eglMakeCurrent", "eglReleaseThread", "eglGetError",
    },
    "gl": _ARRAY_STATE | _POINTER_FUNCS | _DRAW_FUNCS | {
        "glEnableClientState", "glDisableClientState",
        "glClientActiveTexture", "glClientActiveTextureARB", "glArrayElement",
        "glPushClientAttrib", "glPopClientAttrib",
        "glXMakeCurrent", "glXMakeContextCurrent",
    },
}

WRAPPED = {"all": set().union(*_WRAPPED.values())}

# Calls that must block even though they return nothing.
FORCE_SYNC = {"glFinish", "glXSwapBuffers", "glXWaitGL", "glXWaitX"}

# Commands whose registry entry lacks a usable len=.
LEN_OVERRIDES = {
    ("eglChooseConfig", "configs"): "config_size",
    ("eglGetConfigs", "configs"): "config_size",
    # array commands whose count is the 'count' parameter (x components)
    ("eglSwapBuffersWithDamageKHR", "rects"): "n_rects*4",
    ("eglSwapBuffersWithDamageEXT", "rects"): "n_rects*4",
    ("eglSetDamageRegionKHR", "rects"): "n_rects*4",
    ("glGetUniformIndices", "uniformNames"): "uniformCount",
    ("glGetUniformIndices", "uniformIndices"): "uniformCount",
    ("glViewportArrayv", "v"): "count*4",
    ("glScissorArrayv", "v"): "count*4",
    ("glDepthRangeArrayv", "v"): "count*2",
    ("glWindowRectanglesEXT", "box"): "count*4",
    ("glGetActiveUniformsiv", "params"): "uniformCount",
    ("glPolygonStipple", "mask"): "128",
    ("glGetPolygonStipple", "mask"): "128",
    # DSA sub-image uploads: the registry gives no len
    ("glTextureSubImage1D", "pixels"): "COMPSIZE(format,type,width)",
    ("glTextureSubImage2D", "pixels"): "COMPSIZE(format,type,width,height)",
    ("glTextureSubImage3D", "pixels"):
        "COMPSIZE(format,type,width,height,depth)",
    ("glNamedBufferDataEXT", "data"): "size",
    ("glNamedBufferSubDataEXT", "data"): "size",
    ("glGetNamedBufferSubDataEXT", "data"): "size",
}

# Pointer parameters that are, on the wire, just an integer: either a buffer
# offset (when the matching buffer binding is non-zero) or a client-memory
# pointer that the hand-written wrapper handles itself before ever calling the
# generated encoder.
OFFSET_ONLY = set()
for _f in _POINTER_FUNCS:
    OFFSET_ONLY.add((_f, "pointer"))
for _f in ("glDrawElements", "glDrawRangeElements", "glDrawElementsInstanced",
           "glDrawElementsInstancedARB", "glDrawElementsBaseVertex",
           "glDrawRangeElementsBaseVertex",
           "glDrawElementsInstancedBaseVertex",
           "glDrawElementsInstancedBaseInstance",
           "glDrawElementsInstancedBaseVertexBaseInstance",
           "glDrawElementsInstancedEXT", "glDrawRangeElementsEXT"):
    OFFSET_ONLY.add((_f, "indices"))
for _f in ("glDrawArraysIndirect", "glDrawElementsIndirect",
           "glMultiDrawArraysIndirect", "glMultiDrawElementsIndirect",
           "glMultiDrawArraysIndirectCount", "glMultiDrawElementsIndirectCount",
           "glMultiDrawArraysIndirectCountARB",
           "glMultiDrawElementsIndirectCountARB"):
    OFFSET_ONLY.add((_f, "indirect"))

# Pixel transfer commands. Their data pointer is an *offset* when a
# PIXEL_PACK/UNPACK buffer is bound; the encoder checks client-tracked state
# and sends GB_PBO_LEN + offset instead of data.
PBO_UNPACK_REGEX = (
    r"^gl(Compressed)?(Texture|Tex|MultiTex)(Sub)?Image[123]D(ARB|EXT)?$|"
    r"^glDrawPixels$|^glColor(Sub)?Table(EXT)?$|"
    r"^glConvolutionFilter[12]D(EXT)?$|^glSeparableFilter2D(EXT)?$")
PBO_UNPACK_EXTRA = {"glBitmap", "glPolygonStipple", "glPixelMapfv",
                    "glPixelMapuiv", "glPixelMapusv"}
PBO_PACK_REGEX = (
    r"^glReadPixels$|^glReadnPixels(ARB|KHR)?$|^glGetTexImage$|"
    r"^glGetnTexImage(ARB)?$|^glGetTextureImage(EXT)?$|"
    r"^glGetTextureSubImage$|^glGetCompressedTexImage(ARB)?$|"
    r"^glGetnCompressedTexImage(ARB)?$|^glGetCompressedTextureImage$|"
    r"^glGetCompressedTextureSubImage$|^glGetColorTable(EXT)?$|"
    r"^glGetnColorTable(ARB)?$|^glGetConvolutionFilter(EXT)?$|"
    r"^glGetnConvolutionFilter(ARB)?$|^glGetSeparableFilter(EXT)?$|"
    r"^glGetnSeparableFilter(ARB)?$|^glGetHistogram(EXT)?$|"
    r"^glGetnHistogram(ARB)?$|^glGetMinmax(EXT)?$|^glGetnMinmax(ARB)?$")
PBO_PACK_EXTRA = {"glGetPolygonStipple", "glGetnPolygonStipple",
                  "glGetnPolygonStippleARB", "glGetPixelMapfv",
                  "glGetPixelMapuiv", "glGetPixelMapusv",
                  "glGetnPixelMapfv", "glGetnPixelMapfvARB",
                  "glGetnPixelMapuiv", "glGetnPixelMapuivARB",
                  "glGetnPixelMapusv", "glGetnPixelMapusvARB"}
# typed (non-void) pixel pointers
PBO_TYPED_PARAMS = {
    ("glBitmap", "bitmap"), ("glPolygonStipple", "mask"),
    ("glGetPolygonStipple", "mask"), ("glGetnPolygonStipple", "pattern"),
    ("glGetnPolygonStippleARB", "pattern"),
    ("glPixelMapfv", "values"), ("glPixelMapuiv", "values"),
    ("glPixelMapusv", "values"), ("glGetPixelMapfv", "values"),
    ("glGetPixelMapuiv", "values"), ("glGetPixelMapusv", "values"),
}

# COMPSIZE(...) argument tuples -> size-rule key. The key selects a
# GB_BYTES_<key>() macro (pixel rectangles etc.) or the pname table.
COMPSIZE = {
    ("pname",): "pname",
    ("format", "type", "width"): "pixels1",
    ("format", "type", "height"): "pixels1",
    ("format", "type", "width", "height"): "pixels2",
    ("format", "type", "width", "height", "depth"): "pixels3",
    ("target", "level", "format", "type"): "pixels_tex",
    ("target", "level"): "ctex",
    ("n", "type"): "callists",
    ("width", "height"): "bitmap",
    ("target",): "pname",              # glGet*i_v: class geti_v
    ("buffer",): "pname",              # glClearBuffer*v: class clearbuffer
    ("format", "type"): "pixel1",      # one texel (glClearTexImage, ...)
    ("target", "stride", "order"): "map1",
    ("target", "ustride", "uorder", "vstride", "vorder"): "map2",
}

# Arrays whose element count only the *server* can know (it holds the GL
# state): the reply carries the byte length and the client copies exactly
# that. (regex, key, candidate argument-name lists)
SERVER_SIZED = [
    (r"^glGetUniform(f|i|ui|d|i64|ui64)v(ARB|EXT|NV)?$", "uniform",
     [["program", "location"], ["programObj", "location"]]),
    (r"^glGetTexImage$", "pixels_tex", [["target", "level", "format", "type"]]),
    (r"^glGetCompressedTexImage(ARB)?$", "ctex", [["target", "level"]]),
    (r"^glGetPixelMap(f|ui|us)v$", "pixelmap", [["map"]]),
    (r"^glGetActiveUniformBlockiv$", "uniformblock",
     [["program", "uniformBlockIndex", "pname"]]),
    (r"^glGetProgramStringARB$", "progstring", [["target", "pname"]]),
]

# glGet*/gl*v pname -> element count is command specific. Regex -> class
# (see gb_pname_count_class in gb_gl_sizes.h).
PNAME_CLASSES = [
    (r"^glGet(Boolean|Double|Float|Integer|Integer64)v$", "getv"),
    (r"^glGet(Boolean|Double|Float|Integer|Integer64)i_v$", "geti_v"),
    (r"^glGet(Boolean|Double|Float|Integer|Integer64)_?vEXT$", "getv"),
    (r"^gl(Get)?Light[fi]v$", "light"),
    (r"^gl(Get)?Material[fi]v$", "material"),
    (r"^gl(Get)?TexEnv[fi]v$", "texenv"),
    (r"^gl(Get)?TexGen[dfi]v$", "texgen"),
    (r"^gl(Get)?LightModel[fi]v$", "lightmodel"),
    (r"^glFog[fi]v$", "fog"),
    (r"^glPointParameter[fi]v(ARB|EXT)?$", "pointparam"),
    (r"^gl(Get)?(Tex|Sampler)Parameter(f|i|Iiv|Iuiv|fv|iv)?(EXT|ARB)?$",
     "texparam"),
    (r"^gl(Get)?TextureParameter.*$", "texparam"),
    (r"^glGet(Boolean|Double|Float|Integer)IndexedvEXT$", "geti_v"),
    (r"^glClear(Named)?(Framebuffer|Buffer)(f|i|ui)v$", "clearbuffer"),
]

# Handle lifetime: command -> (handle parameter, success condition or None).
HANDLE_FREE = {
    "eglDestroyContext": ("ctx", "ret == EGL_TRUE"),
    "eglDestroySurface": ("surface", "ret == EGL_TRUE"),
    "eglDestroyImage": ("image", "ret == EGL_TRUE"),
    "eglDestroyImageKHR": ("image", "ret == EGL_TRUE"),
    "eglDestroySync": ("sync", "ret == EGL_TRUE"),
    "eglDestroySyncKHR": ("sync", "ret == EGL_TRUE"),
    "glDeleteSync": ("sync", None),
    "glXDestroyContext": ("ctx", None),
}

# Explicitly unsupported (with the reason shown in the report).
UNSUPPORTED = {
    # The driver keeps these client pointers and writes through them later.
    "glFeedbackBufferEXT": "retained client pointer",
    "glSelectBufferEXT": "retained client pointer",
    # needs a font round-trip + display lists built from X font metrics
    "glXUseXFont": "needs server-side X font access",
    "glXCreateGLXPixmapMESA": "obsolete (use glXCreatePixmap)",
    # explicit-length strings (length < 0 means NUL-terminated): needs a
    # dedicated kind
}

# Extensions that add no entry points (behavior / enums only) and that the
# registry does not list for the APIs we serve: safe to pass through.
EXT_NO_COMMANDS = {
    "GL_OES_EGL_sync", "GL_EXT_frag_depth", "GL_NV_bgr", "GL_NV_packed_float",
    "GL_NV_packed_float_linear", "GL_NV_texture_array",
    "GL_NV_texture_compression_latc", "GL_NV_texture_compression_s3tc",
    "GL_OES_depth_texture_cube_map", "GL_EXT_multisample_compatibility",
    "GL_EXT_compressed_ETC1_RGB8_sub_texture",
    "GL_EXT_EGL_image_external_wrap_modes", "GL_NV_occlusion_query_samples",
    # EGL (behavior-only)
    "EGL_MESA_configless_context", "EGL_MESA_x11_native_visual_id",
    # desktop, enum-only (advertised by Mesa; absent from the registry)
    "GL_APPLE_packed_pixels", "GL_ATI_texture_compression_3dc",
    "GL_EXT_demote_to_helper_invocation", "GL_EXT_shader_clock",
    "GL_EXT_texture_rectangle", "GL_MESA_texture_signed_rgba",
    "GL_NV_ES1_1_compatibility",
    # desktop (enum-only)
    "GL_EXT_texture_edge_clamp", "GL_EXT_texture_lod", "GL_EXT_Cg_shader",
}

# Hand-written entry points for extensions the shipped registry does not list
# (they are advertised by real drivers): added to the proc table and to the
# extension table so that the advertised strings keep them.
EXTRA_PROCS = {
    "glBlendEquationSeparateATI", "glMultiDrawArraysSUN",
    "glMultiDrawElementsSUN",
}
EXT_EXTRA = {
    "GL_ATI_blend_equation_separate": ["glBlendEquationSeparateATI"],
    "GL_SUN_multi_draw_arrays": ["glMultiDrawArraysSUN",
                                 "glMultiDrawElementsSUN"],
}

# Vendor-suffixed spellings of core entry points (ES extensions): the public
# symbol forwards to the core function so that the client-side shadow state
# (client arrays, mapped buffers, divisors) sees every call.
ALIASES = {
    "eglQueryDisplayAttribKHR": "eglQueryDisplayAttribEXT",
    # EXT_vertex_array / EXT_fog_coord / EXT_secondary_color spellings
    "glArrayElementEXT": "glArrayElement",
    "glDrawArraysEXT": "glDrawArrays",
    "glGetPointervEXT": "glGetPointerv",
    "glGetPointervKHR": "glGetPointerv",
    # indirect multi-draw spellings
    "glMultiDrawArraysIndirectAMD": "glMultiDrawArraysIndirect",
    "glMultiDrawElementsIndirectAMD": "glMultiDrawElementsIndirect",
    "glMultiDrawArraysIndirectEXT": "glMultiDrawArraysIndirect",
    "glMultiDrawElementsIndirectEXT": "glMultiDrawElementsIndirect",
    "glFogCoordPointerEXT": "glFogCoordPointer",
    "glSecondaryColorPointerEXT": "glSecondaryColorPointer",
    "glMapBufferRangeEXT": "glMapBufferRange",
    "glFlushMappedBufferRangeEXT": "glFlushMappedBufferRange",
    "glMapBufferOES": "glMapBuffer",
    "glUnmapBufferOES": "glUnmapBuffer",
    "glGetBufferPointervOES": "glGetBufferPointerv",
    "glDrawElementsBaseVertexEXT": "glDrawElementsBaseVertex",
    "glDrawElementsBaseVertexOES": "glDrawElementsBaseVertex",
    "glDrawRangeElementsBaseVertexEXT": "glDrawRangeElementsBaseVertex",
    "glDrawRangeElementsBaseVertexOES": "glDrawRangeElementsBaseVertex",
    "glDrawElementsInstancedBaseVertexEXT": "glDrawElementsInstancedBaseVertex",
    "glDrawElementsInstancedBaseVertexOES": "glDrawElementsInstancedBaseVertex",
    "glMultiDrawElementsBaseVertexEXT": "glMultiDrawElementsBaseVertex",
    "glDrawArraysInstancedNV": "glDrawArraysInstanced",
    "glDrawElementsInstancedNV": "glDrawElementsInstanced",
    "glDrawArraysInstancedBaseInstanceEXT": "glDrawArraysInstancedBaseInstance",
    "glDrawElementsInstancedBaseInstanceEXT":
        "glDrawElementsInstancedBaseInstance",
    "glDrawElementsInstancedBaseVertexBaseInstanceEXT":
        "glDrawElementsInstancedBaseVertexBaseInstance",
    "glVertexAttribDivisorNV": "glVertexAttribDivisor",
    "glVertexAttribDivisorEXT": "glVertexAttribDivisor",
    "glViewportArrayvOES": "glViewportArrayv",
    "glViewportArrayvNV": "glViewportArrayv",
    "glScissorArrayvOES": "glScissorArrayv",
    "glScissorArrayvNV": "glScissorArrayv",
}

# Calls after which the server's cached "current native context" is stale.
CTX_CHANGERS = {"eglMakeCurrent", "eglReleaseThread", "glXMakeCurrent",
                "glXMakeContextCurrent", "glXMakeCurrentReadSGI",
                "glXDestroyContext", "eglDestroyContext", "eglTerminate"}

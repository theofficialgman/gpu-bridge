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
GLES_VERSIONS = {"2.0", "3.0"}
GL_VERSIONS = {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "2.0", "2.1", "3.0",
               "3.1", "3.2", "3.3", "4.0", "4.1", "4.2", "4.3", "4.4", "4.5",
               "4.6"}
GLX_VERSIONS = {"1.0", "1.1", "1.2", "1.3", "1.4"}

EGL_EXTENSIONS = {
    "EGL_EXT_platform_base",
    "EGL_KHR_fence_sync",
    "EGL_KHR_reusable_sync",
    "EGL_ANDROID_native_fence_sync",
}
GLES_EXTENSIONS = {
    "GL_OES_EGL_image",
    "GL_OES_vertex_array_object",
    "GL_EXT_discard_framebuffer",
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

# (registry, feature api, versions) and (registry, ext apis, (names, prefixes))
LIBS = {
    "gles": {
        "features": [("egl", "egl", EGL_VERSIONS),
                     ("gl", "gles2", GLES_VERSIONS)],
        "extensions": [("egl", ["egl"], (EGL_EXTENSIONS, ())),
                       ("gl", ["gles2"], (GLES_EXTENSIONS, ()))],
        # extension -> commands table used to filter advertised strings
        "ext_filter": [("egl", ["egl"], None), ("gl", ["gles2"], None)],
    },
    "gl": {
        "features": [("glx", "glx", GLX_VERSIONS),
                     ("gl", "gl", GL_VERSIONS)],
        "extensions": [("glx", ["glx"], (GLX_EXTENSIONS, ())),
                       ("gl", ["gl", "glcore"],
                        (set(), ("GL_ARB_", "GL_KHR_", "GL_EXT_")))],
        "ext_filter": [("glx", ["glx"], None),
                       ("gl", ["gl", "glcore"], None)],
    },
}

# --------------------------------------------------------------------------
# Hand-written entry points per library
# --------------------------------------------------------------------------
# Public symbol is written by hand (client) and, where needed, a hand-written
# server op (see gb_opx.h).
MANUAL = {
    "gles": {
        "eglGetDisplay", "eglGetPlatformDisplay", "eglGetPlatformDisplayEXT",
        "eglCreateWindowSurface", "eglCreatePlatformWindowSurface",
        "eglCreatePlatformWindowSurfaceEXT",
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
ALL_MANUAL = set().union(*MANUAL.values())

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
WRAPPED = {
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

# Calls that must block even though they return nothing.
FORCE_SYNC = {"glFinish", "glXSwapBuffers", "glXWaitGL", "glXWaitX"}

# Commands whose registry entry lacks a usable len=.
LEN_OVERRIDES = {
    ("eglChooseConfig", "configs"): "config_size",
    ("eglGetConfigs", "configs"): "config_size",
    # array commands whose count is the 'count' parameter (x components)
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
    (r"^glGetUniform(f|i|ui|d|i64|ui64)v(ARB|EXT)?$", "uniform",
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
    "glFeedbackBuffer": "retained client pointer (written at glRenderMode)",
    "glSelectBuffer": "retained client pointer (written at glRenderMode)",
    "glFeedbackBufferEXT": "retained client pointer",
    "glSelectBufferEXT": "retained client pointer",
    # needs a font round-trip + display lists built from X font metrics
    "glXUseXFont": "needs server-side X font access",
    "glXCreateGLXPixmapMESA": "obsolete (use glXCreatePixmap)",
    # explicit-length strings (length < 0 means NUL-terminated): needs a
    # dedicated kind
    "glDebugMessageInsert": "string with explicit length parameter",
    "glObjectLabel": "string with explicit length parameter",
    "glPushDebugGroup": "string with explicit length parameter",
}

# SPDX-License-Identifier: MIT
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
# Number of translation units each generated source is split into (the
# compiler's cost for a unit grows faster than its size).
GEN_CHUNKS = 16

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
        # fds the server returns (OPX_FdOp)
        "eglGetStreamFileDescriptorKHR", "eglQueryOutputLayerStringEXT",
        "eglQueryOutputPortStringEXT", "glListDrawCommandsStatesClientNV",
        "eglDupNativeFenceFDANDROID", "eglExportDMABUFImageQueryMESA",
        "eglExportDMABUFImageMESA",
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
        # EXT_direct_state_access: pointer getters answered from the shadow
        "glGetPointeri_vEXT", "glGetPointerIndexedvEXT",
        "glGetVertexArrayPointervEXT", "glGetVertexArrayPointeri_vEXT",
        "glGetVertexAttribPointervNV",
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
    # BUFFER_MAPPED & co describe the client-side mapping
    "glGetNamedBufferParameteriv", "glGetNamedBufferParameterivEXT",
    "glGetNamedBufferParameteri64v",
    # explicit-length strings where a negative length means NUL-terminated
    "glNamedStringARB", "glDeleteNamedStringARB", "glIsNamedStringARB",
    "glGetNamedStringARB", "glGetNamedStringivARB",
    # errors while the buffer is mapped (the client holds the mappings)
    "glBufferSubData", "glGetBufferSubData", "glCopyBufferSubData",
    "glClearBufferData", "glClearBufferSubData", "glNamedBufferSubData",
    "glGetNamedBufferSubData", "glCopyNamedBufferSubData",
    "glClearNamedBufferData", "glClearNamedBufferSubData",
    "glBindBuffer", "glBindBufferARB", "glDeleteBuffers", "glDeleteBuffersARB",
    "glEnableVertexAttribArray", "glDisableVertexAttribArray",
    "glEnableVertexAttribArrayARB", "glDisableVertexAttribArrayARB",
    "glBindVertexArray", "glBindVertexArrayOES", "glDeleteVertexArrays",
    # DSA: the element buffer lives in the shadow VAO too
    "glVertexArrayElementBuffer",
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
    # answered from a cache once the driver has said where a name is
    "glGetUniformLocation", "glGetUniformLocationARB",
    "glGetAttribLocation", "glGetAttribLocationARB",
    # shader names the client has seen created (glShaderSource must not
    # read its string array for a name the driver would reject first)
    "glCreateShader", "glCreateShaderObjectARB",
    # pixmap XIDs belong to the application's X connection (ordered by XSync)
    "eglCreatePixmapSurface",
    # EGLImage sources: GL object names / X pixmaps cross as integers; fds
    # (dma-buf) are per-process and must not
    "eglCreateImage", "eglCreateImageKHR", "eglCreateSync",
    "eglCreateSyncKHR", "eglCreateSync64KHR",
    "glImportMemoryFdEXT", "glImportSemaphoreFdEXT",
    "eglCreateStreamFromFileDescriptorKHR",
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
    "glMultiTexCoordPointerEXT",
    "glPointSizePointerOES", "glMatrixIndexPointerOES",
    "glMatrixIndexPointerARB", "glWeightPointerOES", "glWeightPointerARB",
    "glVertexAttribPointerNV",
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
    # the restart index is client state (it limits the vertex-range scan)
    "glPrimitiveRestartIndex", "glPrimitiveRestartIndexNV",
    # client-memory command structures (compatibility profile) are expanded
    # into ordinary draws by hand wrappers
    "glDrawArraysIndirect", "glDrawElementsIndirect",
    "glMultiDrawArraysIndirect", "glMultiDrawElementsIndirect",
    "glMultiDrawArraysIndirectCount", "glMultiDrawElementsIndirectCount",
    "glMultiDrawArraysIndirectCountARB",
    "glMultiDrawElementsIndirectCountARB",
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

# Synchronous requests whose reply is collected one call later: the caller
# waits for the previous swap, not the one it just queued, so the application
# builds the next frame while the server presents this one (the same one
# frame of queueing a driver does). GLX swaps return nothing.
LAG_SYNC = {"glXSwapBuffers"}

# Statements run in the client stub before the request is built (names are the
# command's own parameters): what makes cached answers stale.
CLIENT_PRE = {
    "glLinkProgram": "gb_cl_loc_invalidate((uint32_t)program);",
    "glLinkProgramARB": "gb_cl_loc_invalidate((uint32_t)(uintptr_t)programObj);",
    "glDeleteProgram": "gb_cl_loc_invalidate((uint32_t)program);",
    "glDeleteObjectARB": "gb_cl_loc_invalidate((uint32_t)(uintptr_t)obj);",
    "glProgramBinary": "gb_cl_loc_invalidate((uint32_t)program);",
    "glProgramBinaryOES": "gb_cl_loc_invalidate((uint32_t)program);",
}

# Commands whose registry entry lacks a usable len=.
LEN_OVERRIDES = {
    # evaluator / pixel-map getters: the element count depends on map state;
    # the server computes it (SERVER_SIZED "map" / "pixelmap")
    **{(f"glGet{n}Map{t}v{a}", "v"): "1" for n in ("", "n") for t in "dfi"
       for a in ("", "ARB") if not (n == "" and a)},
    **{(f"glGetnPixelMap{t}v{a}", "values"): "1" for t in ("f", "ui", "us")
       for a in ("", "ARB")},
    # arrays the registry sizes with nothing (default 1) although a count
    # parameter says how many there are
    ("glGetProgramivNV", "params"): "1",
    ("glDepthRangeArrayfvOES", "v"): "count*2",
    ("glDepthRangeArrayfvNV", "v"): "count*2",
    ("glDepthRangeArraydvNV", "v"): "count*2",
    ("glFramebufferSampleLocationsfvARB", "v"): "count*2",
    ("glFramebufferSampleLocationsfvNV", "v"): "count*2",
    ("glNamedFramebufferSampleLocationsfvARB", "v"): "count*2",
    ("glNamedFramebufferSampleLocationsfvNV", "v"): "count*2",
    ("glMulticastFramebufferSampleLocationsfvNV", "v"): "count*2",
    ("glFramebufferSamplePositionsfvAMD", "values"): "numsamples*2",
    ("glNamedFramebufferSamplePositionsfvAMD", "values"): "numsamples*2",
    ("glDebugMessageControlKHR", "ids"): "count",
    ("glGetProgramResourcefvNV", "props"): "propCount",
    ("glDrawCommandsNV", "indirects"): "count",
    ("glDrawCommandsNV", "sizes"): "count",
    ("glDrawCommandsAddressNV", "indirects"): "count",
    ("glDrawCommandsAddressNV", "sizes"): "count",
    ("glDrawCommandsStatesNV", "indirects"): "count",
    ("glDrawCommandsStatesNV", "sizes"): "count",
    ("glDrawCommandsStatesNV", "states"): "count",
    ("glDrawCommandsStatesNV", "fbos"): "count",
    ("glDrawCommandsStatesAddressNV", "indirects"): "count",
    ("glDrawCommandsStatesAddressNV", "sizes"): "count",
    ("glDrawCommandsStatesAddressNV", "states"): "count",
    ("glDrawCommandsStatesAddressNV", "fbos"): "count",
    ("glMatrixLoad3x2fNV", "m"): "6",
    ("glMatrixMult3x2fNV", "m"): "6",
    ("glMatrixLoad3x3fNV", "m"): "9",
    ("glMatrixMult3x3fNV", "m"): "9",
    ("glMatrixLoadTranspose3x3fNV", "m"): "9",
    ("glMatrixMultTranspose3x3fNV", "m"): "9",
    ("glSpecializeShaderARB", "pConstantIndex"): "numSpecializationConstants",
    ("glSpecializeShaderARB", "pConstantValue"): "numSpecializationConstants",
    ("eglCompositorSetContextListEXT", "external_ref_ids"): "num_entries",
    ("eglCompositorSetContextAttributesEXT", "context_attributes"):
        "num_entries",
    ("eglCompositorSetWindowListEXT", "external_win_ids"): "num_entries",
    ("eglCompositorSetWindowAttributesEXT", "window_attributes"):
        "num_entries",
    ("eglSwapBuffersRegionNOK", "rects"): "numRects*4",
    ("eglSwapBuffersRegion2NOK", "rects"): "numRects*4",
    ("eglChooseConfig", "configs"): "config_size",
    ("eglGetConfigs", "configs"): "config_size",
    # array commands whose count is the 'count' parameter (x components)
    ("glGetSeparableFilter", "span"): "0",     # reserved, must be NULL
    ("glGetSeparableFilterEXT", "span"): "0",
    ("glTransformFeedbackAttribsNV", "attribs"): "count*3",
    ("eglSetStreamMetadataNV", "data"): "size",
    ("eglQueryStreamMetadataNV", "data"): "size",
    ("eglGetOutputLayersEXT", "layers"): "max_layers",
    ("eglGetOutputPortsEXT", "ports"): "max_ports",
    ("eglQueryDmaBufFormatsEXT", "formats"): "max_formats",
    ("eglQueryDmaBufModifiersEXT", "modifiers"): "max_modifiers",
    ("eglQueryDmaBufModifiersEXT", "external_only"): "max_modifiers",
    ("glWaitSemaphoreEXT", "buffers"): "numBufferBarriers",
    ("glWaitSemaphoreEXT", "textures"): "numTextureBarriers",
    ("glWaitSemaphoreEXT", "srcLayouts"): "numTextureBarriers",
    ("glSignalSemaphoreEXT", "buffers"): "numBufferBarriers",
    ("glSignalSemaphoreEXT", "textures"): "numTextureBarriers",
    ("glSignalSemaphoreEXT", "dstLayouts"): "numTextureBarriers",
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
for _f in ("glMultiDrawArraysIndirectBindlessNV",
           "glMultiDrawElementsIndirectBindlessNV",
           "glMultiDrawArraysIndirectBindlessCountNV",
           "glMultiDrawElementsIndirectBindlessCountNV"):
    OFFSET_ONLY.add((_f, "indirect"))
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
    ("format", "type", "count"): "pixels1",       # glColorSubTable
    ("target", "format", "type", "width"): "pixels1t",   # separable filter row
    ("target", "format", "type", "height"): "pixels1t",  # ... column
    ("value",): "pname",                # glGetIntegerui64vNV: class getv
    ("target", "stride", "order"): "map1",
    ("target", "ustride", "uorder", "vstride", "vorder"): "map2",
}

# Compressed uploads read their data out of a larger cuboid when the unpack
# compressed-block state is set: the length is the span, not imageSize.
for _pre in ("Tex", "Texture", "MultiTex"):
    for _sub in ("", "Sub"):
        for _n in (1, 2, 3):
            _dims = ("width", "height", "depth")[:_n]
            COMPSIZE[("imageSize",) + _dims] = "cbytes%d" % _n
            for _suf in ("", "ARB", "EXT"):
                LEN_OVERRIDES[("glCompressed%s%sImage%dD%s" %
                               (_pre, _sub, _n, _suf), "data")] = \
                    "COMPSIZE(imageSize,%s)" % ",".join(_dims)

# Arrays whose element count only the *server* can know (it holds the GL
# state): the reply carries the byte length and the client copies exactly
# that. (regex, key, candidate argument-name lists)
SERVER_SIZED = [
    (r"^glGetUniform(f|i|ui|d|i64|ui64)v(ARB|EXT|NV)?$", "uniform",
     [["program", "location"], ["programObj", "location"]]),
    (r"^glGetTextureImageEXT$", "pixels_dsa",
     [["texture", "target", "level", "format", "type"]]),
    (r"^glGetMultiTexImageEXT$", "pixels_multi",
     [["texunit", "target", "level", "format", "type"]]),
    (r"^glGetCompressedTextureImageEXT$", "ctex_dsa",
     [["texture", "target", "lod"]]),
    (r"^glGetCompressedMultiTexImageEXT$", "ctex_multi",
     [["texunit", "target", "lod"]]),
    (r"^glGetNamedProgramStringEXT$", "progstring_named",
     [["program", "target"]]),
    # ARB_imaging getters: sizes live in the image's own parameters
    (r"^glGetColorTable(EXT)?$", "img_ct", [["target", "format", "type"]]),
    (r"^glGetConvolutionFilter(EXT)?$", "img_conv",
     [["target", "format", "type"]]),
    (r"^glGetSeparableFilter(EXT)?$", "img_sep_row",
     [["target", "format", "type"]], "row"),
    (r"^glGetSeparableFilter(EXT)?$", "img_sep_col",
     [["target", "format", "type"]], "column"),
    (r"^glGetHistogram(EXT)?$", "img_hist", [["target", "format", "type"]]),
    (r"^glGetMinmax(EXT)?$", "img_minmax", [["target", "format", "type"]]),
    (r"^glGetProgramStringNV$", "progstring_nv", [["id", "pname"]]),
    (r"^glGetTexImage$", "pixels_tex", [["target", "level", "format", "type"]]),
    (r"^glGetCompressedTexImage(ARB)?$", "ctex", [["target", "level"]]),
    (r"^glGet(n)?PixelMap(f|ui|us)v(ARB)?$", "pixelmap", [["map"]]),
    (r"^glGet(n)?Map(d|f|i)v(ARB)?$", "map", [["target", "query"]]),
    (r"^glGetActiveUniformBlockiv$", "uniformblock",
     [["program", "uniformBlockIndex", "pname"]]),
    (r"^glGetActiveSubroutineUniformiv$", "subr_uniform",
     [["program", "shadertype", "index", "pname"]]),
    (r"^glGetActiveAtomicCounterBufferiv$", "atomic_buf",
     [["program", "bufferIndex", "pname"]]),
    (r"^glGetProgramStringARB$", "progstring", [["target", "pname"]]),
]

# Server-sized pixel getters whose result may leave gaps in the application's
# buffer (GL_PACK_ROW_LENGTH, skips): the reply also carries the transfer's
# width / height / depth and the client copies only the pixel rows.
PIXEL_SCATTER = {"pixels_tex", "pixels_dsa", "pixels_multi"}

# glGet*/gl*v pname -> element count is command specific. Regex -> class
# (see gb_pname_count_class in gb_gl_sizes.h).
PNAME_CLASSES = [
    (r"^glGet(Boolean|Double|Float|Integer|Integer64)v$", "getv"),
    (r"^glGet(Boolean|Double|Float|Integer|Integer64)i_v$", "geti_v"),
    (r"^glGet(Boolean|Double|Float|Integer)_?vEXT$", "getv"),
    (r"^glGet(Fixed|Integer64|UnsignedByte)v(OES|APPLE|EXT)?$", "getv"),
    (r"^glGet(Boolean|Double|Float|Integer|UnsignedByte)i_v(EXT|NV|OES)$",
     "geti_v"),
    (r"^gl(Get)?Light[fix]v(OES)?$", "light"),
    (r"^gl(Get)?Material[fix]v(OES)?$", "material"),
    (r"^gl(Get)?(Multi)?TexEnv[fix]v(OES|EXT)?$", "texenv"),
    (r"^gl(Get)?(Multi)?TexGen[dfix]v(OES|EXT)?$", "texgen"),
    (r"^gl(Get)?LightModel[fix]v(OES)?$", "lightmodel"),
    (r"^glFog[fix]v(OES)?$", "fog"),
    (r"^glPointParameter[fix]v(ARB|EXT|NV|OES|SGIS)?$", "pointparam"),
    # texture / sampler parameters, every spelling (legacy, EXT, DSA)
    (r"^gl(Get)?(Tex|Sampler|MultiTex|Texture)Parameter"
     r"(f|i|x|Ii|Iui|I)?v?(EXT|ARB|OES)?$", "texparam"),
    (r"^glGet(Boolean|Double|Float|Integer)IndexedvEXT$", "geti_v"),
    (r"^glGetIntegerui64vNV$", "getv"),
    (r"^glGetIntegerui64i_vNV$", "geti_v"),
    (r"^glClear(Named)?(Framebuffer|Buffer)(f|i|ui)v$", "clearbuffer"),
    # image-processing parameters: scale/bias/border colour are RGBA
    (r"^gl(Get)?(Convolution|ColorTable)Parameter[fix]v(EXT|SGI|OES)?$",
     "imgparam"),
    # the one generic attribute query that returns more than a scalar
    (r"^glGetVertexAttrib(I|L)?(f|d|i|ui|Li64|Lui64|i64|ui64)v"
     r"(EXT|NV|ARB)?$", "vattrib"),
    (r"^glGetVertexAttribL(i64|ui64)v(NV|ARB)?$", "vattrib"),
    (r"^glPatchParameterfv$", "patch"),
    (r"^glGetMultisamplefv(NV)?$", "multisample"),
]

# File descriptors. FD_PARAMS: (command, parameter) that is an fd the driver
# takes ownership of; on the wire a token (see gb_cl_fd_put). FD_ATTRIB_CMDS:
# commands whose attribute list holds fds under the keys below; the value says
# whether the driver takes ownership on success (native fence) or merely reads
# them during the call (dma-buf import).
FD_PARAMS = {
    ("eglCreateStreamFromFileDescriptorKHR", "file_descriptor"),
    ("glImportMemoryFdEXT", "fd"),
    ("glImportSemaphoreFdEXT", "fd"),
}
FD_ATTRIB_CMDS = {
    "eglCreateImage": False, "eglCreateImageKHR": False,
    "eglCreateSync": True, "eglCreateSyncKHR": True,
    "eglCreateSync64KHR": True,
}
FD_ATTRIB_KEYS = {0x3272, 0x3275, 0x3278, 0x3440, 0x3145, 0x333C}

# Array lengths the registry does not state (or states as COMPSIZE of other
# arrays' contents).
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
    "EGL_EXT_stream_acquire_mode", "EGL_NV_output_drm_flip_event",
    "EGL_WL_wayland_eglstream", "EGL_NV_nvrm_fence_sync",
    # NVIDIA GL/ES extensions with no entry points
    "GL_EXTX_framebuffer_mixed_formats", "GL_EXT_transform_feedback2",
    "GL_NV_EGL_stream_consumer_external", "GL_NV_ES3_1_compatibility",
    "GL_NV_gpu_program4_1", "GL_NV_gpu_program_fp64",
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

# C statements the server runs before the native call (argument variables are
# a_<name>). A deleted buffer name may be one of the server's own scratch
# buffers, and the name can be handed straight back to the application.
SERVER_PRE = {
    "glDeleteBuffers": "gb_sv_scratch_buffers_deleted(a_n, a_buffers);",
    "glDeleteBuffersARB": "gb_sv_scratch_buffers_deleted(a_n, a_buffers);",
}

# Calls after which the server's cached "current native context" is stale.
CTX_CHANGERS = {"eglMakeCurrent", "eglReleaseThread", "glXMakeCurrent",
                "glXMakeContextCurrent", "glXMakeCurrentReadSGI",
                "glXDestroyContext", "eglDestroyContext", "eglTerminate"}


# Persistent buffer mappings are shadow copies; these commands are where the
# GPU may consume the application's writes (UP) or produce data the
# application will read (DOWN). See gb_cl_persist_sync.
PERSIST_UP_REGEX = (
    r"^gl(Multi)?(Draw|Dispatch|Clear)|^glFlush$|^glFinish$|"
    r"^glFenceSync|^glMemoryBarrier|^gl(Copy|Blit|Read|Get)[A-Za-z]*"
    r"(Pixels|TexImage|BufferSubData|ImageSubData|Framebuffer|"
    r"TextureSubImage|CompressedTexImage)|"
    r"^gl(Tex|Texture)(Sub)?Image|^glCopyNamedBufferSubData|"
    r"^glBegin(Transform|Query|Conditional)|^glEndTransformFeedback|"
    r"^glBufferSubData|^glNamedBufferSubData|^glClearBuffer|"
    r"^glClearNamedBuffer|^glInvalidateBuffer|^glUnmapBuffer|"
    r"^(egl|glX)SwapBuffers|^eglWaitClient|^glWaitSync|"
    r"^glGetQueryObject|^glDispatchCompute|^glMultiDraw")
PERSIST_DOWN_REGEX = (
    r"^glFinish$|^glClientWaitSync|^glMemoryBarrier|^glGetSynciv|"
    r"^glWaitSync|^(egl|glX)SwapBuffers|^eglWaitClient|^glFlush$")


# Array parameters whose (negative) count the driver quietly treats as 0
# instead of reporting an error.
NEG_COUNT_IS_ZERO = {("eglGetConfigs", "configs"), ("eglChooseConfig", "configs")}

# GLchar buffers that hold several NUL-terminated strings back to back (not
# one string): copied whole.
NOT_A_STRING = {("glGetDebugMessageLog", "messageLog"),
                ("glGetDebugMessageLogKHR", "messageLog"),
                ("glGetDebugMessageLogARB", "messageLog"),
                ("glGetDebugMessageLogAMD", "message")}

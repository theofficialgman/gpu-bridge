"""
gen_config.py - hand-maintained policy for gb_gen.py.

Everything here is a *decision*, not derived from the registry. Keep each
entry commented with why. If a command is not generated and not listed in
MANUAL it will be reported as unsupported (stub that logs + returns error).
"""

# Which API versions / extensions to expose.
EGL_VERSIONS = {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5"}
GLES_VERSIONS = {"2.0", "3.0"}
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

# Commands implemented by hand on both sides (client symbol + server op).
# The generator only reserves an opcode and lists them in the proc table.
MANUAL = {
    # native display/window arguments: translated to strings / XIDs
    "eglGetDisplay", "eglGetPlatformDisplay", "eglGetPlatformDisplayEXT",
    "eglCreateWindowSurface", "eglCreatePlatformWindowSurface",
    "eglCreatePlatformWindowSurfaceEXT",
    # string / pointer returns (client-cached and extension-filtered)
    "eglQueryString", "eglGetProcAddress", "glGetString", "glGetStringi",
    # client-array handling (see ARCHITECTURE.md 5.6)
    "glShaderSource", "glVertexAttribPointer", "glDrawElements",
    "glGetVertexAttribPointerv",
}

# Generated, but the public symbol is hand-written (client-side state
# shadowing); the hand-written wrapper calls gbx_<name>().
WRAPPED = {
    # client-array / buffer-binding shadow state (ARCHITECTURE.md 5.6)
    "glDrawArrays", "glBindBuffer", "glDeleteBuffers",
    "glEnableVertexAttribArray", "glDisableVertexAttribArray",
    "glBindVertexArray", "glBindVertexArrayOES",
    "glDrawArraysInstanced",   # guarded: client arrays unsupported here
    # pixel-store shadow + PBO guard (PBO offsets are not pointers)
    "glPixelStorei", "glReadPixels", "glTexImage2D", "glTexSubImage2D",
    "glCompressedTexImage2D", "glCompressedTexSubImage2D",
    # per-thread current-context tracking / local error merging
    "eglMakeCurrent", "eglReleaseThread", "eglGetError", "glGetError",
    # GL_NUM_EXTENSIONS must match the filtered extension list
    "glGetIntegerv",
}

# Calls that must block even though they return nothing.
FORCE_SYNC = {"glFinish"}

# Commands whose registry entry lacks a usable len=.
LEN_OVERRIDES = {
    ("eglChooseConfig", "configs"): "config_size",
    ("eglGetConfigs", "configs"): "config_size",
}

# Parameters that are EGL_NONE-terminated attribute lists even though the
# naming heuristic would not catch them.
ATTRIB_LISTS = set()

# COMPSIZE(...) argument tuples -> helper key understood by the generator.
#   'pname'  : element count from a pname (GB_COUNT_PNAME)
#   'pixels' : byte count for a pixel rectangle (GB_BYTES_PIXELS)
COMPSIZE = {
    ("pname",): "pname",
    ("format", "type", "width", "height"): "pixels",
}

# Handle lifetime: command -> (handle parameter, success condition or None).
HANDLE_FREE = {
    "eglDestroyContext": ("ctx", "ret == EGL_TRUE"),
    "eglDestroySurface": ("surface", "ret == EGL_TRUE"),
    "eglDestroyImage": ("image", "ret == EGL_TRUE"),
    "eglDestroyImageKHR": ("image", "ret == EGL_TRUE"),
    "eglDestroySync": ("sync", "ret == EGL_TRUE"),
    "eglDestroySyncKHR": ("sync", "ret == EGL_TRUE"),
    "glDeleteSync": ("sync", None),
}

# Explicitly unsupported (with the reason shown in the report).
UNSUPPORTED = {}

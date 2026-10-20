/*
 * gb_client_gl.c - EGL / OpenGL ES specific part of the client library:
 *
 *  - native display/window arguments (Display* -> string, Window -> XID)
 *  - string results (cached, extension lists filtered to what we implement)
 *  - current-context tracking
 *
 * Vertex-array/buffer shadow state and the draw wrappers are shared with the
 * desktop-GL library: see gb_client_arrays.c. Wire layouts mirror
 * server/gb_server_manual.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "gb_client_impl.h"

/* ---- EGL: display / strings / proc addresses ------------------------------------- */

/* `inst` identifies the application's Display* (0 = by name): each one is a
 * separate EGLDisplay on the server, as natively. */
static EGLDisplay get_display_kind(uint32_t kind, uint32_t inst,
				   uint32_t drm_tok, const char *str)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t slen = str ? (uint32_t)strlen(str) + 1 : GB_NULL_LEN;
	uint32_t sz = 16 + 16;

	if (!cx)
		return EGL_NO_DISPLAY;
	if (str)
		sz = GB_ALIGN_UP(sz, 8u) + slen;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_GetDisplay, sz,
			    GB_CMD_F_WANT_REPLY))
		return EGL_NO_DISPLAY;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, inst);
	gb_w_u32(&w, drm_tok);
	gb_w_u32(&w, slen);
	if (str)
		memcpy(gb_w_blob(&w, slen), str, slen);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_GetDisplay))
		return EGL_NO_DISPLAY;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t id = (uint32_t)gb_r_u64(&r);
	gb_in_release(&in);
	return (EGLDisplay)(uintptr_t)id;
}

/* EGLDisplay id -> the app's Display*, so window-surface creation can make
 * the app's pending X requests (the window itself) visible first. */
static struct {
	EGLDisplay egl;
	Display *x;
} g_egl_x[8];

static EGLDisplay remember_x(EGLDisplay d, void *native)
{
	if (d != EGL_NO_DISPLAY && native) {
		for (int i = 0; i < 8; i++)
			if (g_egl_x[i].egl == d || !g_egl_x[i].egl) {
				g_egl_x[i].egl = d;
				g_egl_x[i].x = native;
				break;
			}
	}
	return d;
}

static EGLDisplay x11_display_from_native(void *native)
{
	if (!native) {
		const char *d = getenv("DISPLAY");
		return get_display_kind(1, 0, 0, d ? d : ":0");
	}
	/* The app's Display* lives in its own address space; the server opens
	 * its own connection to the same X server by name. */
	const char *name = gb_cl_xdisplay_string(native);
	if (!name) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}
	return remember_x(get_display_kind(1, (uint32_t)(uintptr_t)native, 0,
					   name),
			  native);
}

GB_API EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
	if (display_id == EGL_DEFAULT_DISPLAY)
		return get_display_kind(0, 0, 0, NULL);
	return x11_display_from_native((void *)display_id);
}

/* EGL_DRM_MASTER_FD_EXT in the display attributes: the fd goes to the server
 * under a token (the application keeps its own descriptor). */
static uint32_t drm_master_token(const EGLint *a32, const EGLAttrib *a64)
{
	for (int i = 0; a32 && i < 32 && a32[i] != EGL_NONE; i += 2)
		if (a32[i] == 0x333C)
			return gb_cl_fd_put(a32[i + 1]);
	for (int i = 0; a64 && i < 32 && a64[i] != (EGLAttrib)EGL_NONE; i += 2)
		if (a64[i] == 0x333C)
			return gb_cl_fd_put((int)a64[i + 1]);
	return 0;
}

static EGLDisplay platform_display(EGLenum platform, void *native,
				   const EGLint *attribs32,
				   const EGLAttrib *attribs64)
{
	switch (platform) {
	case 0x31D5: /* EGL_PLATFORM_X11_{KHR,EXT} */
		return x11_display_from_native(native);
	case 0x31DD: /* EGL_PLATFORM_SURFACELESS_MESA */
		return get_display_kind(2, 0, 0, NULL);
	case 0x313F: /* EGL_PLATFORM_DEVICE_EXT: native is a device handle */
		if (!native) {
			gb_cl_egl_error(EGL_BAD_ATTRIBUTE);
			return EGL_NO_DISPLAY;
		}
		return get_display_kind(3, (uint32_t)(uintptr_t)native,
					drm_master_token(attribs32, attribs64),
					NULL);
	default:
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}
}

GB_API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display,
					const EGLAttrib *attrib_list)
{
	return platform_display(platform, native_display, NULL, attrib_list);
}

GB_API EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform,
					   void *native_display,
					   const EGLint *attrib_list)
{
	return platform_display(platform, native_display, attrib_list, NULL);
}

/* The server draws through its own X connection: make the application's
 * requests (window/pixmap creation, resizes) visible first. */
static void sync_x_for(EGLDisplay dpy)
{
	for (int i = 0; i < 8; i++)
		if (g_egl_x[i].egl == dpy && g_egl_x[i].x)
			gb_cl_xsync(g_egl_x[i].x);
}

static EGLSurface create_window_surface(EGLDisplay dpy, EGLConfig cfg,
					uint32_t xid, const EGLint *attribs)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	int64_t n = attribs ? gb_attrib_count_EGLint(attribs) : 0;
	uint32_t alen = attribs ? (uint32_t)n * 4 : GB_NULL_LEN;
	uint32_t sz = 16 + 16;

	if (!cx)
		return EGL_NO_SURFACE;
	sync_x_for(dpy);
	if (attribs && n < 0) {
		gb_cl_egl_error(EGL_BAD_ATTRIBUTE);
		return EGL_NO_SURFACE;
	}
	if (attribs)
		sz = GB_ALIGN_UP(sz, 8u) + alen;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_CreateWindowSurface, sz,
			    GB_CMD_F_WANT_REPLY))
		return EGL_NO_SURFACE;
	gb_w_u32(&w, (uint32_t)(uintptr_t)dpy);
	gb_w_u32(&w, (uint32_t)(uintptr_t)cfg);
	gb_w_u32(&w, xid);
	gb_w_u32(&w, alen);
	if (attribs)
		memcpy(gb_w_blob(&w, alen), attribs, alen);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_CreateWindowSurface))
		return EGL_NO_SURFACE;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t id = (uint32_t)gb_r_u64(&r);
	gb_in_release(&in);
	return (EGLSurface)(uintptr_t)id;
}

GB_API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
					 EGLNativeWindowType win,
					 const EGLint *attrib_list)
{
	return create_window_surface(dpy, config, (uint32_t)(uintptr_t)win,
				     attrib_list);
}

GB_API EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy,
						    EGLConfig config,
						    void *native_window,
						    const EGLint *attrib_list)
{
	/* EGL_EXT_platform_x11: native_window points at a Window. */
	if (!native_window) {
		gb_cl_egl_error(EGL_BAD_NATIVE_WINDOW);
		return EGL_NO_SURFACE;
	}
	return create_window_surface(dpy, config,
				     (uint32_t)*(unsigned long *)native_window,
				     attrib_list);
}

GB_API EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy,
						 EGLConfig config,
						 void *native_window,
						 const EGLAttrib *attrib_list)
{
	EGLint tmp[GB_ATTRIB_MAX + 1];
	const EGLint *p = NULL;

	if (attrib_list) {
		int64_t n = gb_attrib_count_EGLAttrib(
			(const intptr_t *)attrib_list);
		if (n < 0 || n > GB_ATTRIB_MAX) {
			gb_cl_egl_error(EGL_BAD_ATTRIBUTE);
			return EGL_NO_SURFACE;
		}
		for (int64_t i = 0; i < n; i++)
			tmp[i] = (EGLint)attrib_list[i];
		p = tmp;
	}
	return eglCreatePlatformWindowSurfaceEXT(dpy, config, native_window, p);
}

GB_API EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
					 EGLNativePixmapType pixmap,
					 const EGLint *attrib_list)
{
	sync_x_for(dpy);
	return gbx_eglCreatePixmapSurface(dpy, config, pixmap, attrib_list);
}

GB_API EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay dpy,
						    EGLConfig config,
						    void *native_pixmap,
						    const EGLint *attrib_list)
{
	/* EGL_EXT_platform_x11: native_pixmap points at a Pixmap. */
	if (!native_pixmap) {
		gb_cl_egl_error(EGL_BAD_NATIVE_PIXMAP);
		return EGL_NO_SURFACE;
	}
	return eglCreatePixmapSurface(dpy, config,
				      (EGLNativePixmapType)*(unsigned long *)
					      native_pixmap,
				      attrib_list);
}

GB_API EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy,
						 EGLConfig config,
						 void *native_pixmap,
						 const EGLAttrib *attrib_list)
{
	EGLint tmp[GB_ATTRIB_MAX + 1];
	const EGLint *p = NULL;

	if (attrib_list) {
		int64_t n = gb_attrib_count_EGLAttrib(
			(const intptr_t *)attrib_list);
		if (n < 0 || n > GB_ATTRIB_MAX) {
			gb_cl_egl_error(EGL_BAD_ATTRIBUTE);
			return EGL_NO_SURFACE;
		}
		for (int64_t i = 0; i < n; i++)
			tmp[i] = (EGLint)attrib_list[i];
		p = tmp;
	}
	return eglCreatePlatformPixmapSurfaceEXT(dpy, config, native_pixmap, p);
}

/* ---- file descriptors in attribute lists ----------------------------------------------
 * dma-buf planes, a native fence and the DRM master fd are descriptors of this
 * process: they travel as tokens (control socket) and the server swaps in its
 * own copies. The lists are copied; the application's list is untouched. */

static int fd_attrib_key(long k)
{
	return k == 0x3272 || k == 0x3275 || k == 0x3278 || k == 0x3440 ||
	       k == 0x3145 || k == 0x333C;
}

#define GB_MAX_FD_ATTRIBS 32

struct fd_list {
	int fds[GB_MAX_FD_ATTRIBS];
	int n;
};

#define DEFINE_FD_ATTRIBS(NAME, T, NONE)                                     \
static T *NAME(const T *a, struct fd_list *fl)                                \
{                                                                             \
	int n = 0;                                                            \
	T *c;                                                                 \
                                                                              \
	fl->n = 0;                                                            \
	if (!a)                                                               \
		return NULL;                                                  \
	while (n < 2 * GB_MAX_FD_ATTRIBS && a[n] != (T)(NONE))                \
		n += 2;                                                       \
	c = malloc((size_t)(n + 1) * sizeof(T));                              \
	if (!c)                                                               \
		return NULL;                                                  \
	memcpy(c, a, (size_t)n * sizeof(T));                                  \
	c[n] = (T)(NONE);                                                     \
	for (int i = 0; i < n; i += 2) {                                      \
		if (!fd_attrib_key((long)c[i]) || (long)c[i + 1] == -1)       \
			continue;                                             \
		if (fl->n < GB_MAX_FD_ATTRIBS)                                \
			fl->fds[fl->n++] = (int)c[i + 1];                     \
		c[i + 1] = (T)gb_cl_fd_put((int)c[i + 1]);                    \
	}                                                                     \
	return c;                                                             \
}

DEFINE_FD_ATTRIBS(fd_attribs_i32, EGLint, EGL_NONE)
DEFINE_FD_ATTRIBS(fd_attribs_attrib, EGLAttrib, EGL_NONE)

GB_API EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target,
			       EGLClientBuffer buffer,
			       const EGLAttrib *attrib_list)
{
	struct fd_list fl;
	EGLAttrib *c = fd_attribs_attrib(attrib_list, &fl);
	EGLImage r = gbx_eglCreateImage(dpy, ctx, target, buffer,
					c ? c : attrib_list);

	free(c);
	return r;
}

GB_API EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx,
				     EGLenum target, EGLClientBuffer buffer,
				     const EGLint *attrib_list)
{
	struct fd_list fl;
	EGLint *c = fd_attribs_i32(attrib_list, &fl);
	EGLImageKHR r = gbx_eglCreateImageKHR(dpy, ctx, target, buffer,
					      c ? c : attrib_list);

	free(c);
	return r;
}

/* A native fence passed to sync creation is owned by the sync object on
 * success: the application's descriptor is closed here, as the driver would
 * have taken it. */
static void close_fds_if(struct fd_list *fl, int success)
{
	if (success)
		for (int i = 0; i < fl->n; i++)
			if (fl->fds[i] >= 0)
				close(fl->fds[i]);
}

GB_API EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type,
			     const EGLAttrib *attrib_list)
{
	struct fd_list fl;
	EGLAttrib *c = fd_attribs_attrib(attrib_list, &fl);
	EGLSync r = gbx_eglCreateSync(dpy, type, c ? c : attrib_list);

	close_fds_if(&fl, r != EGL_NO_SYNC);
	free(c);
	return r;
}

GB_API EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type,
				   const EGLint *attrib_list)
{
	struct fd_list fl;
	EGLint *c = fd_attribs_i32(attrib_list, &fl);
	EGLSyncKHR r = gbx_eglCreateSyncKHR(dpy, type, c ? c : attrib_list);

	close_fds_if(&fl, r != EGL_NO_SYNC_KHR);
	free(c);
	return r;
}

GB_API EGLSync eglCreateSync64KHR(EGLDisplay dpy, EGLenum type,
				  const EGLAttribKHR *attrib_list)
{
	struct fd_list fl;
	EGLAttrib *c = fd_attribs_attrib((const EGLAttrib *)attrib_list, &fl);
	EGLSync r = gbx_eglCreateSync64KHR(dpy, type,
					   c ? (const EGLAttribKHR *)c
					     : attrib_list);

	close_fds_if(&fl, r != EGL_NO_SYNC);
	free(c);
	return r;
}

/* ---- calls that return descriptors ---------------------------------------------------- */

static int fd_op(uint32_t sub, uint32_t dpy, uint32_t obj, uint32_t *out)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_FdOp, 32,
				   GB_CMD_F_WANT_REPLY))
		return -1;
	gb_w_u32(&w, sub);
	gb_w_u32(&w, dpy);
	gb_w_u32(&w, obj);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_FdOp))
		return -1;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	for (int i = 0; i < 16; i++)
		out[i] = gb_r_u32(&r);
	int bad = r.err;
	gb_in_release(&in);
	return bad ? -1 : 0;
}

GB_API EGLNativeFileDescriptorKHR eglGetStreamFileDescriptorKHR(
	EGLDisplay dpy, EGLStreamKHR stream)
{
	uint32_t o[16];

	if (fd_op(3, (uint32_t)(uintptr_t)dpy, (uint32_t)(uintptr_t)stream, o) ||
	    (int32_t)o[0] < 0)
		return EGL_NO_FILE_DESCRIPTOR_KHR;
	return gb_cl_fd_get(o[1]);
}

GB_API EGLStreamKHR eglCreateStreamFromFileDescriptorKHR(
	EGLDisplay dpy, EGLNativeFileDescriptorKHR file_descriptor)
{
	/* the server gets its own copy of the descriptor (as a token) */
	return gbx_eglCreateStreamFromFileDescriptorKHR(
		dpy, (EGLNativeFileDescriptorKHR)gb_cl_fd_put(file_descriptor));
}

static const char *obj_string(uint32_t kind, EGLDisplay dpy, void *obj,
			      EGLint name)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_ObjString, 32,
				   GB_CMD_F_WANT_REPLY))
		return NULL;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, (uint32_t)(uintptr_t)dpy);
	gb_w_u32(&w, (uint32_t)(uintptr_t)obj);
	gb_w_u32(&w, (uint32_t)name);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_ObjString))
		return NULL;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t len = gb_r_u32(&r);
	const char *res = NULL;

	if (len != GB_NULL_LEN && len > 0 && len <= (1u << 20)) {
		const char *s = gb_r_blob(&r, len);

		if (s && !r.err && s[len - 1] == 0)
			res = gb_cl_intern(s, len - 1);
	}
	gb_in_release(&in);
	return res;
}

GB_API const char *eglQueryOutputLayerStringEXT(EGLDisplay dpy,
						EGLOutputLayerEXT layer,
						EGLint name)
{
	return obj_string(0, dpy, layer, name);
}

GB_API const char *eglQueryOutputPortStringEXT(EGLDisplay dpy,
					       EGLOutputPortEXT port,
					       EGLint name)
{
	return obj_string(1, dpy, port, name);
}

GB_API EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, EGLSyncKHR sync)
{
	uint32_t o[16];

	if (fd_op(0, (uint32_t)(uintptr_t)dpy, (uint32_t)(uintptr_t)sync, o) ||
	    (int32_t)o[0] < 0)
		return -1; /* EGL_NO_NATIVE_FENCE_FD_ANDROID */
	return gb_cl_fd_get(o[1]);
}

GB_API EGLBoolean eglExportDMABUFImageQueryMESA(EGLDisplay dpy, EGLImageKHR image,
						int *fourcc, int *num_planes,
						EGLuint64KHR *modifiers)
{
	uint32_t o[16];

	if (fd_op(1, (uint32_t)(uintptr_t)dpy, (uint32_t)(uintptr_t)image, o) ||
	    !o[0])
		return EGL_FALSE;
	if (fourcc)
		*fourcc = (int)o[1];
	if (num_planes)
		*num_planes = (int)o[2];
	for (uint32_t i = 0; modifiers && i < o[2] && i < 4; i++) {
		uint64_t m;

		memcpy(&m, &o[4 + 2 * i], 8);
		modifiers[i] = m;
	}
	return EGL_TRUE;
}

GB_API EGLBoolean eglExportDMABUFImageMESA(EGLDisplay dpy, EGLImageKHR image,
					   int *fds, EGLint *strides,
					   EGLint *offsets)
{
	uint32_t o[16];

	if (fd_op(2, (uint32_t)(uintptr_t)dpy, (uint32_t)(uintptr_t)image, o) ||
	    !o[0])
		return EGL_FALSE;
	for (int i = 0; i < 4; i++) {
		if (!o[2 + i])
			continue;
		if (fds)
			fds[i] = gb_cl_fd_get(o[2 + i]);
		else
			close(gb_cl_fd_get(o[2 + i]));
		if (strides)
			strides[i] = (EGLint)o[6 + i];
		if (offsets)
			offsets[i] = (EGLint)o[10 + i];
	}
	return EGL_TRUE;
}

/* ---- EGL_MESA_query_driver ---------------------------------------------------------- */

GB_API const char *eglGetDisplayDriverName(EGLDisplay dpy)
{
	return gb_cl_fetch_string(GB_OPX_DriverString,
				  (uint32_t)(uintptr_t)dpy, 0);
}

/* The application owns (and free()s) the configuration string. */
GB_API char *eglGetDisplayDriverConfig(EGLDisplay dpy)
{
	const char *s = gb_cl_fetch_string(GB_OPX_DriverString,
					   (uint32_t)(uintptr_t)dpy, 1);

	return s ? strdup(s) : NULL;
}

/* ---- EGL_KHR_debug --------------------------------------------------------------- */

static EGLDEBUGPROCKHR g_egl_dbg;

void gb_cl_egl_debug_dispatch(uint32_t error, const char *command,
			      uint32_t type, uint64_t thread_label,
			      uint64_t object_label, const char *message)
{
	if (g_egl_dbg)
		g_egl_dbg((EGLenum)error, command, (EGLint)type,
			  (EGLLabelKHR)(uintptr_t)thread_label,
			  (EGLLabelKHR)(uintptr_t)object_label, message);
}

/* one request/reply on OPX_EglDebug: returns the EGLint result (or -1) */
static int64_t egl_debug_call(uint32_t sub, const uint32_t *w, unsigned nw,
			      uint64_t label, uint64_t *value)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w wr;

	if (!cx || gb_cl_msg_begin(cx, &o, &wr, GB_OPX_EglDebug,
				   16 + 8 + 4 * nw + 8 + 8,
				   GB_CMD_F_WANT_REPLY))
		return -1;
	gb_w_u32(&wr, sub);
	gb_w_u32(&wr, nw);
	for (unsigned i = 0; i < nw; i++)
		gb_w_u32(&wr, w[i]);
	gb_w_u64(&wr, label);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_EglDebug))
		return -1;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	int64_t res = (int32_t)gb_r_u32(&r);
	(void)gb_r_u32(&r);
	uint64_t v = gb_r_u64(&r);
	if (value)
		*value = v;
	gb_in_release(&in);
	return res;
}

GB_API EGLint eglDebugMessageControlKHR(EGLDEBUGPROCKHR callback,
					const EGLAttrib *attrib_list)
{
	uint32_t w[17];
	unsigned nw = 1;

	w[0] = callback ? 1u : 0u;
	for (int i = 0; attrib_list && i < 16 &&
	     attrib_list[i] != (EGLAttrib)EGL_NONE && nw < 16; i += 2) {
		w[nw++] = (uint32_t)attrib_list[i];
		w[nw++] = (uint32_t)attrib_list[i + 1];
	}
	g_egl_dbg = callback;
	int64_t r = egl_debug_call(0, w, nw, 0, NULL);

	return r < 0 ? EGL_BAD_ALLOC : (EGLint)r;
}

GB_API EGLBoolean eglQueryDebugKHR(EGLint attribute, EGLAttrib *value)
{
	uint64_t v = 0;

	if (!value) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	if (attribute == 0x33B8 /* EGL_DEBUG_CALLBACK_KHR */) {
		*value = (EGLAttrib)(uintptr_t)g_egl_dbg;
		return EGL_TRUE;
	}
	uint32_t a = (uint32_t)attribute;
	int64_t r = egl_debug_call(2, &a, 1, 0, &v);

	if (r != 1)
		return EGL_FALSE;
	*value = (EGLAttrib)v;
	return EGL_TRUE;
}

GB_API EGLint eglLabelObjectKHR(EGLDisplay display, EGLenum objectType,
				EGLObjectKHR object, EGLLabelKHR label)
{
	uint32_t w[3] = { (uint32_t)(uintptr_t)display, (uint32_t)objectType,
			  (uint32_t)(uintptr_t)object };
	int64_t r = egl_debug_call(1, w, 3, (uint64_t)(uintptr_t)label, NULL);

	return r < 0 ? EGL_BAD_ALLOC : (EGLint)r;
}

/* ---- EGL_ANDROID_blob_cache ------------------------------------------------------ */

static EGLSetBlobFuncANDROID g_blob_set;
static EGLGetBlobFuncANDROID g_blob_get;

GB_API void eglSetBlobCacheFuncsANDROID(EGLDisplay dpy,
					EGLSetBlobFuncANDROID set,
					EGLGetBlobFuncANDROID get)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx)
		return;
	if (!set || !get) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return;
	}
	if (g_blob_set || g_blob_get) {
		gb_cl_egl_error(EGL_BAD_PARAMETER); /* already set (BAD_ACCESS
						     * in the spec; either
						     * way: unchanged) */
		return;
	}
	g_blob_set = set;
	g_blob_get = get;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_BlobFuncs, 24, 0))
		return;
	gb_w_u32(&w, (uint32_t)(uintptr_t)dpy);
	gb_w_u32(&w, 1);
	gb_cl_end(cx, &o);
}

void gb_cl_blob_set(const void *key, long ksz, const void *val, long vsz)
{
	if (g_blob_set)
		g_blob_set(key, (EGLsizeiANDROID)ksz, val, (EGLsizeiANDROID)vsz);
}

long gb_cl_blob_get(const void *key, long ksz, void *val, long vmax)
{
	return g_blob_get ? (long)g_blob_get(key, (EGLsizeiANDROID)ksz, val,
					     (EGLsizeiANDROID)vmax) : 0;
}

/* ---- EGL_EXT_device_* ----------------------------------------------------------------- */

/* One request/reply; fills ok/n/value and up to `nmax` handles. */
static int device_call(uint32_t sub, uint32_t a, uint32_t b, uint32_t *ok,
		       uint32_t *n, uint64_t *value, uint32_t *hs,
		       uint32_t nmax)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_EglDevice, 32,
				   GB_CMD_F_WANT_REPLY))
		return -1;
	gb_w_u32(&w, sub);
	gb_w_u32(&w, a);
	gb_w_u32(&w, b);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_EglDevice))
		return -1;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	*ok = gb_r_u32(&r);
	*n = gb_r_u32(&r);
	*value = gb_r_u64(&r);
	for (uint32_t i = 0; *ok && i < *n && i < nmax; i++)
		hs[i] = gb_r_u32(&r);
	int bad = r.err;
	gb_in_release(&in);
	return bad ? -1 : 0;
}

GB_API EGLBoolean eglQueryDevicesEXT(EGLint max_devices,
				     EGLDeviceEXT *devices, EGLint *num_devices)
{
	uint32_t ok = 0, n = 0, hs[16];
	uint64_t v;

	/* EGL_EXT_device_enumeration: BAD_PARAMETER if max_devices <= 0 and
	 * devices is not NULL, or if num_devices is NULL. */
	if ((max_devices <= 0 && devices) || !num_devices) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	if (!devices)
		max_devices = 0;
	if (device_call(0, (uint32_t)max_devices, 0, &ok, &n, &v, hs, 16) ||
	    !ok)
		return EGL_FALSE;
	*num_devices = (EGLint)n;
	for (uint32_t i = 0; i < n && i < 16 && (EGLint)i < max_devices; i++)
		devices[i] = (EGLDeviceEXT)(uintptr_t)hs[i];
	return EGL_TRUE;
}

GB_API const char *eglQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name)
{
	return gb_cl_fetch_string(GB_OPX_DeviceString,
				  (uint32_t)(uintptr_t)device, (uint32_t)name);
}

GB_API EGLBoolean eglQueryDeviceAttribEXT(EGLDeviceEXT device, EGLint attribute,
					  EGLAttrib *value)
{
	uint32_t ok = 0, n = 0;
	uint64_t v = 0;

	if (!value) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	if (device_call(2, (uint32_t)(uintptr_t)device, (uint32_t)attribute,
			&ok, &n, &v, NULL, 0) || !ok)
		return EGL_FALSE;
	*value = (EGLAttrib)v;
	return EGL_TRUE;
}

GB_API EGLBoolean eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute,
					   EGLAttrib *value)
{
	uint32_t ok = 0, n = 0;
	uint64_t v = 0;

	if (!value) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	if (device_call(3, (uint32_t)(uintptr_t)dpy, (uint32_t)attribute, &ok,
			&n, &v, NULL, 0) || !ok)
		return EGL_FALSE;
	*value = (EGLAttrib)v;
	return EGL_TRUE;
}

/* ---- strings -------------------------------------------------------------------------- */

GB_API const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
	const char *s = gb_cl_fetch_string(GB_OPX_QueryString,
					   (uint32_t)(uintptr_t)dpy,
					   (uint32_t)name);
	if (s && name == EGL_EXTENSIONS)
		s = gb_cl_filter_ext(s);
	return s;
}

GB_API __eglMustCastToProperFunctionPointerType eglGetProcAddress(
	const char *procname)
{
	if (!procname)
		return NULL;
	for (const struct gb_proc *p = gb_proc_table; p->name; p++)
		if (!strcmp(p->name, procname))
			return (__eglMustCastToProperFunctionPointerType)p->addr;
	return NULL;
}

/* ---- error merging, current context ---------------------------------------------------- */

GB_API EGLint eglGetError(void)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && cx->egl_error) {
		EGLint e = cx->egl_error;
		cx->egl_error = 0;
		return e;
	}
	return gbx_eglGetError();
}

GB_API EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
	EGLBoolean r = gbx_eglDestroyContext(dpy, ctx);

	gb_cl_loc_invalidate_all();
	if (r)
		gb_state_free((uint32_t)(uintptr_t)ctx);
	return r;
}

GB_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
				 EGLSurface read, EGLContext ctx)
{
	EGLBoolean r = gbx_eglMakeCurrent(dpy, draw, read, ctx);
	struct gb_cl *cx = gb_cl_get();

	if (r && cx) {
		cx->cur = ctx ? gb_state_for((uint32_t)(uintptr_t)ctx) : NULL;
		gb_cl_debug_update_sync(cx);
	}
	return r;
}

GB_API EGLBoolean eglReleaseThread(void)
{
	EGLBoolean r = gbx_eglReleaseThread();
	struct gb_cl *cx = gb_cl_get();

	if (cx)
		cx->cur = NULL;
	return r;
}

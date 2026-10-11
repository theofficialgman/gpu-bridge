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

#include "gb_client_impl.h"

/* ---- EGL: display / strings / proc addresses ------------------------------------- */

/* `inst` identifies the application's Display* (0 = by name): each one is a
 * separate EGLDisplay on the server, as natively. */
static EGLDisplay get_display_kind(uint32_t kind, uint32_t inst,
				   const char *str)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t slen = str ? (uint32_t)strlen(str) + 1 : GB_NULL_LEN;
	uint32_t sz = 16 + 12;

	if (!cx)
		return EGL_NO_DISPLAY;
	if (str)
		sz = GB_ALIGN_UP(sz, 8u) + slen;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_GetDisplay, sz,
			    GB_CMD_F_WANT_REPLY))
		return EGL_NO_DISPLAY;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, inst);
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
		return get_display_kind(1, 0, d ? d : ":0");
	}
	/* The app's Display* lives in its own address space; the server opens
	 * its own connection to the same X server by name. */
	const char *name = gb_cl_xdisplay_string(native);
	if (!name) {
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}
	return remember_x(get_display_kind(1, (uint32_t)(uintptr_t)native, name),
			  native);
}

GB_API EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
	if (display_id == EGL_DEFAULT_DISPLAY)
		return get_display_kind(0, 0, NULL);
	return x11_display_from_native((void *)display_id);
}

static EGLDisplay platform_display(EGLenum platform, void *native)
{
	switch (platform) {
	case 0x31D5: /* EGL_PLATFORM_X11_{KHR,EXT} */
		return x11_display_from_native(native);
	case 0x31DD: /* EGL_PLATFORM_SURFACELESS_MESA */
		return get_display_kind(2, 0, NULL);
	default:
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}
}

GB_API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display,
					const EGLAttrib *attrib_list)
{
	return platform_display(platform, native_display);
}

GB_API EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform,
					   void *native_display,
					   const EGLint *attrib_list)
{
	return platform_display(platform, native_display);
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

/* EGLImage creation: GL objects and X pixmaps are named by integers that are
 * the same on both sides; a dma-buf is a file descriptor of *this* process
 * and would name an unrelated descriptor in the server. */
#define GB_EGL_LINUX_DMA_BUF_EXT 0x3270

GB_API EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target,
			       EGLClientBuffer buffer,
			       const EGLAttrib *attrib_list)
{
	if (target == GB_EGL_LINUX_DMA_BUF_EXT) {
		gb_cl_unsupported("EGLImage from dma-buf (fd passing)");
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_NO_IMAGE;
	}
	return gbx_eglCreateImage(dpy, ctx, target, buffer, attrib_list);
}

GB_API EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx,
				     EGLenum target, EGLClientBuffer buffer,
				     const EGLint *attrib_list)
{
	if (target == GB_EGL_LINUX_DMA_BUF_EXT) {
		gb_cl_unsupported("EGLImage from dma-buf (fd passing)");
		gb_cl_egl_error(EGL_BAD_PARAMETER);
		return EGL_NO_IMAGE_KHR;
	}
	return gbx_eglCreateImageKHR(dpy, ctx, target, buffer, attrib_list);
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

GB_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
				 EGLSurface read, EGLContext ctx)
{
	EGLBoolean r = gbx_eglMakeCurrent(dpy, draw, read, ctx);
	struct gb_cl *cx = gb_cl_get();

	if (r && cx)
		cx->cur = ctx ? gb_state_for((uint32_t)(uintptr_t)ctx) : NULL;
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

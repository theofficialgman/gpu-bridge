/*
 * gb_client_glx.c - GLX specific part of the desktop-GL client library
 * (libGL replacement): strings, FBConfig lists, proc addresses and
 * current-context tracking.
 *
 * Window-system model (X11 only): the server opens its own X connection
 * and renders into the client's windows by XID; Display* never crosses the
 * wire, it is mapped to a server-side connection id (gb_cl_xdpy_id).
 */
#define _GNU_SOURCE
#include <stdio.h>

#include "gb_client_impl.h"

#define GLX_EXTENSIONS_NAME 3

/* GLXString(kind, dpy, screen, name) -> string */
static const char *glx_string(uint32_t kind, Display *dpy, int screen,
			      uint32_t name)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t id = gb_cl_xdpy_id(dpy);

	if (!cx || !id ||
	    gb_cl_msg_begin(cx, &o, &w, GB_OPX_GLXString, 32,
			    GB_CMD_F_WANT_REPLY))
		return NULL;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, id);
	gb_w_u32(&w, (uint32_t)screen);
	gb_w_u32(&w, name);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_GLXString))
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

GB_API const char *glXQueryExtensionsString(Display *dpy, int screen)
{
	const char *s = glx_string(2, dpy, screen, 0);
	return s ? gb_cl_filter_ext(s) : NULL;
}

GB_API const char *glXGetClientString(Display *dpy, int name)
{
	const char *s = glx_string(0, dpy, 0, (uint32_t)name);
	return (s && name == GLX_EXTENSIONS_NAME) ? gb_cl_filter_ext(s) : s;
}

GB_API const char *glXQueryServerString(Display *dpy, int screen, int name)
{
	const char *s = glx_string(1, dpy, screen, (uint32_t)name);
	return (s && name == GLX_EXTENSIONS_NAME) ? gb_cl_filter_ext(s) : s;
}

/* glXGetFBConfigs / glXChooseFBConfig: array of opaque handles, XFree-able
 * (plain malloc: Xlib's XFree is free() for this kind of memory). */
static GLXFBConfig *fbconfigs(uint32_t kind, Display *dpy, int screen,
			      const int *attribs, int *nelements)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t id = gb_cl_xdpy_id(dpy);
	int64_t n = attribs ? gb_glx_attrib_count(attribs, 0) : 0;
	uint32_t alen = attribs ? (uint32_t)n * 4 : GB_NULL_LEN;
	uint32_t sz = 16 + 16;

	if (nelements)
		*nelements = 0;
	if (!cx || !id || (attribs && n < 0))
		return NULL;
	if (attribs)
		sz = GB_ALIGN_UP(sz, 8u) + alen;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_GLXFBConfigs, sz,
			    GB_CMD_F_WANT_REPLY))
		return NULL;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, id);
	gb_w_u32(&w, (uint32_t)screen);
	gb_w_u32(&w, alen);
	if (attribs)
		memcpy(gb_w_blob(&w, alen), attribs, alen);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_GLXFBConfigs))
		return NULL;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t cnt = gb_r_u32(&r);
	GLXFBConfig *out = NULL;
	if (!r.err && cnt > 0 && cnt <= 65536 &&
	    r.size - r.off >= 4u * cnt) {
		out = malloc((size_t)cnt * sizeof(GLXFBConfig));
		for (uint32_t i = 0; out && i < cnt; i++)
			out[i] = (GLXFBConfig)(uintptr_t)gb_r_u32(&r);
		if (out && nelements)
			*nelements = (int)cnt;
	}
	gb_in_release(&in);
	return out;
}

GB_API GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
	return fbconfigs(0, dpy, screen, NULL, nelements);
}

GB_API GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen,
				      const int *attrib_list, int *nelements)
{
	return fbconfigs(1, dpy, screen, attrib_list, nelements);
}

/* ---- proc addresses ---------------------------------------------------------------- */

GB_API void (*glXGetProcAddressARB(const GLubyte *procName))(void)
{
	if (!procName)
		return NULL;
	for (const struct gb_proc *p = gb_proc_table; p->name; p++)
		if (!strcmp(p->name, (const char *)procName))
			return (void (*)(void))p->addr;
	gb_cl_unsupported((const char *)procName); /* GB_LOG_MISSING */
	return NULL;
}

GB_API void (*glXGetProcAddress(const GLubyte *procName))(void)
{
	return glXGetProcAddressARB(procName);
}

/* ---- current context ---------------------------------------------------------------- */

GB_API Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
	Bool r = gbx_glXMakeCurrent(dpy, drawable, ctx);
	struct gb_cl *cx = gb_cl_get();

	if (r && cx) {
		cx->cur = ctx ? gb_state_for((uint32_t)(uintptr_t)ctx) : NULL;
		gb_cl_debug_update_sync(cx);
		cx->cur_xdpy = ctx ? dpy : NULL;
	}
	return r;
}

GB_API Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw,
				  GLXDrawable read, GLXContext ctx)
{
	Bool r = gbx_glXMakeContextCurrent(dpy, draw, read, ctx);
	struct gb_cl *cx = gb_cl_get();

	if (r && cx) {
		cx->cur = ctx ? gb_state_for((uint32_t)(uintptr_t)ctx) : NULL;
		gb_cl_debug_update_sync(cx);
		cx->cur_xdpy = ctx ? dpy : NULL;
	}
	return r;
}

GB_API Display *glXGetCurrentDisplay(void)
{
	struct gb_cl *cx = gb_cl_get();
	return cx ? cx->cur_xdpy : NULL;
}

/* ---- ARB shader objects: handle type differs, semantics do not ------------------------ */

GB_API void glShaderSourceARB(GLhandleARB shaderObj, GLsizei count,
			      const GLcharARB **string, const GLint *length)
{
	glShaderSource((GLuint)shaderObj, count, (const GLchar *const *)string,
		       length);
}

GB_API void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	gbx_glXDestroyContext(dpy, ctx);
	gb_state_free((uint32_t)(uintptr_t)ctx);
}

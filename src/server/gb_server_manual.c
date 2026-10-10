/*
 * gb_server_manual.c - hand-written server handlers for operations that the
 * registry-driven generator cannot express safely: native display/window
 * arguments, string returns, glShaderSource, and client-memory vertex arrays.
 *
 * Wire layouts (all fields after the 16-byte gb_cmd_hdr; u64 are 8-aligned,
 * blobs 8-aligned) must match client/gb_client_gl.c.
 */
#include <stddef.h>
#include <stdio.h>

#include "gb_server_impl.h"

#define MAX_ATTRIBS 32

/* ---- per-native-context scratch buffers for client arrays ----------------- */

struct gb_scratch {
	struct gb_scratch *next;
	EGLContext ctx;
	GLuint vbo[MAX_ATTRIBS];
	GLuint ebo;
};

static struct gb_scratch *scratch_for(struct gb_sth *t)
{
	GB_NATIVE(eglGetCurrentContext, EGLContext, (void));
	EGLContext cur = fn_eglGetCurrentContext ? fn_eglGetCurrentContext()
						 : EGL_NO_CONTEXT;
	if (cur == EGL_NO_CONTEXT)
		return NULL;
	for (struct gb_scratch *s = t->scratch; s; s = s->next)
		if (s->ctx == cur)
			return s;
	struct gb_scratch *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->ctx = cur;
	s->next = t->scratch;
	t->scratch = s;
	return s;
}

void gb_srv_scratch_free(struct gb_sth *t)
{
	while (t->scratch) {
		struct gb_scratch *s = t->scratch;
		t->scratch = s->next;
		free(s);
	}
}

/* ---- replies --------------------------------------------------------------- */

static void reply_u64(struct gb_sth *t, const struct gb_cmd_hdr *h, uint64_t v)
{
	struct gb_out o;
	struct gb_w w;

	if (!(h->flags & GB_CMD_F_WANT_REPLY) ||
	    gb_sv_reply_begin(t, h, &o, 24))
		return;
	gb_w_init(&w, o.p);
	gb_w_u64(&w, v);
	gb_sv_reply_end(t, &o);
}

static void reply_string(struct gb_sth *t, const struct gb_cmd_hdr *h,
			 const char *s)
{
	struct gb_out o;
	struct gb_w w;
	uint32_t len = s ? (uint32_t)strlen(s) + 1 : GB_NULL_LEN;
	uint32_t total = 20;

	if (!(h->flags & GB_CMD_F_WANT_REPLY))
		return;
	if (s)
		total = GB_ALIGN_UP(total, 8u) + len;
	total = GB_ALIGN_UP(total, 8u);
	if (gb_sv_reply_begin(t, h, &o, total))
		return;
	gb_w_init(&w, o.p);
	gb_w_u32(&w, len);
	if (s)
		memcpy(gb_w_blob(&w, len), s, len);
	gb_sv_reply_end(t, &o);
}

/* ---- X11 display connections ------------------------------------------------ */

struct xconn {
	struct xconn *next;
	char *name;
	void *dpy;
};
static struct xconn *g_xconns;
static pthread_mutex_t g_xlock = PTHREAD_MUTEX_INITIALIZER;

static void *x11_open(const char *name)
{
	static void *(*xopen)(const char *);
	static int tried;
	void *dpy = NULL;

	pthread_mutex_lock(&g_xlock);
	if (!tried) {
		void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
		if (lib)
			xopen = (void *(*)(const char *))dlsym(lib, "XOpenDisplay");
		tried = 1;
	}
	for (struct xconn *c = g_xconns; c; c = c->next)
		if (!strcmp(c->name, name)) {
			dpy = c->dpy;
			break;
		}
	if (!dpy && xopen) {
		dpy = xopen(name);
		if (dpy) {
			struct xconn *c = calloc(1, sizeof(*c));
			c->name = strdup(name);
			c->dpy = dpy;
			c->next = g_xconns;
			g_xconns = c;
		}
	}
	pthread_mutex_unlock(&g_xlock);
	return dpy;
}

/* ---- handlers ------------------------------------------------------------- */

static void h_get_display(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t kind = gb_r_u32(r), slen = gb_r_u32(r);
	const char *s = NULL;

	if (r->err)
		goto bad;
	if (slen != GB_NULL_LEN) {
		if (slen == 0 || slen > 4096)
			goto bad;
		s = gb_r_blob(r, slen);
		if (!s || s[slen - 1] != 0)
			goto bad;
	}

	GB_NATIVE(eglGetDisplay, EGLDisplay, (EGLNativeDisplayType));
	GB_NATIVE(eglGetPlatformDisplayEXT, EGLDisplay,
		  (EGLenum, void *, const EGLint *));
	EGLDisplay d = EGL_NO_DISPLAY;

	if (kind == 1 && s) {
		void *x = x11_open(s);
		/* Ask for the X11 platform explicitly. The legacy
		 * eglGetDisplay() *guesses* the platform from the pointer and
		 * from the inherited EGL_PLATFORM environment variable (a
		 * Wayland session exports EGL_PLATFORM=wayland), which would
		 * hand an Xlib Display* to the Wayland backend. */
		if (x && fn_eglGetPlatformDisplayEXT)
			d = fn_eglGetPlatformDisplayEXT(0x31D5 /* X11_EXT */, x,
							NULL);
		if (x && d == EGL_NO_DISPLAY && fn_eglGetDisplay)
			d = fn_eglGetDisplay((EGLNativeDisplayType)x);
	} else if (kind == 0 || kind == 2) {
		const char *plat = getenv("GB_EGL_PLATFORM");
		if ((kind == 2 || (plat && !strcmp(plat, "surfaceless"))) &&
		    fn_eglGetPlatformDisplayEXT)
			d = fn_eglGetPlatformDisplayEXT(0x31DD /* SURFACELESS_MESA */,
							EGL_DEFAULT_DISPLAY, NULL);
		else if (fn_eglGetDisplay)
			d = fn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}
	reply_u64(t, h, gb_p2h(HK_DISPLAY, d));
	return;
bad:
	gb_sv_proto_error(t, h);
}

static void h_query_string(struct gb_sth *t, const struct gb_cmd_hdr *h,
			   struct gb_r *r)
{
	uint32_t dpy = gb_r_u32(r), name = gb_r_u32(r);

	if (r->err) {
		gb_sv_proto_error(t, h);
		return;
	}
	GB_NATIVE(eglQueryString, const char *, (EGLDisplay, EGLint));
	const char *s = NULL;
	if (fn_eglQueryString)
		s = fn_eglQueryString((EGLDisplay)gb_h2p(HK_DISPLAY, dpy),
				      (EGLint)name);
	reply_string(t, h, s);
}

static void h_get_string(struct gb_sth *t, const struct gb_cmd_hdr *h,
			 struct gb_r *r)
{
	uint32_t name = gb_r_u32(r), index = gb_r_u32(r);

	if (r->err) {
		gb_sv_proto_error(t, h);
		return;
	}
	GB_NATIVE(glGetString, const GLubyte *, (GLenum));
	GB_NATIVE(glGetStringi, const GLubyte *, (GLenum, GLuint));
	const char *s = NULL;
	if (index == 0xFFFFFFFFu) {
		if (fn_glGetString)
			s = (const char *)fn_glGetString(name);
	} else if (fn_glGetStringi) {
		s = (const char *)fn_glGetStringi(name, index);
	}
	reply_string(t, h, s);
}

static void h_shader_source(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r)
{
	uint32_t shader = gb_r_u32(r), count = gb_r_u32(r), blen = gb_r_u32(r);

	if (r->err || count > 4096 || blen > GB_MAX_ARRAY_BYTES ||
	    blen < 4 * count)
		goto bad;
	const uint8_t *blob = gb_r_blob(r, blen);
	if (!blob)
		goto bad;

	const char **strs = gb_tmp(t, (size_t)count * sizeof(char *) + 8);
	GLint *lens = gb_tmp(t, (size_t)count * sizeof(GLint) + 8);
	uint64_t off = 4ull * count;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t l;
		memcpy(&l, blob + 4 * i, 4);
		if (l > blen - off)
			goto bad;
		strs[i] = (const char *)blob + off;
		lens[i] = (GLint)l;
		off += l;
	}
	if (off != blen)
		goto bad;

	GB_NATIVE(glShaderSource, void,
		  (GLuint, GLsizei, const GLchar *const *, const GLint *));
	if (fn_glShaderSource)
		fn_glShaderSource(shader, (GLsizei)count, strs, lens);
	return;
bad:
	gb_sv_proto_error(t, h);
}

static void h_vertex_attrib_ptr_off(struct gb_sth *t,
				    const struct gb_cmd_hdr *h, struct gb_r *r)
{
	uint32_t idx = gb_r_u32(r), size = gb_r_u32(r), type = gb_r_u32(r),
		 norm = gb_r_u32(r);
	int32_t stride = (int32_t)gb_r_u32(r);
	uint64_t off = gb_r_u64(r);

	if (r->err) {
		gb_sv_proto_error(t, h);
		return;
	}
	GB_NATIVE(glVertexAttribPointer, void,
		  (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
	if (fn_glVertexAttribPointer)
		fn_glVertexAttribPointer(idx, (GLint)size, type,
					 (GLboolean)norm, stride,
					 (const void *)(uintptr_t)off);
}

static void h_draw_elements_off(struct gb_sth *t, const struct gb_cmd_hdr *h,
				struct gb_r *r)
{
	uint32_t mode = gb_r_u32(r);
	int32_t count = (int32_t)gb_r_u32(r);
	uint32_t type = gb_r_u32(r);
	uint64_t off = gb_r_u64(r);

	if (r->err) {
		gb_sv_proto_error(t, h);
		return;
	}
	GB_NATIVE(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *));
	if (fn_glDrawElements)
		fn_glDrawElements(mode, count, type,
				  (const void *)(uintptr_t)off);
}

static int index_type_bytes(uint32_t type)
{
	return type == 0x1401 ? 1 : type == 0x1403 ? 2 : type == 0x1405 ? 4 : 0;
}

struct cattr {
	uint32_t index, size, type, norm;
	int32_t stride;
	uint32_t data_len;
};

static void h_draw_client(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t mode = gb_r_u32(r), kind = gb_r_u32(r);
	int32_t count = (int32_t)gb_r_u32(r);
	uint32_t itype = gb_r_u32(r), nverts = gb_r_u32(r),
		 ibytes = gb_r_u32(r), nattr = gb_r_u32(r);

	if (r->err || kind > 1 || count < 0 || nattr > MAX_ATTRIBS ||
	    (nattr == 0 && kind != 1) || nverts > (1u << 26) ||
	    ibytes > GB_MAX_ARRAY_BYTES)
		goto bad;

	struct cattr a[MAX_ATTRIBS];
	for (uint32_t i = 0; i < nattr; i++) {
		a[i].index = gb_r_u32(r);
		a[i].size = gb_r_u32(r);
		a[i].type = gb_r_u32(r);
		a[i].norm = gb_r_u32(r);
		a[i].stride = (int32_t)gb_r_u32(r);
		a[i].data_len = gb_r_u32(r);
	}
	if (r->err)
		goto bad;

	const uint8_t *idx = NULL;
	if (kind == 1) {
		int ib = index_type_bytes(itype);
		if (!ib || (uint64_t)count * ib != ibytes)
			goto bad;
		idx = ibytes ? gb_r_blob(r, ibytes) : NULL;
		if (ibytes && !idx)
			goto bad;
		/* Every index must address an uploaded vertex. */
		for (int32_t i = 0; i < count; i++) {
			uint32_t v = ib == 1 ? idx[i]
				   : ib == 2 ? ((const uint16_t *)idx)[i]
					     : ((const uint32_t *)idx)[i];
			if (nattr && v >= nverts)
				goto bad;
		}
	} else if (ibytes || (uint32_t)count != nverts) {
		goto bad;
	}

	const uint8_t *data[MAX_ATTRIBS];
	for (uint32_t i = 0; i < nattr; i++) {
		int eb = gb_attrib_elem_bytes(a[i].type, (int)a[i].size);
		int64_t stride = a[i].stride ? a[i].stride : eb;
		if (a[i].index >= MAX_ATTRIBS || eb <= 0 || a[i].size < 1 ||
		    a[i].size > 4 || a[i].stride < 0 || nverts == 0)
			goto bad;
		uint64_t need = (uint64_t)(nverts - 1) * stride + eb;
		if (need != a[i].data_len)
			goto bad;
		data[i] = gb_r_blob(r, a[i].data_len);
		if (!data[i])
			goto bad;
	}

	GB_NATIVE(glGenBuffers, void, (GLsizei, GLuint *));
	GB_NATIVE(glBindBuffer, void, (GLenum, GLuint));
	GB_NATIVE(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum));
	GB_NATIVE(glGetIntegerv, void, (GLenum, GLint *));
	GB_NATIVE(glVertexAttribPointer, void,
		  (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
	GB_NATIVE(glDrawArrays, void, (GLenum, GLint, GLsizei));
	GB_NATIVE(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *));
	if (!fn_glGenBuffers || !fn_glBindBuffer || !fn_glBufferData ||
	    !fn_glGetIntegerv || !fn_glVertexAttribPointer ||
	    !fn_glDrawArrays || !fn_glDrawElements)
		return;

	struct gb_scratch *s = scratch_for(t);
	if (!s)
		return; /* no current context: native would error anyway */

	GLint old_abo = 0, old_ebo = 0;
	fn_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_abo);
	if (kind == 1)
		fn_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &old_ebo);

	for (uint32_t i = 0; i < nattr; i++) {
		GLuint *vbo = &s->vbo[a[i].index];
		if (!*vbo)
			fn_glGenBuffers(1, vbo);
		fn_glBindBuffer(GL_ARRAY_BUFFER, *vbo);
		fn_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)a[i].data_len,
				data[i], GL_STREAM_DRAW);
		fn_glVertexAttribPointer(a[i].index, (GLint)a[i].size,
					 a[i].type, (GLboolean)a[i].norm,
					 a[i].stride, NULL);
	}
	if (kind == 0) {
		fn_glDrawArrays(mode, 0, count);
	} else {
		if (!s->ebo)
			fn_glGenBuffers(1, &s->ebo);
		fn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ebo);
		fn_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ibytes,
				idx, GL_STREAM_DRAW);
		fn_glDrawElements(mode, count, itype, NULL);
		fn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)old_ebo);
	}
	fn_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_abo);
	return;
bad:
	gb_sv_proto_error(t, h);
}

/* eglCreateWindowSurface: the window is an X11 XID created by the client's
 * own connection. XIDs are global to the X server, so the server's separate
 * connection can render to it (ARCHITECTURE.md Strategy X). */
static void h_create_window_surface(struct gb_sth *t,
				    const struct gb_cmd_hdr *h, struct gb_r *r)
{
	uint32_t dpy = gb_r_u32(r), cfg = gb_r_u32(r), xid = gb_r_u32(r),
		 alen = gb_r_u32(r);
	const int32_t *attr = NULL;

	if (r->err)
		goto bad;
	if (alen != GB_NULL_LEN) {
		if (alen == 0 || alen % 4 || alen > 4096)
			goto bad;
		attr = gb_r_blob(r, alen);
		if (!attr || !gb_attrib_valid_EGLint(attr, alen / 4))
			goto bad;
	}
	GB_NATIVE(eglCreateWindowSurface, EGLSurface,
		  (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *));
	EGLSurface s = EGL_NO_SURFACE;
	if (fn_eglCreateWindowSurface)
		s = fn_eglCreateWindowSurface(
			(EGLDisplay)gb_h2p(HK_DISPLAY, dpy),
			(EGLConfig)gb_h2p(HK_CONFIG, cfg),
			(EGLNativeWindowType)(uintptr_t)xid, attr);
	reply_u64(t, h, gb_p2h(HK_SURFACE, s));
	return;
bad:
	gb_sv_proto_error(t, h);
}

void gb_srv_manual_dispatch(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r)
{
	switch (h->opcode) {
	case GB_OPX_GetDisplay: h_get_display(t, h, r); break;
	case GB_OPX_QueryString: h_query_string(t, h, r); break;
	case GB_OPX_GetString: h_get_string(t, h, r); break;
	case GB_OPX_ShaderSource: h_shader_source(t, h, r); break;
	case GB_OPX_VertexAttribPtrOff: h_vertex_attrib_ptr_off(t, h, r); break;
	case GB_OPX_DrawClient: h_draw_client(t, h, r); break;
	case GB_OPX_DrawElementsOff: h_draw_elements_off(t, h, r); break;
	case GB_OPX_CreateWindowSurface: h_create_window_surface(t, h, r); break;
	default: gb_sv_proto_error(t, h); break;
	}
}

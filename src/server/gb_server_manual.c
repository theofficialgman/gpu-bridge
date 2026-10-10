/*
 * gb_server_manual.c - hand-written server handlers for operations that the
 * registry-driven generator cannot express safely: native display/window
 * arguments, string returns, glShaderSource, GLX queries, and client-memory
 * vertex arrays.
 *
 * Wire layouts (all fields after the 16-byte gb_cmd_hdr; u64 are 8-aligned,
 * blobs 8-aligned) must match the client (gb_client_gl.c / gb_client_glx.c /
 * gb_client_arrays.c).
 */
#include <stddef.h>
#include <stdio.h>

#include "gb_server_impl.h"

/* ---- per-native-context scratch buffers for client arrays ----------------- */

struct gb_scratch {
	struct gb_scratch *next;
	void *ctx; /* EGLContext or GLXContext (whichever is current) */
	GLuint vbo[CA_COUNT];
	GLuint ebo;
};

static void *current_ctx_key(void)
{
	GB_NATIVE(eglGetCurrentContext, EGLContext, (void));
	GB_NATIVE(glXGetCurrentContext, GLXContext, (void));
	void *k = NULL;

	if (fn_eglGetCurrentContext)
		k = (void *)fn_eglGetCurrentContext();
	if (!k && fn_glXGetCurrentContext)
		k = (void *)fn_glXGetCurrentContext();
	return k;
}

static struct gb_scratch *scratch_for(struct gb_sth *t)
{
	void *cur = current_ctx_key();

	if (!cur)
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

/* Read a NUL-terminated string field (length incl. NUL, or NULL_LEN). */
static int read_str(struct gb_r *r, uint32_t slen, const char **out)
{
	*out = NULL;
	if (slen == GB_NULL_LEN)
		return 0;
	if (slen == 0 || slen > 4096)
		return -1;
	const char *s = gb_r_blob(r, slen);
	if (!s || s[slen - 1] != 0)
		return -1;
	*out = s;
	return 0;
}

/* ---- EGL ------------------------------------------------------------------- */

static void h_get_display(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t kind = gb_r_u32(r), slen = gb_r_u32(r);
	const char *s;

	if (r->err || read_str(r, slen, &s))
		goto bad;

	GB_NATIVE(eglGetDisplay, EGLDisplay, (EGLNativeDisplayType));
	GB_NATIVE(eglGetPlatformDisplayEXT, EGLDisplay,
		  (EGLenum, void *, const EGLint *));
	EGLDisplay d = EGL_NO_DISPLAY;

	if (kind == 1 && s) {
		void *x = gb_x_open(s);
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
	GB_NATIVE(glGetIntegerv, void, (GLenum, GLint *));
	const char *s = NULL;
	if (index == 0xFFFFFFFFu) {
		if (fn_glGetString)
			s = (const char *)fn_glGetString(name);
		/*
		 * Core-profile contexts have no legacy GL_EXTENSIONS string
		 * (glGetString returns NULL and raises INVALID_ENUM): rebuild
		 * the list from glGetStringi so the client sees one list in
		 * every profile. Drain the error we may have just provoked.
		 */
		if (!s && name == GL_EXTENSIONS && fn_glGetStringi &&
		    fn_glGetIntegerv) {
			GB_NATIVE(glGetError, GLenum, (void));
			GLint n = 0;
			if (fn_glGetError)
				(void)fn_glGetError();
			fn_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
			size_t cap = 16;
			for (GLint i = 0; i < n && i < 4096; i++) {
				const char *e = (const char *)
					fn_glGetStringi(GL_EXTENSIONS, i);
				if (e)
					cap += strlen(e) + 1;
			}
			char *buf = gb_tmp(t, cap);
			size_t o = 0;
			for (GLint i = 0; buf && i < n && i < 4096; i++) {
				const char *e = (const char *)
					fn_glGetStringi(GL_EXTENSIONS, i);
				if (!e)
					continue;
				size_t l = strlen(e);
				if (o)
					buf[o++] = ' ';
				memcpy(buf + o, e, l);
				o += l;
			}
			if (buf) {
				buf[o] = 0;
				s = buf;
			}
		}
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

/* ---- GLX -------------------------------------------------------------------- */

/* OpenXDisplay: connection string -> id of the server's own Display. */
static void h_open_xdisplay(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r)
{
	uint32_t slen = gb_r_u32(r);
	const char *s;

	if (r->err || read_str(r, slen, &s) || !s) {
		gb_sv_proto_error(t, h);
		return;
	}
	reply_u64(t, h, gb_p2h(HK_XDPY, gb_x_open(s)));
}

/* GLXString: kind 0 client string, 1 server string, 2 extensions string. */
static void h_glx_string(struct gb_sth *t, const struct gb_cmd_hdr *h,
			 struct gb_r *r)
{
	uint32_t kind = gb_r_u32(r), dpy = gb_r_u32(r), screen = gb_r_u32(r),
		 name = gb_r_u32(r);
	Display *d = (Display *)gb_h2p(HK_XDPY, dpy);

	if (r->err || !d) {
		gb_sv_proto_error(t, h);
		return;
	}
	GB_NATIVE(glXGetClientString, const char *, (Display *, int));
	GB_NATIVE(glXQueryServerString, const char *, (Display *, int, int));
	GB_NATIVE(glXQueryExtensionsString, const char *, (Display *, int));
	const char *s = NULL;
	if (kind == 0 && fn_glXGetClientString)
		s = fn_glXGetClientString(d, (int)name);
	else if (kind == 1 && fn_glXQueryServerString)
		s = fn_glXQueryServerString(d, (int)screen, (int)name);
	else if (kind == 2 && fn_glXQueryExtensionsString)
		s = fn_glXQueryExtensionsString(d, (int)screen);
	reply_string(t, h, s);
}

/* GLXFBConfigs: kind 0 = glXGetFBConfigs, 1 = glXChooseFBConfig.
 * Reply: u32 n, then n x u32 handle ids. */
static void h_glx_fbconfigs(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r)
{
	uint32_t kind = gb_r_u32(r), dpy = gb_r_u32(r), screen = gb_r_u32(r),
		 alen = gb_r_u32(r);
	const int *attr = NULL;
	Display *d = (Display *)gb_h2p(HK_XDPY, dpy);

	if (r->err || !d)
		goto bad;
	if (alen != GB_NULL_LEN) {
		if (alen == 0 || alen % 4 || alen > 65536)
			goto bad;
		attr = gb_r_blob(r, alen);
		if (!attr || !gb_glx_attrib_valid(attr, alen / 4, 0))
			goto bad;
	}
	GB_NATIVE(glXGetFBConfigs, GLXFBConfig *, (Display *, int, int *));
	GB_NATIVE(glXChooseFBConfig, GLXFBConfig *,
		  (Display *, int, const int *, int *));
	int n = 0;
	GLXFBConfig *cfgs = NULL;
	if (kind == 0 && fn_glXGetFBConfigs)
		cfgs = fn_glXGetFBConfigs(d, (int)screen, &n);
	else if (kind == 1 && fn_glXChooseFBConfig)
		cfgs = fn_glXChooseFBConfig(d, (int)screen, attr, &n);
	if (!cfgs || n < 0)
		n = 0;
	if ((uint32_t)n > 65536)
		n = 65536;

	if (h->flags & GB_CMD_F_WANT_REPLY) {
		struct gb_out o;
		struct gb_w w;
		uint32_t total = GB_ALIGN_UP(20 + 4u * (uint32_t)n, 8u);
		if (!gb_sv_reply_begin(t, h, &o, total)) {
			gb_w_init(&w, o.p);
			gb_w_u32(&w, (uint32_t)n);
			for (int i = 0; i < n; i++)
				gb_w_u32(&w, gb_p2h(HK_GLXFBC, cfgs[i]));
			gb_sv_reply_end(t, &o);
		}
	}
	gb_x_free(cfgs);
	return;
bad:
	gb_sv_proto_error(t, h);
}

/* ---- client-memory vertex arrays ------------------------------------------------ */

static int index_type_bytes(uint32_t type)
{
	return type == 0x1401 ? 1 : type == 0x1403 ? 2 : type == 0x1405 ? 4 : 0;
}

struct cattr {
	uint32_t slot, size, type, stride, flags, divisor, len;
};

/* Point native array state for `a` at offset 0 of the currently bound
 * ARRAY_BUFFER. */
static void set_pointer(const struct cattr *a)
{
	GB_NATIVE(glVertexPointer, void, (GLint, GLenum, GLsizei, const void *));
	GB_NATIVE(glNormalPointer, void, (GLenum, GLsizei, const void *));
	GB_NATIVE(glColorPointer, void, (GLint, GLenum, GLsizei, const void *));
	GB_NATIVE(glSecondaryColorPointer, void,
		  (GLint, GLenum, GLsizei, const void *));
	GB_NATIVE(glFogCoordPointer, void, (GLenum, GLsizei, const void *));
	GB_NATIVE(glEdgeFlagPointer, void, (GLsizei, const void *));
	GB_NATIVE(glIndexPointer, void, (GLenum, GLsizei, const void *));
	GB_NATIVE(glTexCoordPointer, void,
		  (GLint, GLenum, GLsizei, const void *));
	GB_NATIVE(glClientActiveTexture, void, (GLenum));
	GB_NATIVE(glGetIntegerv, void, (GLenum, GLint *));
	GB_NATIVE(glVertexAttribPointer, void,
		  (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
	GB_NATIVE(glVertexAttribIPointer, void,
		  (GLuint, GLint, GLenum, GLsizei, const void *));
	GB_NATIVE(glVertexAttribLPointer, void,
		  (GLuint, GLint, GLenum, GLsizei, const void *));
	GLint sz = (GLint)a->size;
	GLsizei st = (GLsizei)a->stride;

	switch (a->slot) {
	case CA_VERTEX:
		if (fn_glVertexPointer) fn_glVertexPointer(sz, a->type, st, NULL);
		break;
	case CA_NORMAL:
		if (fn_glNormalPointer) fn_glNormalPointer(a->type, st, NULL);
		break;
	case CA_COLOR:
		if (fn_glColorPointer) fn_glColorPointer(sz, a->type, st, NULL);
		break;
	case CA_SECONDARY:
		if (fn_glSecondaryColorPointer)
			fn_glSecondaryColorPointer(sz, a->type, st, NULL);
		break;
	case CA_FOGCOORD:
		if (fn_glFogCoordPointer) fn_glFogCoordPointer(a->type, st, NULL);
		break;
	case CA_EDGEFLAG:
		if (fn_glEdgeFlagPointer) fn_glEdgeFlagPointer(st, NULL);
		break;
	case CA_INDEX:
		if (fn_glIndexPointer) fn_glIndexPointer(a->type, st, NULL);
		break;
	default:
		if (a->slot >= CA_TEX0 && a->slot < CA_GENERIC0) {
			GLint old = 0;
			if (fn_glClientActiveTexture && fn_glGetIntegerv &&
			    fn_glTexCoordPointer) {
				fn_glGetIntegerv(0x84E1 /* CLIENT_ACTIVE_TEXTURE */,
						 &old);
				fn_glClientActiveTexture(0x84C0 +
							 (a->slot - CA_TEX0));
				fn_glTexCoordPointer(sz, a->type, st, NULL);
				fn_glClientActiveTexture((GLenum)old);
			}
		} else if (a->slot >= CA_GENERIC0 && a->slot < CA_COUNT) {
			GLuint idx = a->slot - CA_GENERIC0;
			if ((a->flags & GB_CA_LONG) && fn_glVertexAttribLPointer)
				fn_glVertexAttribLPointer(idx, sz, a->type, st, NULL);
			else if ((a->flags & GB_CA_INTEGER) &&
				 fn_glVertexAttribIPointer)
				fn_glVertexAttribIPointer(idx, sz, a->type, st, NULL);
			else if (fn_glVertexAttribPointer)
				fn_glVertexAttribPointer(
					idx, sz, a->type,
					(GLboolean)!!(a->flags & GB_CA_NORMALIZED),
					st, NULL);
		}
		break;
	}
}

/*
 * DrawClient wire layout (after the 16-byte header):
 *   u32 mode, kind (0 arrays, 1 elements), i32 count, u32 index type,
 *   u32 nverts (vertices of each non-divisor array uploaded from vertex 0),
 *   u32 ibytes, u32 nattr, u32 instances (0 = not instanced),
 *   i32 basevertex, u32 baseinstance,
 *   per attribute (7 x u32): slot, size, type, stride, flags, divisor, len
 *   blobs: indices (if ibytes), then each attribute's data in order.
 */
static void h_draw_client(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t mode = gb_r_u32(r), kind = gb_r_u32(r);
	int32_t count = (int32_t)gb_r_u32(r);
	uint32_t itype = gb_r_u32(r), nverts = gb_r_u32(r),
		 ibytes = gb_r_u32(r), nattr = gb_r_u32(r),
		 instances = gb_r_u32(r);
	int32_t basevertex = (int32_t)gb_r_u32(r);
	uint32_t baseinstance = gb_r_u32(r);

	if (r->err || kind > 1 || count < 0 || nattr > CA_COUNT ||
	    (nattr == 0 && kind != 1) || nverts > (1u << 26) ||
	    ibytes > GB_MAX_ARRAY_BYTES || instances > (1u << 24))
		goto bad;

	struct cattr a[CA_COUNT];
	for (uint32_t i = 0; i < nattr; i++) {
		a[i].slot = gb_r_u32(r);
		a[i].size = gb_r_u32(r);
		a[i].type = gb_r_u32(r);
		a[i].stride = gb_r_u32(r);
		a[i].flags = gb_r_u32(r);
		a[i].divisor = gb_r_u32(r);
		a[i].len = gb_r_u32(r);
	}
	if (r->err)
		goto bad;

	int have_plain = 0;
	for (uint32_t i = 0; i < nattr; i++)
		if (a[i].divisor == 0)
			have_plain = 1;

	const uint8_t *idx = NULL;
	if (kind == 1) {
		int ib = index_type_bytes(itype);
		if (!ib || (uint64_t)count * ib != ibytes)
			goto bad;
		idx = ibytes ? gb_r_blob(r, ibytes) : NULL;
		if (ibytes && !idx)
			goto bad;
		/* Every referenced vertex must lie inside what was uploaded. */
		for (int32_t i = 0; have_plain && i < count; i++) {
			int64_t v = ib == 1 ? idx[i]
				  : ib == 2 ? ((const uint16_t *)idx)[i]
					    : ((const uint32_t *)idx)[i];
			v += basevertex;
			if (v < 0 || v >= (int64_t)nverts)
				goto bad;
		}
	} else if (ibytes || (uint32_t)count != nverts) {
		goto bad;
	}

	const uint8_t *data[CA_COUNT];
	for (uint32_t i = 0; i < nattr; i++) {
		int eb = gb_attrib_elem_bytes(a[i].type, (int)a[i].size);
		int64_t stride = a[i].stride ? (int64_t)a[i].stride : eb;
		if (a[i].slot >= CA_COUNT || eb <= 0 || a[i].size < 1 ||
		    a[i].size > 4 || stride < 0 || stride > (1 << 16))
			goto bad;
		uint64_t elems;
		if (a[i].divisor == 0) {
			elems = nverts;
		} else {
			if (instances == 0 || a[i].slot < CA_GENERIC0)
				goto bad;
			elems = (uint64_t)(instances - 1) / a[i].divisor +
				baseinstance + 1;
		}
		if (elems == 0 || elems > (1u << 26))
			goto bad;
		uint64_t need = (elems - 1) * (uint64_t)stride + eb;
		if (need != a[i].len)
			goto bad;
		data[i] = gb_r_blob(r, a[i].len);
		if (!data[i])
			goto bad;
	}

	GB_NATIVE(glGenBuffers, void, (GLsizei, GLuint *));
	GB_NATIVE(glBindBuffer, void, (GLenum, GLuint));
	GB_NATIVE(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum));
	GB_NATIVE(glGetIntegerv, void, (GLenum, GLint *));
	GB_NATIVE(glDrawArrays, void, (GLenum, GLint, GLsizei));
	GB_NATIVE(glDrawArraysInstanced, void, (GLenum, GLint, GLsizei, GLsizei));
	GB_NATIVE(glDrawArraysInstancedBaseInstance, void,
		  (GLenum, GLint, GLsizei, GLsizei, GLuint));
	GB_NATIVE(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *));
	GB_NATIVE(glDrawElementsBaseVertex, void,
		  (GLenum, GLsizei, GLenum, const void *, GLint));
	GB_NATIVE(glDrawElementsInstanced, void,
		  (GLenum, GLsizei, GLenum, const void *, GLsizei));
	GB_NATIVE(glDrawElementsInstancedBaseVertex, void,
		  (GLenum, GLsizei, GLenum, const void *, GLsizei, GLint));
	GB_NATIVE(glDrawElementsInstancedBaseVertexBaseInstance, void,
		  (GLenum, GLsizei, GLenum, const void *, GLsizei, GLint,
		   GLuint));
	if (!fn_glGenBuffers || !fn_glBindBuffer || !fn_glBufferData ||
	    !fn_glGetIntegerv)
		return;

	struct gb_scratch *s = scratch_for(t);
	if (!s)
		return; /* no current context: native would error anyway */

	GLint old_abo = 0, old_ebo = 0;
	fn_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_abo);
	if (kind == 1)
		fn_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &old_ebo);

	for (uint32_t i = 0; i < nattr; i++) {
		GLuint *vbo = &s->vbo[a[i].slot];
		if (!*vbo)
			fn_glGenBuffers(1, vbo);
		fn_glBindBuffer(GL_ARRAY_BUFFER, *vbo);
		fn_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)a[i].len, data[i],
				GL_STREAM_DRAW);
		set_pointer(&a[i]);
	}

	if (kind == 0) {
		if (!instances) {
			if (fn_glDrawArrays)
				fn_glDrawArrays(mode, 0, count);
		} else if (baseinstance && fn_glDrawArraysInstancedBaseInstance) {
			fn_glDrawArraysInstancedBaseInstance(mode, 0, count,
							     (GLsizei)instances,
							     baseinstance);
		} else if (fn_glDrawArraysInstanced) {
			fn_glDrawArraysInstanced(mode, 0, count,
						 (GLsizei)instances);
		}
	} else {
		if (!s->ebo)
			fn_glGenBuffers(1, &s->ebo);
		fn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ebo);
		fn_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ibytes,
				idx, GL_STREAM_DRAW);
		if (instances || baseinstance) {
			GLsizei n = instances ? (GLsizei)instances : 1;
			if (fn_glDrawElementsInstancedBaseVertexBaseInstance)
				fn_glDrawElementsInstancedBaseVertexBaseInstance(
					mode, count, itype, NULL, n, basevertex,
					baseinstance);
			else if (basevertex && fn_glDrawElementsInstancedBaseVertex)
				fn_glDrawElementsInstancedBaseVertex(
					mode, count, itype, NULL, n, basevertex);
			else if (fn_glDrawElementsInstanced)
				fn_glDrawElementsInstanced(mode, count, itype,
							   NULL, n);
		} else if (basevertex && fn_glDrawElementsBaseVertex) {
			fn_glDrawElementsBaseVertex(mode, count, itype, NULL,
						    basevertex);
		} else if (fn_glDrawElements) {
			fn_glDrawElements(mode, count, itype, NULL);
		}
		fn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)old_ebo);
	}
	fn_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_abo);
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
	case GB_OPX_DrawClient: h_draw_client(t, h, r); break;
	case GB_OPX_CreateWindowSurface: h_create_window_surface(t, h, r); break;
	case GB_OPX_OpenXDisplay: h_open_xdisplay(t, h, r); break;
	case GB_OPX_GLXString: h_glx_string(t, h, r); break;
	case GB_OPX_GLXFBConfigs: h_glx_fbconfigs(t, h, r); break;
	default: gb_sv_proto_error(t, h); break;
	}
}

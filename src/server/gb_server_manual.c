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

/*
 * Client-memory array uploads go through one persistent scratch VBO and one
 * scratch EBO per native context, written with glBufferSubData at a running
 * offset (the buffer is orphaned when it wraps). The earlier version
 * allocated a fresh store per attribute per draw with glBufferData.
 */
#define SCRATCH_VBO_BYTES (4u << 20)
#define SCRATCH_EBO_BYTES (1u << 20)

struct gb_scratch {
	struct gb_scratch *next;
	void *ctx; /* EGLContext or GLXContext (whichever is current) */
	GLuint vbo, ebo;
	size_t vbo_cap, vbo_off, ebo_cap, ebo_off;
	/* selection / feedback buffers the driver retains and writes through
	 * (glSelectBuffer / glFeedbackBuffer); the previous generation is kept
	 * one call longer in case the replacing call failed */
	uint32_t *sel, *sel_prev;
	uint32_t sel_n;
	float *fb, *fb_prev;
	uint32_t fb_n;
};

static void *current_ctx_key(struct gb_sth *t)
{
	if (t->ctx_valid)
		return t->ctx_key;

	GB_NATIVE(eglGetCurrentContext, EGLContext, (void));
	GB_NATIVE(glXGetCurrentContext, GLXContext, (void));
	void *k = NULL;

	if (fn_eglGetCurrentContext)
		k = (void *)fn_eglGetCurrentContext();
	if (!k && fn_glXGetCurrentContext)
		k = (void *)fn_glXGetCurrentContext();
	t->ctx_key = k;
	t->ctx_valid = 1;
	return k;
}

static struct gb_scratch *scratch_for(struct gb_sth *t)
{
	void *cur = current_ctx_key(t);

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
		free(s->sel);
		free(s->sel_prev);
		free(s->fb);
		free(s->fb_prev);
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
	uint32_t kind = gb_r_u32(r), inst = gb_r_u32(r), slen = gb_r_u32(r);
	const char *s;

	if (r->err || read_str(r, slen, &s))
		goto bad;

	GB_NATIVE(eglGetDisplay, EGLDisplay, (EGLNativeDisplayType));
	GB_NATIVE(eglGetPlatformDisplayEXT, EGLDisplay,
		  (EGLenum, void *, const EGLint *));
	EGLDisplay d = EGL_NO_DISPLAY;

	if (kind == 1 && s) {
		void *x = gb_x_open_inst(s, inst);
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
	} else if (kind == 3 && fn_eglGetPlatformDisplayEXT) {
		d = fn_eglGetPlatformDisplayEXT(0x313F /* PLATFORM_DEVICE_EXT */,
						gb_h2p(HK_DEVICE, inst), NULL);
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
		/*
		 * GL_EXTENSIONS: desktop 3.0+ contexts enumerate with
		 * glGetStringi (the legacy string does not exist in core
		 * profiles and querying it would raise INVALID_ENUM, which
		 * the app would later see). Decide from the version string
		 * rather than by trying.
		 */
		const char *ver = NULL;
		if (name == GL_EXTENSIONS && fn_glGetString)
			ver = (const char *)fn_glGetString(GL_VERSION);
		int desktop3 = ver && strncmp(ver, "OpenGL ES", 9) &&
			       atoi(ver) >= 3;
		if (desktop3 && fn_glGetStringi && fn_glGetIntegerv) {
			GLint n = 0;
			fn_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
			if (n > 4096)
				n = 4096;
			size_t cap = 16;
			for (GLint i = 0; i < n; i++) {
				const char *e = (const char *)
					fn_glGetStringi(GL_EXTENSIONS, i);
				if (e)
					cap += strlen(e) + 1;
			}
			char *buf = gb_tmp(t, cap);
			size_t o = 0;
			for (GLint i = 0; buf && i < n; i++) {
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
		} else if (fn_glGetString) {
			s = (const char *)fn_glGetString(name);
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

/*
 * BufferRead: contents of a buffer range, for client-side mapping shadows.
 *   u32 mode (0 = by target binding, 1 = by buffer name), u32 target|name,
 *   u64 offset, u64 length  ->  u32 length + blob
 * Desktop GL: glGetBufferSubData / glGetNamedBufferSubData. ES has neither,
 * so a GL ES context reads through a transient READ mapping.
 */
static void h_buffer_read(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t mode = gb_r_u32(r), id = gb_r_u32(r);
	uint64_t off = gb_r_u64(r), len = gb_r_u64(r);

	if (r->err || len == 0 || len > GB_MAX_ARRAY_BYTES ||
	    off > (1ull << 40) || mode > 1)
		goto bad;

	GB_NATIVE(eglGetCurrentContext, EGLContext, (void));
	GB_NATIVE(glGetBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, void *));
	GB_NATIVE(glGetNamedBufferSubData, void,
		  (GLuint, GLintptr, GLsizeiptr, void *));
	GB_NATIVE(glMapBufferRange, void *,
		  (GLenum, GLintptr, GLsizeiptr, GLbitfield));
	GB_NATIVE(glUnmapBuffer, GLboolean, (GLenum));
	void *buf = gb_tmp_zero(t, (size_t)len + 8);
	if (!buf)
		goto bad;
	int es = fn_eglGetCurrentContext &&
		 fn_eglGetCurrentContext() != EGL_NO_CONTEXT;

	if (!es && mode == 0 && fn_glGetBufferSubData) {
		fn_glGetBufferSubData(id, (GLintptr)off, (GLsizeiptr)len, buf);
	} else if (!es && mode == 1 && fn_glGetNamedBufferSubData) {
		fn_glGetNamedBufferSubData(id, (GLintptr)off, (GLsizeiptr)len,
					   buf);
	} else if (mode == 0 && fn_glMapBufferRange && fn_glUnmapBuffer) {
		const void *p = fn_glMapBufferRange(id, (GLintptr)off,
						    (GLsizeiptr)len,
						    0x0001 /* MAP_READ_BIT */);
		if (p) {
			memcpy(buf, p, (size_t)len);
			fn_glUnmapBuffer(id);
		}
	}

	if (h->flags & GB_CMD_F_WANT_REPLY) {
		struct gb_out o;
		struct gb_w w;
		uint32_t total = GB_ALIGN_UP(20, 8u) + (uint32_t)len;
		total = GB_ALIGN_UP(total, 8u);
		if (!gb_sv_reply_begin(t, h, &o, total)) {
			gb_w_init(&w, o.p);
			gb_w_u32(&w, (uint32_t)len);
			memcpy(gb_w_blob(&w, (uint32_t)len), buf, (size_t)len);
			gb_sv_reply_end(t, &o);
		}
	}
	return;
bad:
	gb_sv_proto_error(t, h);
}

/* ---- selection / feedback buffers ------------------------------------------------- */

#define GB_MAX_SELECT_WORDS (1u << 22)

static void h_select_buffer(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r)
{
	uint32_t kind = gb_r_u32(r), size = gb_r_u32(r), type = gb_r_u32(r);

	if (r->err || kind > 1)
		goto bad;

	GB_NATIVE(glSelectBuffer, void, (GLsizei, GLuint *));
	GB_NATIVE(glFeedbackBuffer, void, (GLsizei, GLenum, GLfloat *));
	struct gb_scratch *s = scratch_for(t);

	if (!s)
		return; /* no context: nothing to attach to */
	if ((int32_t)size < 0 || size > GB_MAX_SELECT_WORDS) {
		gb_sv_raise_invalid(); /* INVALID_VALUE, as for a bad size */
		return;
	}
	uint32_t *nb = calloc(size ? size : 1, 4);

	if (!nb)
		return;
	if (kind == 0) {
		if (fn_glSelectBuffer)
			fn_glSelectBuffer((GLsizei)size, nb);
		free(s->sel_prev);
		s->sel_prev = s->sel;
		s->sel = nb;
		s->sel_n = size;
	} else {
		if (fn_glFeedbackBuffer)
			fn_glFeedbackBuffer((GLsizei)size, type, (float *)nb);
		free(s->fb_prev);
		s->fb_prev = s->fb;
		s->fb = (float *)nb;
		s->fb_n = size;
	}
	return;
bad:
	gb_sv_proto_error(t, h);
}

/* RenderMode: switch modes; if the mode being left was SELECT or FEEDBACK,
 * the results (written by the driver into the server's buffer) come back in
 * the reply for the client to copy into the application's buffer. */
static void h_render_mode(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t mode = gb_r_u32(r), prev = gb_r_u32(r);

	if (r->err)
		goto bad;

	GB_NATIVE(glRenderMode, GLint, (GLenum));
	GB_NATIVE(glGetIntegerv, void, (GLenum, GLint *));
	struct gb_scratch *s = scratch_for(t);
	GLint ret = fn_glRenderMode ? fn_glRenderMode(mode) : 0;
	GLint cur = 0x1C00;
	uint32_t n = 0;
	const uint32_t *words = NULL;

	if (fn_glGetIntegerv)
		fn_glGetIntegerv(0x0C40 /* RENDER_MODE */, &cur);
	if (s && prev == 0x1C02 && s->sel) {
		words = s->sel;
		if (ret < 0) {
			n = s->sel_n; /* overflow: the whole buffer */
		} else {
			uint32_t pos = 0;
			for (GLint i = 0; i < ret && pos < s->sel_n; i++)
				pos += 3 + s->sel[pos]; /* name count, z1, z2 */
			n = pos > s->sel_n ? s->sel_n : pos;
		}
	} else if (s && prev == 0x1C01 && s->fb) {
		words = (const uint32_t *)s->fb;
		n = ret < 0 ? s->fb_n
			    : ((uint32_t)ret < s->fb_n ? (uint32_t)ret
						       : s->fb_n);
	}

	if (h->flags & GB_CMD_F_WANT_REPLY) {
		struct gb_out o;
		struct gb_w w;
		uint32_t total = GB_ALIGN_UP(16 + 12 + 4 * n, 8u);

		if (!gb_sv_reply_begin(t, h, &o, total)) {
			gb_w_init(&w, o.p);
			gb_w_u32(&w, (uint32_t)ret);
			gb_w_u32(&w, (uint32_t)cur);
			gb_w_u32(&w, n);
			if (n)
				memcpy(gb_w_blob(&w, 4 * n), words, 4 * n);
			gb_sv_reply_end(t, &o);
		}
	}
	return;
bad:
	gb_sv_proto_error(t, h);
}

/*
 * EglDevice: EGL_EXT_device_{base,enumeration,query}. Device pointers are
 * translated to bridge handles in both directions.
 *   sub 0 QueryDevices(max)           -> u32 ok, u32 num, u64 -, u32 h[n]
 *   sub 2 DeviceAttrib(device, attr)  -> u32 ok, u32 0, u64 value
 *   sub 3 DisplayAttrib(display, attr)-> u32 ok, u32 0, u64 value
 * (EGL_DEVICE_EXT, the display's device, comes back as a handle.)
 */
static void h_device_string(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r)
{
	uint32_t dev = gb_r_u32(r), name = gb_r_u32(r);

	if (r->err) {
		gb_sv_proto_error(t, h);
		return;
	}
	GB_NATIVE(eglQueryDeviceStringEXT, const char *, (EGLDeviceEXT, EGLint));
	const char *str = NULL;

	if (fn_eglQueryDeviceStringEXT)
		str = fn_eglQueryDeviceStringEXT(
			(EGLDeviceEXT)gb_h2p(HK_DEVICE, dev), (EGLint)name);
	reply_string(t, h, str);
}

#define EGL_DEVICE_EXT_ATTR 0x322C
#define GB_MAX_DEVICES 16

static void h_egl_device(struct gb_sth *t, const struct gb_cmd_hdr *h,
			 struct gb_r *r)
{
	uint32_t sub = gb_r_u32(r), a = gb_r_u32(r), b = gb_r_u32(r);

	if (r->err || sub > 3 || sub == 1)
		goto bad;

	GB_NATIVE(eglQueryDevicesEXT, EGLBoolean,
		  (EGLint, EGLDeviceEXT *, EGLint *));
	GB_NATIVE(eglQueryDeviceAttribEXT, EGLBoolean,
		  (EGLDeviceEXT, EGLint, EGLAttrib *));
	GB_NATIVE(eglQueryDisplayAttribEXT, EGLBoolean,
		  (EGLDisplay, EGLint, EGLAttrib *));

	uint32_t ok = 0, n = 0;
	uint64_t value = 0;
	uint32_t hs[GB_MAX_DEVICES];

	if (sub == 0) {
		EGLDeviceEXT devs[GB_MAX_DEVICES];
		EGLint num = 0, max = (EGLint)a > GB_MAX_DEVICES
					      ? GB_MAX_DEVICES : (EGLint)a;

		if (fn_eglQueryDevicesEXT &&
		    fn_eglQueryDevicesEXT(max, max ? devs : NULL, &num)) {
			ok = 1;
			n = (uint32_t)num;
			if (max > 0 && n > (uint32_t)max)
				n = (uint32_t)max;
			for (uint32_t i = 0; i < n; i++)
				hs[i] = gb_p2h(HK_DEVICE, devs[i]);
			/* total count may exceed what was asked for */
			if (max == 0 || (EGLint)a == 0)
				n = (uint32_t)num;
		}
	} else {
		EGLAttrib v = 0;
		EGLBoolean rc = 0;

		if (sub == 2 && fn_eglQueryDeviceAttribEXT)
			rc = fn_eglQueryDeviceAttribEXT(
				(EGLDeviceEXT)gb_h2p(HK_DEVICE, a), (EGLint)b, &v);
		else if (sub == 3 && fn_eglQueryDisplayAttribEXT)
			rc = fn_eglQueryDisplayAttribEXT(
				(EGLDisplay)gb_h2p(HK_DISPLAY, a), (EGLint)b, &v);
		if (rc) {
			ok = 1;
			if (sub == 3 && b == EGL_DEVICE_EXT_ATTR)
				value = gb_p2h(HK_DEVICE, (void *)v);
			else
				value = (uint64_t)v;
		}
	}

	if (h->flags & GB_CMD_F_WANT_REPLY) {
		struct gb_out o;
		struct gb_w w;
		uint32_t nh = sub == 0 && n <= GB_MAX_DEVICES && ok ? n : 0;
		uint32_t total = GB_ALIGN_UP(16 + 16 + 4 * nh, 8u);

		if (!gb_sv_reply_begin(t, h, &o, total)) {
			gb_w_init(&w, o.p);
			gb_w_u32(&w, ok);
			gb_w_u32(&w, n);
			gb_w_u64(&w, value);
			for (uint32_t i = 0; i < nh; i++)
				gb_w_u32(&w, hs[i]);
			gb_sv_reply_end(t, &o);
		}
	}
	return;
bad:
	gb_sv_proto_error(t, h);
}

/*
 * ElementMax: largest index in a range of the bound ELEMENT_ARRAY_BUFFER.
 * Lets the client size the vertex data it must upload when client-memory
 * attribute arrays are drawn with buffer-object indices. Replies -1 when the
 * range is not inside the buffer (the draw would be invalid) so that no GL
 * error is raised behind the application's back.
 */
static void h_element_max(struct gb_sth *t, const struct gb_cmd_hdr *h,
			  struct gb_r *r)
{
	uint32_t itype = gb_r_u32(r), count = gb_r_u32(r);
	uint64_t off = gb_r_u64(r);
	int ib = itype == 0x1401 ? 1 : itype == 0x1403 ? 2
		 : itype == 0x1405 ? 4 : 0;
	int64_t max = -1;

	if (r->err || !ib || count == 0 || count > (1u << 26) ||
	    off > (1ull << 40))
		goto bad;

	GB_NATIVE(glGetBufferParameteriv, void, (GLenum, GLenum, GLint *));
	GB_NATIVE(glGetBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, void *));
	GB_NATIVE(glMapBufferRange, void *,
		  (GLenum, GLintptr, GLsizeiptr, GLbitfield));
	GB_NATIVE(glUnmapBuffer, GLboolean, (GLenum));
	GB_NATIVE(eglGetCurrentContext, EGLContext, (void));
	uint64_t len = (uint64_t)count * (uint64_t)ib;
	GLint size = 0;

	if (fn_glGetBufferParameteriv)
		fn_glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER,
					  0x8764 /* BUFFER_SIZE */, &size);
	if (size > 0 && off + len <= (uint64_t)size) {
		int es = fn_eglGetCurrentContext &&
			 fn_eglGetCurrentContext() != EGL_NO_CONTEXT;
		uint8_t *buf = gb_tmp(t, (size_t)len + 8);
		const uint8_t *src = NULL;

		if (buf && !es && fn_glGetBufferSubData) {
			fn_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
					      (GLintptr)off, (GLsizeiptr)len,
					      buf);
			src = buf;
		} else if (fn_glMapBufferRange && fn_glUnmapBuffer) {
			const uint8_t *p = fn_glMapBufferRange(
				GL_ELEMENT_ARRAY_BUFFER, (GLintptr)off,
				(GLsizeiptr)len, 0x0001 /* MAP_READ_BIT */);
			if (p && buf) {
				memcpy(buf, p, (size_t)len);
				src = buf;
			}
			if (p)
				fn_glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
		}
		if (src) {
			for (uint32_t i = 0; i < count; i++) {
				int64_t x = ib == 1 ? src[i]
					  : ib == 2 ? ((const uint16_t *)src)[i]
						    : ((const uint32_t *)src)[i];
				if (x > max)
					max = x;
			}
		}
	}

	if (h->flags & GB_CMD_F_WANT_REPLY) {
		struct gb_out o;
		struct gb_w w;

		if (!gb_sv_reply_begin(t, h, &o, 24)) {
			gb_w_init(&w, o.p);
			gb_w_u64(&w, (uint64_t)max);
			gb_sv_reply_end(t, &o);
		}
	}
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
static void set_pointer(const struct cattr *a, size_t off)
{
	const void *p = (const void *)(uintptr_t)off;
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
		if (fn_glVertexPointer) fn_glVertexPointer(sz, a->type, st, p);
		break;
	case CA_NORMAL:
		if (fn_glNormalPointer) fn_glNormalPointer(a->type, st, p);
		break;
	case CA_COLOR:
		if (fn_glColorPointer) fn_glColorPointer(sz, a->type, st, p);
		break;
	case CA_SECONDARY:
		if (fn_glSecondaryColorPointer)
			fn_glSecondaryColorPointer(sz, a->type, st, p);
		break;
	case CA_FOGCOORD:
		if (fn_glFogCoordPointer) fn_glFogCoordPointer(a->type, st, p);
		break;
	case CA_EDGEFLAG:
		if (fn_glEdgeFlagPointer) fn_glEdgeFlagPointer(st, p);
		break;
	case CA_INDEX:
		if (fn_glIndexPointer) fn_glIndexPointer(a->type, st, p);
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
				fn_glTexCoordPointer(sz, a->type, st, p);
				fn_glClientActiveTexture((GLenum)old);
			}
		} else if (a->slot >= CA_GENERIC0 && a->slot < CA_COUNT) {
			GLuint idx = a->slot - CA_GENERIC0;
			if ((a->flags & GB_CA_LONG) && fn_glVertexAttribLPointer)
				fn_glVertexAttribLPointer(idx, sz, a->type, st, p);
			else if ((a->flags & GB_CA_INTEGER) &&
				 fn_glVertexAttribIPointer)
				fn_glVertexAttribIPointer(idx, sz, a->type, st, p);
			else if (fn_glVertexAttribPointer)
				fn_glVertexAttribPointer(
					idx, sz, a->type,
					(GLboolean)!!(a->flags & GB_CA_NORMALIZED),
					st, p);
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
 *   u32 old_abo, u32 old_ebo (bindings to restore afterwards),
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
	uint32_t old_abo = gb_r_u32(r), old_ebo = gb_r_u32(r);
	uint32_t bound_off = gb_r_u32(r); /* kind 2: byte offset in bound EBO */

	if (r->err || kind > 2 || count < 0 || nattr > CA_COUNT ||
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
	if (kind == 2) {
		/* indices stay in the application's own element buffer */
		if (!index_type_bytes(itype) || ibytes || !nattr)
			goto bad;
	} else if (kind == 1) {
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
	GB_NATIVE(glBufferSubData, void,
		  (GLenum, GLintptr, GLsizeiptr, const void *));
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
	    !fn_glBufferSubData)
		return;

	struct gb_scratch *s = scratch_for(t);
	if (!s)
		return; /* no current context: native would error anyway */

	/* ---- vertex data: one buffer, running offset, orphan on wrap ---- */
	size_t vtotal = 0;
	for (uint32_t i = 0; i < nattr; i++)
		vtotal += GB_ALIGN_UP(a[i].len, 16u);
	if (nattr) {
		if (!s->vbo)
			fn_glGenBuffers(1, &s->vbo);
		fn_glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
		if (s->vbo_cap < vtotal || s->vbo_off + vtotal > s->vbo_cap) {
			size_t cap = s->vbo_cap ? s->vbo_cap : SCRATCH_VBO_BYTES;
			if (cap < vtotal)
				cap = vtotal;
			fn_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cap, NULL,
					GL_STREAM_DRAW); /* orphan */
			s->vbo_cap = cap;
			s->vbo_off = 0;
		}
		for (uint32_t i = 0; i < nattr; i++) {
			fn_glBufferSubData(GL_ARRAY_BUFFER,
					   (GLintptr)s->vbo_off,
					   (GLsizeiptr)a[i].len, data[i]);
			set_pointer(&a[i], s->vbo_off);
			s->vbo_off += GB_ALIGN_UP(a[i].len, 16u);
		}
	}

	/* ---- indices ---- */
	size_t ioff = kind == 2 ? bound_off : 0;
	if (kind == 1) {
		size_t itotal = GB_ALIGN_UP(ibytes, 16u);
		if (!s->ebo)
			fn_glGenBuffers(1, &s->ebo);
		fn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ebo);
		if (s->ebo_cap < itotal || s->ebo_off + itotal > s->ebo_cap) {
			size_t cap = s->ebo_cap ? s->ebo_cap : SCRATCH_EBO_BYTES;
			if (cap < itotal)
				cap = itotal;
			fn_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)cap,
					NULL, GL_STREAM_DRAW);
			s->ebo_cap = cap;
			s->ebo_off = 0;
		}
		if (ibytes)
			fn_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
					   (GLintptr)s->ebo_off,
					   (GLsizeiptr)ibytes, idx);
		ioff = s->ebo_off;
		s->ebo_off += itotal;
	}

	const void *ip = (const void *)(uintptr_t)ioff;
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
		if (instances || baseinstance) {
			GLsizei n = instances ? (GLsizei)instances : 1;
			if (fn_glDrawElementsInstancedBaseVertexBaseInstance)
				fn_glDrawElementsInstancedBaseVertexBaseInstance(
					mode, count, itype, ip, n, basevertex,
					baseinstance);
			else if (basevertex && fn_glDrawElementsInstancedBaseVertex)
				fn_glDrawElementsInstancedBaseVertex(
					mode, count, itype, ip, n, basevertex);
			else if (fn_glDrawElementsInstanced)
				fn_glDrawElementsInstanced(mode, count, itype,
							   ip, n);
		} else if (basevertex && fn_glDrawElementsBaseVertex) {
			fn_glDrawElementsBaseVertex(mode, count, itype, ip,
						    basevertex);
		} else if (fn_glDrawElements) {
			fn_glDrawElements(mode, count, itype, ip);
		}
		if (kind == 1)
			fn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, old_ebo);
	}
	if (nattr)
		fn_glBindBuffer(GL_ARRAY_BUFFER, old_abo);
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
	case GB_OPX_BufferRead: h_buffer_read(t, h, r); break;
	case GB_OPX_ElementMax: h_element_max(t, h, r); break;
	case GB_OPX_EglDevice: h_egl_device(t, h, r); break;
	case GB_OPX_SelectBuffer: h_select_buffer(t, h, r); break;
	case GB_OPX_RenderMode: h_render_mode(t, h, r); break;
	case GB_OPX_DeviceString: h_device_string(t, h, r); break;
	default: gb_sv_proto_error(t, h); break;
	}
}

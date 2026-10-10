/*
 * gb_client_gl.c - hand-written part of the client EGL/GLES library.
 *
 *  - native display/window arguments (Display* -> string, Window -> XID)
 *  - string results (cached, extension lists filtered to what we implement)
 *  - client-memory vertex arrays and index data (shadowed GL state, uploaded
 *    at draw time; see ARCHITECTURE.md 5.6)
 *  - pixel-store shadowing and PBO guards
 *
 * Wire layouts mirror server/gb_server_manual.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

#include "gb_client_impl.h"

#define API __attribute__((visibility("default")))

#define GL_ARRAY_BUFFER_ 0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ 0x8893
#define GL_PIXEL_PACK_BUFFER_ 0x88EB
#define GL_PIXEL_UNPACK_BUFFER_ 0x88EC
#define GL_EXTENSIONS_ 0x1F03
#define GL_NUM_EXTENSIONS_ 0x821D
#define GL_VERTEX_ATTRIB_ARRAY_POINTER_ 0x8645

/* ---- local errors ----------------------------------------------------------- */

static void egl_err(EGLint e)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && !cx->egl_error)
		cx->egl_error = e;
}

static void gl_err(GLenum e)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && !cx->gl_error)
		cx->gl_error = e;
}

void gb_cl_unsupported_egl(const char *what)
{
	gb_cl_unsupported(what);
	egl_err(EGL_BAD_ALLOC);
}

void gb_cl_unsupported_gl(const char *what)
{
	gb_cl_unsupported(what);
	gl_err(GL_INVALID_OPERATION);
}

/* ---- interned strings --------------------------------------------------------- */

struct istr {
	struct istr *next;
	char s[];
};
static struct istr *g_istr;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *intern(const char *s, size_t len)
{
	pthread_mutex_lock(&g_mu);
	for (struct istr *i = g_istr; i; i = i->next)
		if (!strncmp(i->s, s, len) && i->s[len] == 0) {
			pthread_mutex_unlock(&g_mu);
			return i->s;
		}
	struct istr *n = malloc(sizeof(*n) + len + 1);
	if (!n) {
		pthread_mutex_unlock(&g_mu);
		return NULL;
	}
	memcpy(n->s, s, len);
	n->s[len] = 0;
	n->next = g_istr;
	g_istr = n;
	pthread_mutex_unlock(&g_mu);
	return n->s;
}

/* ---- per-context shadow state --------------------------------------------------- */

static struct gb_ctxstate *g_states;

static struct gb_ctxstate *state_for(uint32_t ctx_id)
{
	pthread_mutex_lock(&g_mu);
	struct gb_ctxstate *s;
	for (s = g_states; s; s = s->next)
		if (s->ctx_id == ctx_id)
			goto out;
	s = calloc(1, sizeof(*s));
	if (s) {
		s->ctx_id = ctx_id;
		s->pack = (struct gb_pixstore){ 4, 0, 0, 0 };
		s->unpack = (struct gb_pixstore){ 4, 0, 0, 0 };
		s->next = g_states;
		g_states = s;
	}
out:
	pthread_mutex_unlock(&g_mu);
	return s;
}

static struct gb_ctxstate *cur_state(void)
{
	struct gb_cl *cx = gb_cl_get();
	return cx ? cx->cur : NULL;
}

/* ---- generic send helpers ------------------------------------------------------- */

/* Begin a hand-written message; returns the cursor or NULL if disconnected.
 * `size` must already include alignment padding of any blobs. */
static struct gb_w *msg_begin(struct gb_cl *cx, struct gb_out *o,
			      struct gb_w *w, uint32_t op, uint32_t size,
			      uint32_t flags)
{
	if (gb_cl_begin(cx, o, op, GB_ALIGN_UP(size, 8u), flags))
		return NULL;
	gb_w_init(w, o->p);
	return w;
}

/* ---- EGL: display / strings / proc addresses ------------------------------------- */

static EGLDisplay get_display_kind(uint32_t kind, const char *str)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t slen = str ? (uint32_t)strlen(str) + 1 : GB_NULL_LEN;
	uint32_t sz = 16 + 8;

	if (!cx)
		return EGL_NO_DISPLAY;
	if (str)
		sz = GB_ALIGN_UP(sz, 8u) + slen;
	if (!msg_begin(cx, &o, &w, GB_OPX_GetDisplay, sz,
		       GB_CMD_F_WANT_REPLY))
		return EGL_NO_DISPLAY;
	gb_w_u32(&w, kind);
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

static EGLDisplay x11_display_from_native(void *native)
{
	if (!native) {
		const char *d = getenv("DISPLAY");
		return get_display_kind(1, d ? d : ":0");
	}
	/* The app's Display* lives in its own address space; the server opens
	 * its own connection to the same X server by name. */
	char *(*xds)(void *) =
		(char *(*)(void *))dlsym(RTLD_DEFAULT, "XDisplayString");
	if (!xds) {
		egl_err(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}
	const char *name = xds(native);
	return get_display_kind(1, name ? name : ":0");
}

API EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
	if (display_id == EGL_DEFAULT_DISPLAY)
		return get_display_kind(0, NULL);
	return x11_display_from_native((void *)display_id);
}

static EGLDisplay platform_display(EGLenum platform, void *native)
{
	switch (platform) {
	case 0x31D5: /* EGL_PLATFORM_X11_{KHR,EXT} */
		return x11_display_from_native(native);
	case 0x31DD: /* EGL_PLATFORM_SURFACELESS_MESA */
		return get_display_kind(2, NULL);
	default:
		egl_err(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}
}

API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display,
				     const EGLAttrib *attrib_list)
{
	return platform_display(platform, native_display);
}

API EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native_display,
					const EGLint *attrib_list)
{
	return platform_display(platform, native_display);
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
	if (attribs && n < 0) {
		egl_err(EGL_BAD_ATTRIBUTE);
		return EGL_NO_SURFACE;
	}
	if (attribs)
		sz = GB_ALIGN_UP(sz, 8u) + alen;
	if (!msg_begin(cx, &o, &w, GB_OPX_CreateWindowSurface, sz,
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

API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
				      EGLNativeWindowType win,
				      const EGLint *attrib_list)
{
	return create_window_surface(dpy, config, (uint32_t)(uintptr_t)win,
				     attrib_list);
}

API EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy,
						 EGLConfig config,
						 void *native_window,
						 const EGLint *attrib_list)
{
	/* EGL_EXT_platform_x11: native_window points at a Window. */
	if (!native_window) {
		egl_err(EGL_BAD_NATIVE_WINDOW);
		return EGL_NO_SURFACE;
	}
	return create_window_surface(dpy, config,
				     (uint32_t)*(unsigned long *)native_window,
				     attrib_list);
}

API EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config,
					      void *native_window,
					      const EGLAttrib *attrib_list)
{
	EGLint tmp[GB_ATTRIB_MAX + 1];
	const EGLint *p = NULL;

	if (attrib_list) {
		int64_t n = gb_attrib_count_EGLAttrib(
			(const intptr_t *)attrib_list);
		if (n < 0 || n > GB_ATTRIB_MAX) {
			egl_err(EGL_BAD_ATTRIBUTE);
			return EGL_NO_SURFACE;
		}
		for (int64_t i = 0; i < n; i++)
			tmp[i] = (EGLint)attrib_list[i];
		p = tmp;
	}
	return eglCreatePlatformWindowSurfaceEXT(dpy, config, native_window, p);
}

/* ---- extension filtering ------------------------------------------------------------- */

static int proc_present(const char *name)
{
	for (const struct gb_proc *p = gb_proc_table; p->name; p++)
		if (!strcmp(p->name, name))
			return 1;
	return 0;
}

/* An extension is advertised only if the registry knows it and every
 * command it adds is implemented. Unknown extensions are dropped. */
static int ext_allowed(const char *name, size_t len)
{
	for (const struct gb_ext *e = gb_ext_table; e->name; e++) {
		if (strlen(e->name) != len || strncmp(e->name, name, len))
			continue;
		for (int i = 0; i < e->ncmds; i++)
			if (!proc_present(e->cmds[i]))
				return 0;
		return 1;
	}
	return 0;
}

/* Returns an interned, space-separated, filtered copy of `list`. */
static const char *filter_ext_string(const char *list)
{
	size_t cap = strlen(list) + 1;
	char *out = malloc(cap);
	size_t o = 0;

	if (!out)
		return NULL;
	out[0] = 0;
	for (const char *p = list; *p;) {
		while (*p == ' ')
			p++;
		const char *s = p;
		while (*p && *p != ' ')
			p++;
		size_t len = (size_t)(p - s);
		if (len && ext_allowed(s, len)) {
			if (o)
				out[o++] = ' ';
			memcpy(out + o, s, len);
			o += len;
			out[o] = 0;
		}
	}
	const char *r = intern(out, o);
	free(out);
	return r;
}

/* ---- eglQueryString ------------------------------------------------------------------ */

static const char *fetch_string(uint32_t op, uint32_t a, uint32_t b)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || !msg_begin(cx, &o, &w, op, 24, GB_CMD_F_WANT_REPLY))
		return NULL;
	gb_w_u32(&w, a);
	gb_w_u32(&w, b);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, op))
		return NULL;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t len = gb_r_u32(&r);
	const char *res = NULL;
	if (len != GB_NULL_LEN && len > 0 && len <= (1u << 20)) {
		const char *s = gb_r_blob(&r, len);
		if (s && !r.err && s[len - 1] == 0)
			res = intern(s, len - 1);
	}
	gb_in_release(&in);
	return res;
}

API const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
	const char *s = fetch_string(GB_OPX_QueryString,
				     (uint32_t)(uintptr_t)dpy, (uint32_t)name);
	if (s && name == EGL_EXTENSIONS)
		s = filter_ext_string(s);
	return s;
}

API __eglMustCastToProperFunctionPointerType eglGetProcAddress(
	const char *procname)
{
	if (!procname)
		return NULL;
	for (const struct gb_proc *p = gb_proc_table; p->name; p++)
		if (!strcmp(p->name, procname))
			return (__eglMustCastToProperFunctionPointerType)p->addr;
	return NULL;
}

/* ---- wrapped: error merging, current context ---------------------------------------- */

API EGLint eglGetError(void)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && cx->egl_error) {
		EGLint e = cx->egl_error;
		cx->egl_error = 0;
		return e;
	}
	return gbx_eglGetError();
}

API GLenum glGetError(void)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && cx->gl_error) {
		GLenum e = cx->gl_error;
		cx->gl_error = 0;
		return e;
	}
	return gbx_glGetError();
}

API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
			      EGLContext ctx)
{
	EGLBoolean r = gbx_eglMakeCurrent(dpy, draw, read, ctx);
	struct gb_cl *cx = gb_cl_get();

	if (r && cx)
		cx->cur = ctx ? state_for((uint32_t)(uintptr_t)ctx) : NULL;
	return r;
}

API EGLBoolean eglReleaseThread(void)
{
	EGLBoolean r = gbx_eglReleaseThread();
	struct gb_cl *cx = gb_cl_get();

	if (cx)
		cx->cur = NULL;
	return r;
}

/* ---- GL strings --------------------------------------------------------------------- */

struct extlist {
	uint32_t ctx_id;
	const char *joined;
	const char **names;
	int n;
};
static struct extlist g_ext;

/* Filtered extension list for the current context (cached per context id). */
static const struct extlist *ext_list(void)
{
	struct gb_ctxstate *st = cur_state();
	uint32_t id = st ? st->ctx_id : 0;

	pthread_mutex_lock(&g_mu);
	if (g_ext.joined && g_ext.ctx_id == id) {
		pthread_mutex_unlock(&g_mu);
		return &g_ext;
	}
	pthread_mutex_unlock(&g_mu);

	const char *native = fetch_string(GB_OPX_GetString, GL_EXTENSIONS_,
					  0xFFFFFFFFu);
	if (!native)
		return NULL;
	const char *joined = filter_ext_string(native);
	if (!joined)
		return NULL;

	int n = 0;
	for (const char *p = joined; *p; p++)
		if (*p == ' ')
			n++;
	n += joined[0] ? 1 : 0;
	const char **names = calloc((size_t)n + 1, sizeof(char *));
	int k = 0;
	for (const char *p = joined; *p && names;) {
		const char *s = p;
		while (*p && *p != ' ')
			p++;
		names[k++] = intern(s, (size_t)(p - s));
		if (*p == ' ')
			p++;
	}
	pthread_mutex_lock(&g_mu);
	free(g_ext.names); /* leak-tolerant: per-context switches are rare */
	g_ext.ctx_id = id;
	g_ext.joined = joined;
	g_ext.names = names;
	g_ext.n = names ? k : 0;
	pthread_mutex_unlock(&g_mu);
	return &g_ext;
}

API const GLubyte *glGetString(GLenum name)
{
	if (name == GL_EXTENSIONS_) {
		const struct extlist *l = ext_list();
		return l ? (const GLubyte *)l->joined : NULL;
	}
	return (const GLubyte *)fetch_string(GB_OPX_GetString, name,
					     0xFFFFFFFFu);
}

API const GLubyte *glGetStringi(GLenum name, GLuint index)
{
	if (name == GL_EXTENSIONS_) {
		const struct extlist *l = ext_list();
		if (!l || index >= (GLuint)l->n) {
			gl_err(GL_INVALID_VALUE);
			return NULL;
		}
		return (const GLubyte *)l->names[index];
	}
	return (const GLubyte *)fetch_string(GB_OPX_GetString, name, index);
}

API void glGetIntegerv(GLenum pname, GLint *data)
{
	if (pname == GL_NUM_EXTENSIONS_ && data) {
		const struct extlist *l = ext_list();
		if (l) {
			*data = l->n;
			return;
		}
	}
	gbx_glGetIntegerv(pname, data);
}

/* ---- glShaderSource ------------------------------------------------------------------- */

API void glShaderSource(GLuint shader, GLsizei count,
			const GLchar *const *string, const GLint *length)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx)
		return;
	if (count < 0 || (count > 0 && !string) || count > 4096) {
		gl_err(count < 0 || !string ? GL_INVALID_VALUE
					    : GL_INVALID_OPERATION);
		return;
	}
	uint64_t total = 4ull * count;
	uint32_t *lens = malloc(((size_t)count + 1) * sizeof(uint32_t));
	if (!lens)
		return;
	for (GLsizei i = 0; i < count; i++) {
		size_t l = 0;
		if (string[i])
			l = (length && length[i] >= 0) ? (size_t)length[i]
						       : strlen(string[i]);
		lens[i] = (uint32_t)l;
		total += l;
	}
	if (total > GB_MAX_ARRAY_BYTES) {
		free(lens);
		gl_err(GL_OUT_OF_MEMORY);
		return;
	}
	uint32_t sz = GB_ALIGN_UP(16 + 12, 8u) + (uint32_t)total;
	if (!msg_begin(cx, &o, &w, GB_OPX_ShaderSource, sz, 0)) {
		free(lens);
		return;
	}
	gb_w_u32(&w, shader);
	gb_w_u32(&w, (uint32_t)count);
	gb_w_u32(&w, (uint32_t)total);
	uint8_t *blob = gb_w_blob(&w, (uint32_t)total);
	memcpy(blob, lens, 4u * (uint32_t)count);
	size_t off = 4u * (size_t)count;
	for (GLsizei i = 0; i < count; i++) {
		if (lens[i])
			memcpy(blob + off, string[i], lens[i]);
		off += lens[i];
	}
	gb_cl_end(cx, &o);
	free(lens);
}

/* ---- buffers / vertex arrays: shadow state ------------------------------------------- */

API void glBindBuffer(GLenum target, GLuint buffer)
{
	struct gb_ctxstate *st = cur_state();

	if (st) {
		switch (target) {
		case GL_ARRAY_BUFFER_: st->array_buffer = buffer; break;
		case GL_ELEMENT_ARRAY_BUFFER_: st->element_buffer = buffer; break;
		case GL_PIXEL_PACK_BUFFER_: st->pack_buffer = buffer; break;
		case GL_PIXEL_UNPACK_BUFFER_: st->unpack_buffer = buffer; break;
		}
	}
	gbx_glBindBuffer(target, buffer);
}

API void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
	struct gb_ctxstate *st = cur_state();

	if (st && buffers && n > 0) {
		for (GLsizei i = 0; i < n; i++) {
			GLuint b = buffers[i];
			if (!b)
				continue;
			if (st->array_buffer == b)
				st->array_buffer = 0;
			if (st->element_buffer == b)
				st->element_buffer = 0;
			if (st->pack_buffer == b)
				st->pack_buffer = 0;
			if (st->unpack_buffer == b)
				st->unpack_buffer = 0;
			for (int a = 0; a < GB_MAX_ATTRIBS; a++)
				if (st->attr[a].buffer == b)
					st->attr[a].buffer = 0;
		}
	}
	gbx_glDeleteBuffers(n, buffers);
}

API void glBindVertexArray(GLuint array)
{
	struct gb_ctxstate *st = cur_state();
	if (st)
		st->vao = array;
	gbx_glBindVertexArray(array);
}

API void glBindVertexArrayOES(GLuint array)
{
	struct gb_ctxstate *st = cur_state();
	if (st)
		st->vao = array;
	gbx_glBindVertexArrayOES(array);
}

API void glEnableVertexAttribArray(GLuint index)
{
	struct gb_ctxstate *st = cur_state();
	if (st && st->vao == 0 && index < GB_MAX_ATTRIBS)
		st->attr[index].enabled = 1;
	gbx_glEnableVertexAttribArray(index);
}

API void glDisableVertexAttribArray(GLuint index)
{
	struct gb_ctxstate *st = cur_state();
	if (st && st->vao == 0 && index < GB_MAX_ATTRIBS)
		st->attr[index].enabled = 0;
	gbx_glDisableVertexAttribArray(index);
}

static void send_attrib_ptr_offset(GLuint index, GLint size, GLenum type,
				   GLboolean norm, GLsizei stride,
				   uint64_t off)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || !msg_begin(cx, &o, &w, GB_OPX_VertexAttribPtrOff, 16 + 28 + 4, 0))
		return;
	gb_w_u32(&w, index);
	gb_w_u32(&w, (uint32_t)size);
	gb_w_u32(&w, type);
	gb_w_u32(&w, norm);
	gb_w_u32(&w, (uint32_t)stride);
	gb_w_u64(&w, off);
	gb_cl_end(cx, &o);
}

API void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
			       GLboolean normalized, GLsizei stride,
			       const void *pointer)
{
	struct gb_ctxstate *st = cur_state();

	if (st && st->vao == 0 && index < GB_MAX_ATTRIBS) {
		struct gb_attrib *a = &st->attr[index];
		a->size = size;
		a->type = type;
		a->normalized = normalized;
		a->stride = stride;
		a->ptr = pointer;
		a->buffer = st->array_buffer;
		if (st->array_buffer == 0)
			return; /* client array: uploaded at draw time */
	}
	send_attrib_ptr_offset(index, size, type, normalized, stride,
			       (uint64_t)(uintptr_t)pointer);
}

API void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer)
{
	struct gb_ctxstate *st = cur_state();

	if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER_) {
		gl_err(GL_INVALID_ENUM);
		return;
	}
	if (index >= GB_MAX_ATTRIBS || !pointer) {
		gl_err(GL_INVALID_VALUE);
		return;
	}
	*pointer = (st && st->vao == 0) ? (void *)st->attr[index].ptr : NULL;
}

/* ---- draws ------------------------------------------------------------------------------ */

static int any_client_arrays(const struct gb_ctxstate *st)
{
	for (int i = 0; i < GB_MAX_ATTRIBS; i++)
		if (st->attr[i].enabled && st->attr[i].buffer == 0)
			return 1;
	return 0;
}

/* mode/kind/count/type + attribs with data for vertices [0, nverts) starting
 * at vertex `first` of each client array. Returns 0 if sent. */
static int send_draw_client(struct gb_ctxstate *st, GLenum mode, int kind,
			    GLsizei count, GLenum itype, const void *indices,
			    uint32_t nverts, GLint first)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t na = 0, ibytes = 0;
	struct {
		uint32_t index, size, type, norm, stride, len;
		const uint8_t *src;
	} at[GB_MAX_ATTRIBS];

	if (!cx)
		return -1;
	for (uint32_t i = 0; i < GB_MAX_ATTRIBS; i++) {
		const struct gb_attrib *a = &st->attr[i];
		if (!a->enabled || a->buffer != 0)
			continue;
		int eb = gb_attrib_elem_bytes(a->type, a->size);
		if (!a->ptr || eb <= 0 || a->size < 1 || a->size > 4 ||
		    a->stride < 0 || nverts == 0) {
			gl_err(GL_INVALID_OPERATION);
			return -1;
		}
		int64_t stride = a->stride ? a->stride : eb;
		uint64_t len = (uint64_t)(nverts - 1) * stride + eb;
		if (len > GB_MAX_ARRAY_BYTES) {
			gl_err(GL_OUT_OF_MEMORY);
			return -1;
		}
		at[na].index = i;
		at[na].size = (uint32_t)a->size;
		at[na].type = a->type;
		at[na].norm = a->normalized;
		at[na].stride = (uint32_t)a->stride;
		at[na].len = (uint32_t)len;
		at[na].src = (const uint8_t *)a->ptr +
			     (int64_t)first * stride;
		na++;
	}
	if (kind == 1) {
		int ib = itype == 0x1401 ? 1 : itype == 0x1403 ? 2
					   : itype == 0x1405 ? 4 : 0;
		if (!ib || !indices) {
			gl_err(GL_INVALID_ENUM);
			return -1;
		}
		ibytes = (uint32_t)count * (uint32_t)ib;
	}
	uint32_t sz = 16 + 28 + 24 * na;
	if (ibytes)
		sz = GB_ALIGN_UP(sz, 8u) + ibytes;
	for (uint32_t i = 0; i < na; i++)
		sz = GB_ALIGN_UP(sz, 8u) + at[i].len;
	if (!msg_begin(cx, &o, &w, GB_OPX_DrawClient, sz, 0))
		return -1;
	gb_w_u32(&w, mode);
	gb_w_u32(&w, (uint32_t)kind);
	gb_w_u32(&w, (uint32_t)count);
	gb_w_u32(&w, itype);
	gb_w_u32(&w, nverts);
	gb_w_u32(&w, ibytes);
	gb_w_u32(&w, na);
	for (uint32_t i = 0; i < na; i++) {
		gb_w_u32(&w, at[i].index);
		gb_w_u32(&w, at[i].size);
		gb_w_u32(&w, at[i].type);
		gb_w_u32(&w, at[i].norm);
		gb_w_u32(&w, at[i].stride);
		gb_w_u32(&w, at[i].len);
	}
	if (ibytes)
		memcpy(gb_w_blob(&w, ibytes), indices, ibytes);
	for (uint32_t i = 0; i < na; i++)
		memcpy(gb_w_blob(&w, at[i].len), at[i].src, at[i].len);
	gb_cl_end(cx, &o);
	return 0;
}

API void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	struct gb_ctxstate *st = cur_state();

	if (st && st->vao == 0 && count > 0 && first >= 0 &&
	    any_client_arrays(st)) {
		send_draw_client(st, mode, 0, count, 0, NULL, (uint32_t)count,
				 first);
		return;
	}
	gbx_glDrawArrays(mode, first, count);
}

API void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
			       GLsizei instancecount)
{
	struct gb_ctxstate *st = cur_state();

	if (st && st->vao == 0 && any_client_arrays(st)) {
		gb_cl_unsupported("glDrawArraysInstanced with client arrays");
		gl_err(GL_INVALID_OPERATION);
		return;
	}
	gbx_glDrawArraysInstanced(mode, first, count, instancecount);
}

API void glDrawElements(GLenum mode, GLsizei count, GLenum type,
			const void *indices)
{
	struct gb_ctxstate *st = cur_state();
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx)
		return;
	if (st && st->vao == 0 && st->element_buffer == 0 && count > 0) {
		/* Client-memory indices: find the highest referenced vertex
		 * so the matching attribute ranges can be uploaded. */
		int ib = type == 0x1401 ? 1 : type == 0x1403 ? 2
				   : type == 0x1405 ? 4 : 0;
		if (!ib || !indices) {
			gl_err(!ib ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
			return;
		}
		uint32_t maxi = 0;
		for (GLsizei i = 0; i < count; i++) {
			uint32_t v = ib == 1 ? ((const uint8_t *)indices)[i]
				   : ib == 2 ? ((const uint16_t *)indices)[i]
					     : ((const uint32_t *)indices)[i];
			if (v > maxi)
				maxi = v;
		}
		uint32_t nverts = any_client_arrays(st) ? maxi + 1 : 0;
		if (maxi >= (1u << 26)) {
			gl_err(GL_OUT_OF_MEMORY);
			return;
		}
		send_draw_client(st, mode, 1, count, type, indices, nverts, 0);
		return;
	}
	if (st && st->vao == 0 && any_client_arrays(st)) {
		/* client vertex arrays + indices in a buffer object: the
		 * highest index is unknown to us. */
		gb_cl_unsupported("glDrawElements: client arrays with buffer "
				  "indices");
		gl_err(GL_INVALID_OPERATION);
		return;
	}
	if (!msg_begin(cx, &o, &w, GB_OPX_DrawElementsOff, 16 + 12 + 4 + 8, 0))
		return;
	gb_w_u32(&w, mode);
	gb_w_u32(&w, (uint32_t)count);
	gb_w_u32(&w, type);
	gb_w_u64(&w, (uint64_t)(uintptr_t)indices);
	gb_cl_end(cx, &o);
}

/* ---- pixel store and PBO guards -------------------------------------------------------- */

API void glPixelStorei(GLenum pname, GLint param)
{
	struct gb_ctxstate *st = cur_state();

	if (st) {
		int ok_align = param == 1 || param == 2 || param == 4 || param == 8;
		switch (pname) {
		case 0x0CF5: if (ok_align) st->unpack.alignment = param; break;
		case 0x0D05: if (ok_align) st->pack.alignment = param; break;
		case 0x0CF2: if (param >= 0) st->unpack.row_length = param; break;
		case 0x0D02: if (param >= 0) st->pack.row_length = param; break;
		case 0x0CF4: if (param >= 0) st->unpack.skip_pixels = param; break;
		case 0x0D04: if (param >= 0) st->pack.skip_pixels = param; break;
		case 0x0CF3: if (param >= 0) st->unpack.skip_rows = param; break;
		case 0x0D03: if (param >= 0) st->pack.skip_rows = param; break;
		}
	}
	gbx_glPixelStorei(pname, param);
}

static int pbo_unpack(void)
{
	struct gb_ctxstate *st = cur_state();
	if (st && st->unpack_buffer) {
		gb_cl_unsupported("PIXEL_UNPACK_BUFFER (PBO uploads)");
		gl_err(GL_INVALID_OPERATION);
		return 1;
	}
	return 0;
}

API void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
		      GLenum format, GLenum type, void *pixels)
{
	struct gb_ctxstate *st = cur_state();
	if (st && st->pack_buffer) {
		gb_cl_unsupported("PIXEL_PACK_BUFFER (PBO readback)");
		gl_err(GL_INVALID_OPERATION);
		return;
	}
	gbx_glReadPixels(x, y, width, height, format, type, pixels);
}

API void glTexImage2D(GLenum target, GLint level, GLint internalformat,
		      GLsizei width, GLsizei height, GLint border,
		      GLenum format, GLenum type, const void *pixels)
{
	if (pbo_unpack())
		return;
	gbx_glTexImage2D(target, level, internalformat, width, height, border,
			 format, type, pixels);
}

API void glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
			 GLint yoffset, GLsizei width, GLsizei height,
			 GLenum format, GLenum type, const void *pixels)
{
	if (pbo_unpack())
		return;
	gbx_glTexSubImage2D(target, level, xoffset, yoffset, width, height,
			    format, type, pixels);
}

API void glCompressedTexImage2D(GLenum target, GLint level,
				GLenum internalformat, GLsizei width,
				GLsizei height, GLint border,
				GLsizei imageSize, const void *data)
{
	if (pbo_unpack())
		return;
	gbx_glCompressedTexImage2D(target, level, internalformat, width,
				   height, border, imageSize, data);
}

API void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset,
				   GLint yoffset, GLsizei width, GLsizei height,
				   GLenum format, GLsizei imageSize,
				   const void *data)
{
	if (pbo_unpack())
		return;
	gbx_glCompressedTexSubImage2D(target, level, xoffset, yoffset, width,
				      height, format, imageSize, data);
}

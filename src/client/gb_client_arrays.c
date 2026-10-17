/*
 * gb_client_arrays.c - client-side shadow of vertex-array / buffer-binding /
 * pixel-store state, and the client-memory draw path.
 *
 * Why this exists (ARCHITECTURE.md 5.6): a vertex-array pointer may point at
 * client memory, which the server cannot see. Pointer calls are recorded
 * here; at draw time the used range of each enabled client array (and the
 * indices) is uploaded with the draw command (GB_OPX_DrawClient).
 *
 * Shared by both client libraries; desktop-only parts are under GB_LIB_GL.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>

#include "gb_client_impl.h"

/* ---- context states ----------------------------------------------------------- */

static struct gb_ctxstate *g_states;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

struct gb_ctxstate *gb_state_for(uint32_t ctx_id)
{
	struct gb_ctxstate *s;

	pthread_mutex_lock(&g_mu);
	for (s = g_states; s; s = s->next)
		if (s->ctx_id == ctx_id)
			goto out;
	s = calloc(1, sizeof(*s));
	if (s) {
		s->ctx_id = ctx_id;
		s->vao = &s->vao0;
		s->core = -1;
		s->es = -1;
		s->es_restart = -1;
		s->pack = (struct gb_pixstore){ 4, 0, 0, 0, 0, 0 };
		s->unpack = (struct gb_pixstore){ 4, 0, 0, 0, 0, 0 };
		s->next = g_states;
		g_states = s;
	}
out:
	pthread_mutex_unlock(&g_mu);
	return s;
}

/* A destroyed context's id is reused by the server for the next context; its
 * shadow (array pointers, bindings, maps, profile, extension list) must not
 * carry over. */
void gb_state_free(uint32_t ctx_id)
{
	struct gb_ctxstate **pp, *s = NULL;

	pthread_mutex_lock(&g_mu);
	for (pp = &g_states; *pp; pp = &(*pp)->next)
		if ((*pp)->ctx_id == ctx_id) {
			s = *pp;
			*pp = s->next;
			break;
		}
	pthread_mutex_unlock(&g_mu);
	if (!s)
		return;
	struct gb_cl *cx = gb_cl_get();
	if (cx && cx->cur == s)
		cx->cur = NULL;
	for (int i = 0; i < GB_MAX_MAPS; i++)
		free(s->maps[i].alloc);
	while (s->vaos) {
		struct gb_vao *v = s->vaos;

		s->vaos = v->next;
		free(v);
	}
	for (int i = 0; i < GB_CLIENT_ATTRIB_STACK; i++)
		free(s->stack[i]);
	free(s);
	gb_cl_ext_list_invalidate();
}

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...)
{
	static int on = -1;
	char buf[160];
	va_list ap;
	int n;

	if (on < 0)
		on = getenv("GB_TRACE_OPS") != NULL;
	if (!on)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[n++] = '\n';
	(void)!write(2, buf, (size_t)n);
}

static struct gb_ctxstate *cur_raw(void)
{
	struct gb_cl *cx = gb_cl_get();
	return cx ? cx->cur : NULL;
}

/* NULL between glBegin and glEnd: see struct gb_ctxstate::in_begin. Fine for
 * wrappers that only mirror state and otherwise forward; anything that would
 * forward a *client pointer* (draws, gl*Pointer) must use gb_cur_state_raw()
 * and raise the error itself, or the server would dereference it. */
struct gb_ctxstate *gb_cur_state(void)
{
	struct gb_ctxstate *st = cur_raw();
	return st && !st->in_begin ? st : NULL;
}

struct gb_ctxstate *gb_cur_state_raw(void)
{
	return cur_raw();
}

GB_API void glBegin(GLenum mode)
{
	struct gb_ctxstate *st = cur_raw();

	trace("c: glBegin(%#x) in_begin=%d list=%#x", mode, st ? st->in_begin : -1,
	      st ? st->list_mode : 0);

	/* Two kinds of "inside Begin/End": immediate (the driver executes the
	 * glBegin: all non-compilable commands, e.g. gl*Pointer, error) and
	 * compile-time (a list is open: compilable commands such as draws are
	 * checked while being compiled). COMPILE_AND_EXECUTE is both. */
	if (st && !st->in_begin && !st->save_begin && mode <= 0x000E) {
		if (st->list_mode)
			st->save_begin = 1;
		if (st->list_mode != 0x1300 /* COMPILE */)
			st->in_begin = 1;
	}
	gbx_glBegin(mode);
}

GB_API void glEnd(void)
{
	struct gb_ctxstate *st = cur_raw();

	trace("c: glEnd in_begin=%d list=%#x", st ? st->in_begin : -1, st ? st->list_mode : 0);

	if (st) {
		st->save_begin = 0;
		if (st->list_mode != 0x1300)
			st->in_begin = 0;
	}
	gbx_glEnd();
}

GB_API void glNewList(GLuint list, GLenum mode)
{
	struct gb_ctxstate *st = cur_raw();

	trace("c: glNewList(%u,%#x) list=%#x in_begin=%d", list, mode,
	      st ? st->list_mode : 0, st ? st->in_begin : -1);

	if (st && !st->in_begin && !st->list_mode && list != 0 &&
	    (mode == 0x1300 || mode == 0x1301))
		st->list_mode = (uint16_t)mode;
	gbx_glNewList(list, mode);
}

GB_API void glEndList(void)
{
	struct gb_ctxstate *st = cur_raw();

	trace("c: glEndList list=%#x in_begin=%d", st ? st->list_mode : 0, st ? st->in_begin : -1);

	if (st && !st->in_begin)
		st->list_mode = 0;
	gbx_glEndList();
}

static struct gb_vao *vao_get(struct gb_ctxstate *st, uint32_t name, int create)
{
	if (name == 0)
		return &st->vao0;
	for (struct gb_vao *v = st->vaos; v; v = v->next)
		if (v->name == name)
			return v;
	if (!create)
		return NULL;
	struct gb_vao *v = calloc(1, sizeof(*v));
	if (v) {
		v->name = name;
		v->next = st->vaos;
		st->vaos = v;
	}
	return v;
}

static void vao_free(struct gb_ctxstate *st, uint32_t name)
{
	if (name == 0)
		return;
	for (struct gb_vao **pp = &st->vaos; *pp; pp = &(*pp)->next) {
		if ((*pp)->name == name) {
			struct gb_vao *v = *pp;
			*pp = v->next;
			if (st->vao == v)
				st->vao = &st->vao0;
			free(v);
			return;
		}
	}
}

int gb_cl_buf_mapped(struct gb_cl *cx, int which)
{
	struct gb_ctxstate *st = cx ? cx->cur : NULL;
	uint32_t buf;
	struct gb_map *m;

	if (!st)
		return 0;
	buf = which == GB_BUF_PACK ? st->pack_buffer : st->unpack_buffer;
	m = buf ? gb_state_map(st, buf) : NULL;
	return m && !(m->flags & 0x40 /* MAP_PERSISTENT */);
}

int gb_cl_buf_bound(struct gb_cl *cx, int which)
{
	struct gb_ctxstate *st = cx ? cx->cur : NULL;

	if (!st)
		return 0;
	if (which == GB_BUF_QUERY)
		return gb_state_bound(st, 0x9192 /* QUERY_BUFFER */) != 0;
	return which == GB_BUF_PACK ? st->pack_buffer != 0
				    : st->unpack_buffer != 0;
}

int gb_cl_pack_sparse(struct gb_cl *cx, int64_t w, int64_t h, int64_t d,
		      uint32_t format, uint32_t type)
{
	struct gb_ctxstate *st = cx ? cx->cur : NULL;
	const struct gb_pixstore *ps;

	if (!st)
		return 0;
	ps = &st->pack;
	if (ps->skip_pixels || ps->skip_rows || ps->skip_images)
		return 1;
	if (w < 0 || h < 0 || d < 0)
		return ps->row_length != 0 || ps->image_height != 0;
	if (w == 0 || h == 0 || d == 0)
		return 0;
	/* tightly packed size vs the span the transfer covers */
	struct gb_pixstore dense = *ps;

	dense.row_length = dense.image_height = 0;
	dense.skip_pixels = dense.skip_rows = dense.skip_images = 0;
	dense.alignment = 1;
	int64_t tight = gb_pixel_bytes3(format, type, w, h, d > 1 ? d : GB_DEPTH_2D,
					&dense);
	int64_t span = gb_pixel_bytes3(format, type, w, h, d > 1 ? d : GB_DEPTH_2D,
				       ps);

	if (tight < 0 || span < 0)
		return ps->row_length != 0 || ps->image_height != 0;
	return span != tight;
}

void gb_cl_pack_store(struct gb_cl *cx, void *dst, const void *src,
		      uint32_t len, uint32_t format, uint32_t type, int32_t w,
		      int32_t h, int32_t d)
{
	struct gb_ctxstate *st = cx ? cx->cur : NULL;

	if (!st || w <= 0) {
		memcpy(dst, src, len);
		return;
	}
	gb_pixel_scatter(dst, src, len, format, type, w, h, d, &st->pack);
}

/* ---- size-rule inputs ----------------------------------------------------------- */

int gb_cl_geti(void *ctx, uint32_t pname)
{
	GLint v = 0;

	(void)ctx;
	glGetIntegerv(pname, &v);
	return v;
}

int64_t gb_cl_pixel_bytes(int mode, uint32_t format, uint32_t type, int64_t w,
			  int64_t h, int64_t d)
{
	static const struct gb_pixstore def = { 4, 0, 0, 0, 0, 0 };
	struct gb_ctxstate *st = gb_cur_state();
	const struct gb_pixstore *ps = &def;

	if (st)
		ps = mode == GB_PM_PACK ? &st->pack : &st->unpack;
	return gb_pixel_bytes3(format, type, w, h, d, ps);
}

int64_t gb_cl_bitmap_bytes(int mode, int64_t w, int64_t h)
{
	static const struct gb_pixstore def = { 4, 0, 0, 0, 0, 0 };
	struct gb_ctxstate *st = gb_cur_state();
	const struct gb_pixstore *ps = &def;

	if (st)
		ps = mode == GB_PM_PACK ? &st->pack : &st->unpack;
	return gb_bitmap_bytes(w, h, ps);
}

/* Core profiles (desktop 3.2+ without ARB_compatibility) have no client-
 * memory arrays. Determined lazily, once per context, without provoking
 * errors: version string first, profile mask only where it is valid. */
static int ctx_is_core(struct gb_ctxstate *st)
{
#ifdef GB_LIB_GL
	if (st->core < 0) {
		const char *v = (const char *)glGetString(GL_VERSION);
		int core = 0;
		if (v && strncmp(v, "OpenGL ES", 9)) {
			int maj = atoi(v), min = 0;
			const char *dot = strchr(v, '.');
			if (dot)
				min = atoi(dot + 1);
			if (maj > 3 || (maj == 3 && min >= 2)) {
				GLint mask = 0;
				glGetIntegerv(0x9126 /* PROFILE_MASK */, &mask);
				/* Core only when the core bit is set: legacy
				 * contexts may report 0 (NVIDIA does). */
				core = (mask & 1) && !(mask & 2);
			}
		}
		st->core = (int8_t)core;
	}
	return st->core;
#else
	(void)st;
	return 0;
#endif
}

/* The index that restarts the primitive for an index type, or -1 when
 * primitive restart is off. ES 3.0+ always restarts on the fixed index. */
/* ES 3.0+ context? (st->es is computed once) */
int gb_cl_es3_context(struct gb_ctxstate *st)
{
	if (st->es < 0) {
		const char *v = (const char *)glGetString(GL_VERSION);

		st->es = 0;
		if (v && !strncmp(v, "OpenGL ES", 9)) {
			v += 9;
			while (*v && (*v < '0' || *v > '9'))
				v++;
			st->es = atoi(v) >= 3;
		}
	}
	return st->es;
}

static int64_t restart_value(struct gb_ctxstate *st, uint32_t itype)
{
	if (st->es < 0) {
		const char *v = (const char *)glGetString(GL_VERSION);

		st->es = 0;
		if (v && !strncmp(v, "OpenGL ES", 9)) {
			v += 9;
			while (*v && (*v < '0' || *v > '9'))
				v++;
			st->es = atoi(v) >= 3;
		}
	}
	if (st->es) {
		/* ES: ask the driver whether the fixed restart index is active
		 * (an ES 2.0 context may report a 3.x version and still not
		 * restart) */
		if (st->es_restart < 0)
			st->es_restart = glIsEnabled(0x8D69) ? 1 : 0;
		if (!st->es_restart)
			return -1;
	} else if (!st->restart_fixed) {
		return st->restart_on ? (int64_t)st->restart_index : -1;
	}
	return itype == 0x1401 ? 0xFF : itype == 0x1403 ? 0xFFFF : 0xFFFFFFFFll;
}

/* ---- array shadow ----------------------------------------------------------------- */

/* An enabled array that reads application memory. A NULL pointer is not one:
 * the driver would fault on it, so an application that leaves such an array
 * enabled must not be using it (the shader ignores the attribute). */
static inline int is_client_array(const struct gb_carray *a)
{
	return a->enabled && a->buffer == 0 && a->ptr != NULL;
}

/* The pointer the application passed for client-memory data (enabled or
 * not); a buffer offset is the driver's to report. */
static inline int client_pointer(const struct gb_carray *a)
{
	return a->buffer == 0 && a->ptr != NULL;
}

/* Arguments that no driver accepts. The driver is still told (it raises the
 * error); we just must not record such a call as the array's state. Kept
 * deliberately lenient: a false "invalid" would lose a valid client array. */
static int pointer_args_invalid(int slot, int size, uint32_t type, int stride)
{
	static const uint32_t types[] = {
		0x1400, 0x1401, 0x1402, 0x1403, 0x1404, 0x1405, 0x1406, 0x140A,
		0x140B, 0x140C, 0x8D9F, 0x8368, 0x8C3B,
	};
	int known = 0;

	if (stride < 0)
		return 1;
	for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
		known |= types[i] == type;
	if (!known)
		return 1;
	if (slot == CA_WEIGHT || slot == CA_MATRIXINDEX)
		return 0;
	if (size == 0x80E1 /* BGRA */)
		return !(type == 0x1401 || type == 0x8D9F || type == 0x8368);
	if (size < 1 || size > 4)
		return 1;
	if ((type == 0x8D9F || type == 0x8368) && size != 4)
		return 1;
	if (type == 0x8C3B && size != 3)
		return 1;
	return 0;
}

/* Record a *Pointer() call. Returns 0: buffer-backed, send it as is;
 * 1: handled (error raised), send nothing; 2: client memory, shadowed here --
 * send it with a NULL pointer so the driver validates it and tracks
 * size/type/stride (the pointer itself it can never use). */
int gb_arr_pointer(struct gb_ctxstate *st, int slot, int size, uint32_t type,
		   int normalized, int integer, int is_long, int stride,
		   const void *ptr)
{
	if (slot < 0 || slot >= CA_COUNT)
		return 0;
	/* Core profile: a non-NULL client pointer with no buffer bound is an
	 * error at the call (not at the draw), exactly as in the driver. */
	if (st->array_buffer == 0 && ptr != NULL && ctx_is_core(st)) {
		gb_cl_gl_error(0x0502 /* INVALID_OPERATION */);
		return 1;
	}
	/* ES 3.x: client memory is only for the default VAO */
	if (st->array_buffer == 0 && ptr != NULL && st->vao != &st->vao0 &&
	    gb_cl_es3_context(st)) {
		gb_cl_gl_error(0x0502);
		return 1;
	}
	struct gb_carray *a = &st->vao->a[slot];

	if (st->in_begin) { /* the driver would raise this */
		gb_cl_gl_error(0x0502);
		return 1;
	}
	if (!pointer_args_invalid(slot, size, type, stride)) {
		a->size = size;
		a->type = type;
		a->normalized = (uint8_t)normalized;
		a->integer = (uint8_t)integer;
		a->is_long = (uint8_t)is_long;
		a->stride = stride;
		a->ptr = ptr;
		a->buffer = st->array_buffer;
	}
	return st->array_buffer == 0 ? 2 : 0;
}

void gb_arr_enable(struct gb_ctxstate *st, int slot, int on)
{
	if (slot >= 0 && slot < CA_COUNT)
		st->vao->a[slot].enabled = (uint8_t)on;
}

/* ---- client-memory draws ------------------------------------------------------------- */

/* Largest index in [off, off + count * size(itype)) of the bound element
 * buffer, or -1 if the range is not inside the buffer. */
static int64_t gb_cl_element_max(uint32_t itype, uint32_t count, uintptr_t off,
				 int64_t restart)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	int64_t max = -1;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_ElementMax, 40,
				   GB_CMD_F_WANT_REPLY))
		return -1;
	gb_w_u32(&w, itype);
	gb_w_u32(&w, count);
	gb_w_u64(&w, (uint64_t)off);
	gb_w_u64(&w, (uint64_t)restart); /* -1: none */
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_ElementMax))
		return -1;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint64_t v = gb_r_u64(&r);
	if (!r.err)
		max = (int64_t)v;
	gb_in_release(&in);
	return max;
}

/* Compile a client-array draw into a display list as vertex data. Returns 1
 * if handled; 0 to let the call be forwarded (cases not representable). */
static int emulate_list_draw(struct gb_ctxstate *st, uint32_t mode, int indexed,
			     int32_t first, int32_t count, uint32_t itype,
			     const void *indices, int32_t instances,
			     int is_instanced, int32_t basevertex)
{
	int ib = itype == 0x1401 ? 1 : itype == 0x1403 ? 2 : itype == 0x1405 ? 4 : 0;

	if (count < 0) {
		gb_cl_gl_error(0x0501);
		return 1;
	}
	if ((is_instanced && instances != 1) ||
	    (indexed && (st->vao->element_buffer || !ib || !indices)))
		return 0;
	int64_t rs = indexed ? restart_value(st, itype) : -1;

	glBegin(mode);
	for (int32_t i = 0; i < count; i++) {
		int64_t e;

		if (!indexed) {
			e = (int64_t)first + i;
		} else {
			if (ib == 1) {
				e = ((const uint8_t *)indices)[i];
			} else if (ib == 2) {
				uint16_t t;

				memcpy(&t, (const uint8_t *)indices + 2 * i, 2);
				e = t;
			} else {
				uint32_t t;

				memcpy(&t, (const uint8_t *)indices + 4 * i, 4);
				e = t;
			}
			if (e == rs) { /* restart: a new primitive begins */
				glEnd();
				glBegin(mode);
				continue;
			}
			e += basevertex;
		}
		glArrayElement((GLint)e);
	}
	glEnd();
	return 1;
}

int gb_draw_client(struct gb_ctxstate *st, uint32_t mode, int indexed,
		   int32_t first, int32_t count, uint32_t itype,
		   const void *indices, int32_t instances, int is_instanced,
		   int32_t basevertex, uint32_t baseinstance)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_vao *v = st->vao;
	struct gb_out o;
	struct gb_w w;
	uint32_t nclient = 0, ibytes = 0, nverts;
	struct {
		uint32_t slot, size, type, stride, flags, divisor, len;
		const uint8_t *src;
	} at[CA_COUNT];

	if (!cx)
		return 0;
	/* an invalid primitive mode is the driver's INVALID_ENUM, which it
	 * reports before looking at anything else (indices, arrays) */
	if (mode > 0xE)
		return 0;
	if (__builtin_expect(gb_cl_persist_pending(cx), 0))
		gb_cl_persist_sync(cx, GB_PS_UP);
	for (int i = 0; i < CA_COUNT; i++)
		if (is_client_array(&v->a[i]))
			nclient++;
	if (st->list_mode) {
		/*
		 * Inside glNewList: array data is read when the list is
		 * compiled, and the server has no client pointers to read. A
		 * draw inside Begin/End is forwarded (the driver raises the
		 * error immediately, before touching any array, and compiles
		 * the command as it would natively); otherwise client arrays
		 * are compiled as immediate-mode vertices.
		 */
		/* client index memory counts as client data too */
		if (!nclient && (!indexed || v->element_buffer))
			return 0;
		/* Never forward client pointers: the server would dereference
		 * them (list compilation reads array data). Inside Begin/End:
		 * executing (COMPILE_AND_EXECUTE) is an error; compile-only is
		 * accepted silently by the drivers. */
		if (st->in_begin) {
			gb_cl_gl_error(0x0502);
			return 1;
		}
		if (st->save_begin)
			return 1;
		return emulate_list_draw(st, mode, indexed, first, count, itype,
					 indices, instances, is_instanced,
					 basevertex);
	}
	if (st->in_begin) { /* draws are invalid between glBegin and glEnd */
		gb_cl_gl_error(0x0502);
		return 1;
	}
	if (nclient && ctx_is_core(st)) {
		/* the driver would raise this; do not be more permissive */
		if (getenv("GB_TRACE_OPS"))
			for (int i = 0; i < CA_COUNT; i++)
				if (is_client_array(&v->a[i]))
					fprintf(stderr, "c: core context, "
						"client array in slot %d "
						"(type %#x size %d)\n", i,
						v->a[i].type, v->a[i].size);
		gb_cl_gl_error(0x0502 /* INVALID_OPERATION */);
		return 1;
	}

	uint32_t bound_off = 0;
	int from_ebo = 0; /* indices live in the application's element buffer */

	if (indexed) {
		if (v->element_buffer) {
			if (!nclient)
				return 0;
			if (count < 0 || (is_instanced && instances <= 0))
				return 0; /* let the driver report it */
			if (count == 0)
				return 0; /* the driver still validates state */
			int ib = itype == 0x1401 ? 1 : itype == 0x1403 ? 2
					   : itype == 0x1405 ? 4 : 0;
			uintptr_t eoff = (uintptr_t)indices;
			if (!ib) {
				gb_cl_gl_error(0x0500 /* INVALID_ENUM */);
				return 1;
			}
			if (eoff > 0x7fffffffu)
				return 0;
			/* One round trip: the server reads the range. */
			int64_t maxv = gb_cl_element_max(
				itype, (uint32_t)count, eoff,
				restart_value(st, itype));
			if (maxv < 0) {
				gb_cl_gl_error(0x0502 /* INVALID_OPERATION */);
				return 1;
			}
			maxv += basevertex;
			if (maxv < 0 || maxv >= (1 << 26)) {
				gb_cl_gl_error(maxv < 0 ? 0x0502 : 0x0505);
				return 1;
			}
			nverts = (uint32_t)maxv + 1;
			bound_off = (uint32_t)eoff;
			from_ebo = 1;
			goto attrs;
		}
		if (count < 0 || (is_instanced && instances <= 0))
			return 0; /* let the driver report it */
		if (count == 0)
			return 0; /* the driver still validates state */
		int ib = itype == 0x1401 ? 1 : itype == 0x1403 ? 2
				   : itype == 0x1405 ? 4 : 0;
		if (!ib) {
			gb_cl_gl_error(0x0500 /* INVALID_ENUM */);
			return 1;
		}
		if (!indices) {
			gb_cl_gl_error(0x0502);
			return 1;
		}
		int64_t maxv = -1, rs = restart_value(st, itype);
		int skipped = 0;
		/* The indices are application memory: not necessarily aligned,
		 * and not necessarily readable (an invalid call is rejected by
		 * the driver before it looks at them, so tests pass junk). */
		union { uint8_t b[256]; uint32_t w[64]; } ch;
		int per = (int)sizeof ch.b / ib;

		for (int32_t i = 0; i < count; i++) {
			int64_t x;
			int k = i % per;

			if (k == 0) {
				int32_t n = count - i < per ? count - i : per;

				if (gb_cl_copy_in(ch.b, (const uint8_t *)indices +
						  (size_t)i * ib,
						  (size_t)n * ib)) {
					/* unreadable: leave it to the driver
					 * if the call is invalid anyway */
					if (mode > 0xE)
						return 0;
					gb_cl_gl_error(0x0502);
					return 1;
				}
			}
			if (ib == 1) {
				x = ch.b[k];
			} else if (ib == 2) {
				uint16_t t;

				memcpy(&t, ch.b + 2 * k, 2);
				x = t;
			} else {
				uint32_t t;

				memcpy(&t, ch.b + 4 * k, 4);
				x = t;
			}
			if (x == rs) { /* restart marker, not a vertex */
				skipped = 1;
				continue;
			}
			x += basevertex;
			if (x > maxv)
				maxv = x;
		}
		if (maxv < 0 && skipped)
			maxv = 0;
		if (maxv < 0 || maxv >= (1 << 26)) {
			gb_cl_gl_error(maxv < 0 ? 0x0502 : 0x0505);
			return 1;
		}
		nverts = (uint32_t)maxv + 1;
		ibytes = (uint32_t)count * (uint32_t)ib;
	} else {
		if (!nclient)
			return 0;
		if (count <= 0 || first < 0 || (is_instanced && instances <= 0))
			return 0;
		nverts = (uint32_t)count;
	}

attrs:;
	uint32_t na = 0;
	for (uint32_t s = 0; s < CA_COUNT; s++) {
		const struct gb_carray *a = &v->a[s];
		if (!is_client_array(a))
			continue;
		int eb = gb_attrib_elem_bytes(a->type, a->size);
		if (!a->ptr || eb <= 0 || a->size < 1 ||
		    (a->size > 4 && a->size != 0x80E1) || a->stride < 0) {
			if (getenv("GB_TRACE_OPS"))
				fprintf(stderr, "c: client array slot %u "
					"unusable: ptr=%p type=%#x size=%d "
					"stride=%d buffer=%u\n", s, a->ptr,
					a->type, a->size, a->stride,
					a->buffer);
			gb_cl_gl_error(0x0502);
			return 1;
		}
		int64_t stride = a->stride ? a->stride : eb;
		uint64_t elems;
		const uint8_t *src = a->ptr;
		if (a->divisor == 0) {
			elems = nverts;
			if (!indexed)
				src += (int64_t)first * stride;
		} else {
			/* a plain draw is one instance of an instanced draw */
			elems = (uint64_t)((is_instanced ? instances : 1) - 1) /
					a->divisor + baseinstance + 1;
		}
		uint64_t len = (elems - 1) * (uint64_t)stride + eb;
		if (len > GB_MAX_ARRAY_BYTES || elems > (1u << 26)) {
			gb_cl_gl_error(0x0505 /* OUT_OF_MEMORY */);
			return 1;
		}
		at[na].slot = s;
		at[na].size = (uint32_t)a->size;
		at[na].type = a->type;
		at[na].stride = (uint32_t)a->stride;
		at[na].flags = (a->normalized ? GB_CA_NORMALIZED : 0) |
			       (a->integer ? GB_CA_INTEGER : 0) |
			       (a->is_long ? GB_CA_LONG : 0);
		at[na].divisor = a->divisor;
		at[na].len = (uint32_t)len;
		at[na].src = src;
		na++;
	}

	uint32_t sz = 16 + 60 + 28 * na;
	if (ibytes)
		sz = GB_ALIGN_UP(sz, 8u) + ibytes;
	for (uint32_t i = 0; i < na; i++)
		sz = GB_ALIGN_UP(sz, 8u) + at[i].len;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_DrawClient, sz, 0))
		return 1;
	gb_w_u32(&w, mode);
	gb_w_u32(&w, from_ebo ? 2u : indexed ? 1u : 0u);
	gb_w_u32(&w, (uint32_t)count);
	gb_w_u32(&w, itype);
	gb_w_u32(&w, nverts);
	gb_w_u32(&w, ibytes);
	gb_w_u32(&w, na);
	gb_w_u32(&w, is_instanced ? (uint32_t)instances : 0);
	gb_w_u32(&w, (uint32_t)basevertex);
	gb_w_u32(&w, baseinstance);
	/* bindings the server restores afterwards (so it need not query them) */
	gb_w_u32(&w, st->array_buffer);
	gb_w_u32(&w, v->element_buffer);
	gb_w_u32(&w, bound_off);
	{ /* primitive-restart index (-1: none), exempt from the range check */
		int64_t rs = indexed ? restart_value(st, itype) : -1;

		gb_w_u32(&w, (uint32_t)((uint64_t)rs & 0xffffffffu));
		gb_w_u32(&w, (uint32_t)((uint64_t)rs >> 32));
	}
	for (uint32_t i = 0; i < na; i++) {
		gb_w_u32(&w, at[i].slot);
		gb_w_u32(&w, at[i].size);
		gb_w_u32(&w, at[i].type);
		gb_w_u32(&w, at[i].stride);
		gb_w_u32(&w, at[i].flags);
		gb_w_u32(&w, at[i].divisor);
		gb_w_u32(&w, at[i].len);
	}
	if (ibytes)
		memcpy(gb_w_blob(&w, ibytes), indices, ibytes);
	for (uint32_t i = 0; i < na; i++)
		memcpy(gb_w_blob(&w, at[i].len), at[i].src, at[i].len);
	gb_cl_end(cx, &o);
	return 1;
}

/* ============================================================================
 * Public entry points that exist in both libraries
 * ========================================================================== */

#define GL_ARRAY_BUFFER_ 0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ 0x8893
#define GL_PIXEL_PACK_BUFFER_ 0x88EB
#define GL_PIXEL_UNPACK_BUFFER_ 0x88EC
#define GL_NUM_EXTENSIONS_ 0x821D

GB_API GLenum glGetError(void)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && cx->gl_error) {
		GLenum e = cx->gl_error;
		cx->gl_error = 0;
		return e;
	}
	return gbx_glGetError();
}

/*
 * Fixed-function client array state (size / type / stride / buffer binding).
 * Client-memory arrays never reach the driver until a draw, so the driver's
 * own state does not reflect the application's glXxxPointer calls; answer
 * these queries from the shadow. Returns 1 and the value when pname is one.
 */
static int array_state_query(GLenum pname, int64_t *out)
{
	enum { SIZE, TYPE, STRIDE, BUF };
	int slot, what;
	struct gb_ctxstate *st = gb_cur_state();

	switch (pname) {
	case 0x807A: slot = CA_VERTEX;    what = SIZE;   break;
	case 0x807B: slot = CA_VERTEX;    what = TYPE;   break;
	case 0x807C: slot = CA_VERTEX;    what = STRIDE; break;
	case 0x807E: slot = CA_NORMAL;    what = TYPE;   break;
	case 0x807F: slot = CA_NORMAL;    what = STRIDE; break;
	case 0x8081: slot = CA_COLOR;     what = SIZE;   break;
	case 0x8082: slot = CA_COLOR;     what = TYPE;   break;
	case 0x8083: slot = CA_COLOR;     what = STRIDE; break;
	case 0x8085: slot = CA_INDEX;     what = TYPE;   break;
	case 0x8086: slot = CA_INDEX;     what = STRIDE; break;
	case 0x8088: slot = -1;           what = SIZE;   break; /* TEXCOORD */
	case 0x8089: slot = -1;           what = TYPE;   break;
	case 0x808A: slot = -1;           what = STRIDE; break;
	case 0x808C: slot = CA_EDGEFLAG;  what = STRIDE; break;
	case 0x8454: slot = CA_FOGCOORD;  what = TYPE;   break;
	case 0x8455: slot = CA_FOGCOORD;  what = STRIDE; break;
	case 0x845A: slot = CA_SECONDARY; what = SIZE;   break;
	case 0x845B: slot = CA_SECONDARY; what = TYPE;   break;
	case 0x845C: slot = CA_SECONDARY; what = STRIDE; break;
	case 0x8896: slot = CA_VERTEX;    what = BUF;    break;
	case 0x8897: slot = CA_NORMAL;    what = BUF;    break;
	case 0x8898: slot = CA_COLOR;     what = BUF;    break;
	case 0x8899: slot = CA_INDEX;     what = BUF;    break;
	case 0x889A: slot = -1;           what = BUF;    break;
	case 0x889B: slot = CA_EDGEFLAG;  what = BUF;    break;
	case 0x889C: slot = CA_SECONDARY; what = BUF;    break;
	case 0x889D: slot = CA_FOGCOORD;  what = BUF;    break;
	default:
		return 0;
	}
	if (!st || ctx_is_core(st))
		return 0;
	if (slot < 0) /* texture coordinate array of the client-active unit */
		slot = CA_TEX0 + (int)(st->client_active_tex % GB_MAX_TEXUNITS);

	const struct gb_carray *a = &st->vao->a[slot];
	int is_set = a->type != 0;

	/* Only arrays in client memory are unknown to the driver. Buffer-backed
	 * arrays (and everything set through the direct-state-access calls,
	 * which the shadow does not follow) are answered by the driver. */
	if (!is_set || a->buffer != 0)
		return 0;

	switch (what) {
	case SIZE:
		*out = is_set ? a->size : 4;
		break;
	case TYPE:
		*out = is_set ? (int64_t)a->type : 0x1406 /* FLOAT */;
		break;
	case STRIDE:
		*out = is_set ? a->stride : 0;
		break;
	default:
		*out = a->buffer;
		break;
	}
	return 1;
}

GB_API void glGetIntegerv(GLenum pname, GLint *data)
{
	int64_t v;

	if (pname == GL_NUM_EXTENSIONS_ && data) {
		const struct gb_extlist *l = gb_cl_ext_list();
		if (l) {
			*data = l->n;
			return;
		}
	}
	if (data && array_state_query(pname, &v)) {
		*data = (GLint)v;
		return;
	}
	gbx_glGetIntegerv(pname, data);
}

GB_API void glGetInteger64v(GLenum pname, GLint64 *data)
{
	int64_t v;

	if (data && array_state_query(pname, &v)) {
		*data = v;
		return;
	}
	gbx_glGetInteger64v(pname, data);
}

GB_API void glGetFloatv(GLenum pname, GLfloat *data)
{
	int64_t v;

	if (data && array_state_query(pname, &v)) {
		*data = (GLfloat)v;
		return;
	}
	gbx_glGetFloatv(pname, data);
}

GB_API void glGetDoublev(GLenum pname, GLdouble *data)
{
	int64_t v;

	if (data && array_state_query(pname, &v)) {
		*data = (GLdouble)v;
		return;
	}
	gbx_glGetDoublev(pname, data);
}

GB_API void glGetBooleanv(GLenum pname, GLboolean *data)
{
	int64_t v;

	if (data && array_state_query(pname, &v)) {
		*data = v != 0;
		return;
	}
	gbx_glGetBooleanv(pname, data);
}

/* Generic buffer targets other than the four with dedicated fields. */
static int target_slot(uint32_t target)
{
	switch (target) {
	case 0x8F36: return 0;   /* COPY_READ */
	case 0x8F37: return 1;   /* COPY_WRITE */
	case 0x8A11: return 2;   /* UNIFORM */
	case 0x8C8E: return 3;   /* TRANSFORM_FEEDBACK */
	case 0x90D2: return 4;   /* SHADER_STORAGE */
	case 0x8C2A: return 5;   /* TEXTURE */
	case 0x8F3F: return 6;   /* DRAW_INDIRECT */
	case 0x90EE: return 7;   /* DISPATCH_INDIRECT */
	case 0x92C0: return 8;   /* ATOMIC_COUNTER */
	case 0x9192: return 9;   /* QUERY */
	case 0x80EE: return 10;  /* PARAMETER */
	default: return -1;
	}
}

uint32_t gb_state_bound(struct gb_ctxstate *st, uint32_t target)
{
	switch (target) {
	case GL_ARRAY_BUFFER_: return st->array_buffer;
	case GL_ELEMENT_ARRAY_BUFFER_: return st->vao->element_buffer;
	case GL_PIXEL_PACK_BUFFER_: return st->pack_buffer;
	case GL_PIXEL_UNPACK_BUFFER_: return st->unpack_buffer;
	default: {
		int s = target_slot(target);
		return s >= 0 ? st->bound[s] : 0;
	}
	}
}

void gb_state_bind(struct gb_ctxstate *st, uint32_t target, uint32_t buffer)
{
	switch (target) {
	case GL_ARRAY_BUFFER_: st->array_buffer = buffer; return;
	case GL_ELEMENT_ARRAY_BUFFER_: st->vao->element_buffer = buffer; return;
	case GL_PIXEL_PACK_BUFFER_: st->pack_buffer = buffer; return;
	case GL_PIXEL_UNPACK_BUFFER_: st->unpack_buffer = buffer; return;
	default: {
		int s = target_slot(target);
		if (s >= 0)
			st->bound[s] = buffer;
	}
	}
}

struct gb_map *gb_state_map(struct gb_ctxstate *st, uint32_t buffer)
{
	for (int i = 0; i < GB_MAX_MAPS; i++)
		if (st->maps[i].buffer && st->maps[i].buffer == buffer)
			return &st->maps[i];
	return NULL;
}

static void bind_buffer(GLenum target, GLuint buffer)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st)
		gb_state_bind(st, target, buffer);
}

static void delete_buffers(GLsizei n, const GLuint *buffers)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (!st || !buffers || n <= 0)
		return;
	for (GLsizei i = 0; i < n; i++) {
		GLuint b = buffers[i];
		if (!b)
			continue;
		/* deleting a mapped buffer unmaps it */
		for (int m = 0; m < GB_MAX_MAPS; m++)
			if (st->maps[m].buffer == b) {
				free(st->maps[m].alloc);
				memset(&st->maps[m], 0, sizeof(st->maps[m]));
			}
		if (st->array_buffer == b)
			st->array_buffer = 0;
		if (st->pack_buffer == b)
			st->pack_buffer = 0;
		if (st->unpack_buffer == b)
			st->unpack_buffer = 0;
		if (st->vao->element_buffer == b)
			st->vao->element_buffer = 0;
		for (int t = 0; t < GB_BUF_TARGETS; t++)
			if (st->bound[t] == b)
				st->bound[t] = 0;
		for (int a = 0; a < CA_COUNT; a++)
			if (st->vao->a[a].buffer == b)
				st->vao->a[a].buffer = 0;
	}
}

GB_API void glBindBuffer(GLenum target, GLuint buffer)
{
	bind_buffer(target, buffer);
	gbx_glBindBuffer(target, buffer);
}

GB_API void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
	delete_buffers(n, buffers);
	gbx_glDeleteBuffers(n, buffers);
}

GB_API void glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	bind_buffer(target, buffer);
	gbx_glBindBufferBase(target, index, buffer);
}

GB_API void glBindBufferRange(GLenum target, GLuint index, GLuint buffer,
			      GLintptr offset, GLsizeiptr size)
{
	bind_buffer(target, buffer);
	gbx_glBindBufferRange(target, index, buffer, offset, size);
}

GB_API void glPixelStorei(GLenum pname, GLint param)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st) {
		int ok_align = param == 1 || param == 2 || param == 4 || param == 8;
		int ok = param >= 0;
		switch (pname) {
		case 0x0CF5: if (ok_align) st->unpack.alignment = param; break;
		case 0x0D05: if (ok_align) st->pack.alignment = param; break;
		case 0x0CF2: if (ok) st->unpack.row_length = param; break;
		case 0x0D02: if (ok) st->pack.row_length = param; break;
		case 0x0CF4: if (ok) st->unpack.skip_pixels = param; break;
		case 0x0D04: if (ok) st->pack.skip_pixels = param; break;
		case 0x0CF3: if (ok) st->unpack.skip_rows = param; break;
		case 0x0D03: if (ok) st->pack.skip_rows = param; break;
		case 0x806E: if (ok) st->unpack.image_height = param; break;
		case 0x806C: if (ok) st->pack.image_height = param; break;
		case 0x806D: if (ok) st->unpack.skip_images = param; break;
		case 0x806B: if (ok) st->pack.skip_images = param; break;
		}
	}
	gbx_glPixelStorei(pname, param);
}

GB_API void glBindVertexArray(GLuint array)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st) {
		struct gb_vao *v = vao_get(st, array, 1);
		if (v)
			st->vao = v;
	}
	gbx_glBindVertexArray(array);
}

GB_API void glDeleteVertexArrays(GLsizei n, const GLuint *arrays)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && arrays)
		for (GLsizei i = 0; i < n; i++)
			vao_free(st, arrays[i]);
	gbx_glDeleteVertexArrays(n, arrays);
}

GB_API void glEnableVertexAttribArray(GLuint index)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && index < GB_MAX_GENERIC)
		gb_arr_enable(st, CA_GENERIC0 + (int)index, 1);
	gbx_glEnableVertexAttribArray(index);
}

GB_API void glDisableVertexAttribArray(GLuint index)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && index < GB_MAX_GENERIC)
		gb_arr_enable(st, CA_GENERIC0 + (int)index, 0);
	gbx_glDisableVertexAttribArray(index);
}

GB_API void glVertexAttribDivisor(GLuint index, GLuint divisor)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && index < GB_MAX_GENERIC)
		st->vao->a[CA_GENERIC0 + index].divisor = divisor;
	gbx_glVertexAttribDivisor(index, divisor);
}

/* Record a pointer call; send it only if it is a buffer offset. */
#define GENERIC_POINTER(idx_) \
	((idx_) < GB_MAX_GENERIC ? CA_GENERIC0 + (int)(idx_) : -1)

GB_API void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
				  GLboolean normalized, GLsizei stride,
				  const void *pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && GENERIC_POINTER(index) >= 0) {
		int r_ = gb_arr_pointer(st, GENERIC_POINTER(index), size, type, normalized,
			   0, 0, stride, pointer);

		if (r_ == 1)
			return;
		if (r_ == 2)
			pointer = NULL;
	}
	gbx_glVertexAttribPointer(index, size, type, normalized, stride,
				  pointer);
}

GB_API void glVertexAttribIPointer(GLuint index, GLint size, GLenum type,
				   GLsizei stride, const void *pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && GENERIC_POINTER(index) >= 0) {
		int r_ = gb_arr_pointer(st, GENERIC_POINTER(index), size, type, 0, 1, 0,
			   stride, pointer);

		if (r_ == 1)
			return;
		if (r_ == 2)
			pointer = NULL;
	}
	gbx_glVertexAttribIPointer(index, size, type, stride, pointer);
}

GB_API void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 0, first, count, 0, NULL, 0, 0, 0, 0))
		return;
	gbx_glDrawArrays(mode, first, count);
}

GB_API void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
				  GLsizei instancecount)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 0, first, count, 0, NULL,
				 instancecount, 1, 0, 0))
		return;
	gbx_glDrawArraysInstanced(mode, first, count, instancecount);
}

GB_API void glDrawElements(GLenum mode, GLsizei count, GLenum type,
			   const void *indices)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices, 0, 0, 0, 0))
		return;
	gbx_glDrawElements(mode, count, type, indices);
}

GB_API void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
				    const void *indices, GLsizei instancecount)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices,
				 instancecount, 1, 0, 0))
		return;
	gbx_glDrawElementsInstanced(mode, count, type, indices, instancecount);
}

/* glDrawRangeElements* carry start/end only as a hint once the draw is turned
 * into a plain indexed draw, so the one error they add (end < start) is
 * raised here, after the enum errors the driver would report first. */
static int range_invalid(GLenum mode, GLuint start, GLuint end, GLsizei count,
			 GLenum type)
{
	int mode_ok = mode <= 0xE; /* POINTS .. PATCHES */
	int type_ok = type == 0x1401 || type == 0x1403 || type == 0x1405;

	if (mode_ok && type_ok && count >= 0 && end < start) {
		gb_cl_gl_error(0x0501);
		return 1;
	}
	return 0;
}

GB_API void glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
				GLsizei count, GLenum type, const void *indices)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (range_invalid(mode, start, end, count, type))
		return;
	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices, 0, 0, 0, 0))
		return;
	gbx_glDrawRangeElements(mode, start, end, count, type, indices);
}

/* OES_vertex_array_object spells these differently. */
GB_API void glBindVertexArrayOES(GLuint array)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st) {
		struct gb_vao *v = vao_get(st, array, 1);
		if (v)
			st->vao = v;
	}
	gbx_glBindVertexArrayOES(array);
}

GB_API void glDeleteVertexArraysOES(GLsizei n, const GLuint *arrays)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && arrays)
		for (GLsizei i = 0; i < n; i++)
			vao_free(st, arrays[i]);
	gbx_glDeleteVertexArraysOES(n, arrays);
}


/* ============================================================================
 * Desktop GL only: fixed-function arrays, ARB/EXT aliases, immediate-mode
 * emulation of glArrayElement, glInterleavedArrays, client attrib stack.
 * ========================================================================== */

#define GL_VERTEX_ARRAY_ 0x8074
#define GL_NORMAL_ARRAY_ 0x8075
#define GL_COLOR_ARRAY_ 0x8076
#define GL_INDEX_ARRAY_ 0x8077
#define GL_TEXTURE_COORD_ARRAY_ 0x8078
#define GL_EDGE_FLAG_ARRAY_ 0x8079
#define GL_FOG_COORD_ARRAY_ 0x8457
#define GL_SECONDARY_COLOR_ARRAY_ 0x845E

/* NV_vertex_program attribute aliasing: attribute index -> array slot */
static int nv_attrib_slot(unsigned i)
{
	switch (i) {
	case 0: return CA_VERTEX;
	case 1: return CA_WEIGHT;
	case 2: return CA_NORMAL;
	case 3: return CA_COLOR;
	case 4: return CA_SECONDARY;
	case 5: return CA_FOGCOORD;
	case 6: return CA_POINTSIZE;
	default:
		return i >= 8 && i < 8 + GB_MAX_TEXUNITS ? CA_TEX0 + (int)(i - 8)
							 : -1;
	}
}

static int cap_slot(struct gb_ctxstate *st, GLenum cap)
{
	switch (cap) {
	case GL_VERTEX_ARRAY_: return CA_VERTEX;
	case GL_NORMAL_ARRAY_: return CA_NORMAL;
	case GL_COLOR_ARRAY_: return CA_COLOR;
	case GL_INDEX_ARRAY_: return CA_INDEX;
	case GL_EDGE_FLAG_ARRAY_: return CA_EDGEFLAG;
	case GL_FOG_COORD_ARRAY_: return CA_FOGCOORD;
	case GL_SECONDARY_COLOR_ARRAY_: return CA_SECONDARY;
	case 0x8B9C: return CA_POINTSIZE;    /* POINT_SIZE_ARRAY_OES */
	case 0x8844: return CA_MATRIXINDEX;  /* MATRIX_INDEX_ARRAY_OES/ARB */
	case 0x86AD: return CA_WEIGHT;       /* WEIGHT_ARRAY_OES/ARB */
	case GL_TEXTURE_COORD_ARRAY_:
		return CA_TEX0 + (int)(st->client_active_tex % GB_MAX_TEXUNITS);
	default:
		/* NV_vertex_program: glEnable(VERTEX_ATTRIB_ARRAYi_NV) */
		if (cap >= 0x8650 && cap <= 0x865F)
			return nv_attrib_slot(cap - 0x8650);
		return -1;
	}
}

GB_API void glEnableClientState(GLenum array)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st) {
		int s = cap_slot(st, array);
		if (s >= 0)
			gb_arr_enable(st, s, 1);
		else if (array == 0x8558) /* PRIMITIVE_RESTART_NV */
			st->restart_on = 1;
	}
	gbx_glEnableClientState(array);
}

GB_API void glDisableClientState(GLenum array)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st) {
		int s = cap_slot(st, array);
		if (s >= 0)
			gb_arr_enable(st, s, 0);
		else if (array == 0x8558) /* PRIMITIVE_RESTART_NV */
			st->restart_on = 0;
	}
	gbx_glDisableClientState(array);
}

/* The array enables are also accepted by glEnable/glDisable (GL 1.1). */
GB_API void glEnable(GLenum cap)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st && cap == 0x8242 /* DEBUG_OUTPUT_SYNCHRONOUS */) {
		st->debug_sync = 1;
		gb_cl_debug_update_sync(gb_cl_get());
	}

	if (st) { /* glEnable/glDisable also accept the array caps */
		int s = cap_slot(st, cap);

		if (s >= 0)
			gb_arr_enable(st, s, 1);
		else if (cap == 0x8558 || cap == 0x8F9D) /* PRIMITIVE_RESTART */
			st->restart_on = 1;
		else if (cap == 0x8D69) /* PRIMITIVE_RESTART_FIXED_INDEX */
			st->restart_fixed = 1;
	}
	gbx_glEnable(cap);
}

GB_API void glDisable(GLenum cap)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st && cap == 0x8242 /* DEBUG_OUTPUT_SYNCHRONOUS */) {
		st->debug_sync = 0;
		gb_cl_debug_update_sync(gb_cl_get());
	}

	if (st) { /* glEnable/glDisable also accept the array caps */
		int s = cap_slot(st, cap);

		if (s >= 0)
			gb_arr_enable(st, s, 0);
		else if (cap == 0x8558 || cap == 0x8F9D) /* PRIMITIVE_RESTART */
			st->restart_on = 0;
		else if (cap == 0x8D69) /* PRIMITIVE_RESTART_FIXED_INDEX */
			st->restart_fixed = 0;
	}
	gbx_glDisable(cap);
}

GB_API void glPrimitiveRestartIndex(GLuint index)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st)
		st->restart_index = index;
	gbx_glPrimitiveRestartIndex(index);
}

GB_API void glPrimitiveRestartIndexNV(GLuint index)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st)
		st->restart_index = index;
	gbx_glPrimitiveRestartIndexNV(index);
}

GB_API void glClientActiveTexture(GLenum texture)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && texture >= 0x84C0 && texture < 0x84C0 + GB_MAX_TEXUNITS)
		st->client_active_tex = texture - 0x84C0;
	gbx_glClientActiveTexture(texture);
}

GB_API void glClientActiveTextureARB(GLenum texture)
{
	glClientActiveTexture(texture);
}

/* Fixed-function pointers: slot, then forward only if a buffer offset. */
#define FIXED_POINTER(slot_, size_, type_, stride_, ptr_, send_)           \
	do {                                                               \
		struct gb_ctxstate *st_ = gb_cur_state();                  \
		if (st_) {                                                 \
			int r_ = gb_arr_pointer(st_, (slot_), (size_),     \
						(type_), 0, 0, 0,          \
						(stride_), (ptr_));        \
			if (r_ == 1)                                       \
				return;                                    \
			if (r_ == 2)                                       \
				(ptr_) = NULL;                             \
		}                                                          \
		send_;                                                     \
	} while (0)

GB_API void glVertexPointer(GLint size, GLenum type, GLsizei stride,
			    const void *pointer)
{
	FIXED_POINTER(CA_VERTEX, size, type, stride, pointer,
		      gbx_glVertexPointer(size, type, stride, pointer));
}

GB_API void glNormalPointer(GLenum type, GLsizei stride, const void *pointer)
{
	FIXED_POINTER(CA_NORMAL, 3, type, stride, pointer,
		      gbx_glNormalPointer(type, stride, pointer));
}

GB_API void glColorPointer(GLint size, GLenum type, GLsizei stride,
			   const void *pointer)
{
	FIXED_POINTER(CA_COLOR, size, type, stride, pointer,
		      gbx_glColorPointer(size, type, stride, pointer));
}

GB_API void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride,
				    const void *pointer)
{
	FIXED_POINTER(CA_SECONDARY, size, type, stride, pointer,
		      gbx_glSecondaryColorPointer(size, type, stride, pointer));
}

GB_API void glFogCoordPointer(GLenum type, GLsizei stride, const void *pointer)
{
	FIXED_POINTER(CA_FOGCOORD, 1, type, stride, pointer,
		      gbx_glFogCoordPointer(type, stride, pointer));
}

GB_API void glIndexPointer(GLenum type, GLsizei stride, const void *pointer)
{
	FIXED_POINTER(CA_INDEX, 1, type, stride, pointer,
		      gbx_glIndexPointer(type, stride, pointer));
}

GB_API void glPointSizePointerOES(GLenum type, GLsizei stride,
				  const void *pointer)
{
	FIXED_POINTER(CA_POINTSIZE, 1, type, stride, pointer,
		      gbx_glPointSizePointerOES(type, stride, pointer));
}

GB_API void glMatrixIndexPointerOES(GLint size, GLenum type, GLsizei stride,
				    const void *pointer)
{
	FIXED_POINTER(CA_MATRIXINDEX, size, type, stride, pointer,
		      gbx_glMatrixIndexPointerOES(size, type, stride, pointer));
}

GB_API void glMatrixIndexPointerARB(GLint size, GLenum type, GLsizei stride,
				    const void *pointer)
{
	FIXED_POINTER(CA_MATRIXINDEX, size, type, stride, pointer,
		      gbx_glMatrixIndexPointerARB(size, type, stride, pointer));
}

GB_API void glWeightPointerOES(GLint size, GLenum type, GLsizei stride,
			       const void *pointer)
{
	FIXED_POINTER(CA_WEIGHT, size, type, stride, pointer,
		      gbx_glWeightPointerOES(size, type, stride, pointer));
}

GB_API void glWeightPointerARB(GLint size, GLenum type, GLsizei stride,
			       const void *pointer)
{
	FIXED_POINTER(CA_WEIGHT, size, type, stride, pointer,
		      gbx_glWeightPointerARB(size, type, stride, pointer));
}

/* NV_vertex_program: attribute `index` aliases the conventional arrays */
GB_API void glVertexAttribPointerNV(GLuint index, GLint fsize, GLenum type,
				    GLsizei stride, const void *pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	int slot = nv_attrib_slot(index);

	if (st && slot >= 0) {
		int r_ = gb_arr_pointer(st, slot, fsize, type, 0, 0, 0, stride, pointer);

		if (r_ == 1)
			return;
		if (r_ == 2)
			pointer = NULL;
	}
	gbx_glVertexAttribPointerNV(index, fsize, type, stride, pointer);
}

GB_API void glGetVertexAttribPointervNV(GLuint index, GLenum pname,
					void **pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	int slot = nv_attrib_slot(index);

	if (!pointer)
		return;
	if (pname != 0x8645 /* VERTEX_ATTRIB_ARRAY_POINTER_NV */ || slot < 0 ||
	    !st) {
		gb_cl_gl_error(pname != 0x8645 ? 0x0500 : 0x0501);
		*pointer = NULL;
		return;
	}
	*pointer = (void *)st->vao->a[slot].ptr;
}

GB_API void glEdgeFlagPointer(GLsizei stride, const void *pointer)
{
	FIXED_POINTER(CA_EDGEFLAG, 1, 0x1401, stride, pointer,
		      gbx_glEdgeFlagPointer(stride, pointer));
}

GB_API void glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
			      const void *pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	int slot = CA_TEX0 + (st ? (int)(st->client_active_tex %
					 GB_MAX_TEXUNITS) : 0);
	FIXED_POINTER(slot, size, type, stride, pointer,
		      gbx_glTexCoordPointer(size, type, stride, pointer));
}

GB_API void glVertexAttribLPointer(GLuint index, GLint size, GLenum type,
				   GLsizei stride, const void *pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && GENERIC_POINTER(index) >= 0) {
		int r_ = gb_arr_pointer(st, GENERIC_POINTER(index), size, type, 0, 0, 1,
			   stride, pointer);

		if (r_ == 1)
			return;
		if (r_ == 2)
			pointer = NULL;
	}
	gbx_glVertexAttribLPointer(index, size, type, stride, pointer);
}

GB_API void glVertexAttribPointerARB(GLuint index, GLint size, GLenum type,
				     GLboolean normalized, GLsizei stride,
				     const void *pointer)
{
	glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

GB_API void glVertexAttribIPointerEXT(GLuint index, GLint size, GLenum type,
				      GLsizei stride, const void *pointer)
{
	glVertexAttribIPointer(index, size, type, stride, pointer);
}

GB_API void glVertexAttribLPointerEXT(GLuint index, GLint size, GLenum type,
				      GLsizei stride, const void *pointer)
{
	glVertexAttribLPointer(index, size, type, stride, pointer);
}

/* ARB/EXT spellings of the buffer/array-state calls. */
GB_API void glBindBufferARB(GLenum target, GLuint buffer)
{
	bind_buffer(target, buffer);
	gbx_glBindBufferARB(target, buffer);
}

GB_API void glDeleteBuffersARB(GLsizei n, const GLuint *buffers)
{
	delete_buffers(n, buffers);
	gbx_glDeleteBuffersARB(n, buffers);
}

GB_API void glEnableVertexAttribArrayARB(GLuint index)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && index < GB_MAX_GENERIC)
		gb_arr_enable(st, CA_GENERIC0 + (int)index, 1);
	gbx_glEnableVertexAttribArrayARB(index);
}

GB_API void glDisableVertexAttribArrayARB(GLuint index)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && index < GB_MAX_GENERIC)
		gb_arr_enable(st, CA_GENERIC0 + (int)index, 0);
	gbx_glDisableVertexAttribArrayARB(index);
}

GB_API void glVertexAttribDivisorARB(GLuint index, GLuint divisor)
{
	struct gb_ctxstate *st = gb_cur_state();
	if (st && index < GB_MAX_GENERIC)
		st->vao->a[CA_GENERIC0 + index].divisor = divisor;
	gbx_glVertexAttribDivisorARB(index, divisor);
}

/* More draw spellings / variants. */
GB_API void glDrawArraysInstancedARB(GLenum mode, GLint first, GLsizei count,
				     GLsizei primcount)
{
	glDrawArraysInstanced(mode, first, count, primcount);
}

GB_API void glDrawArraysInstancedEXT(GLenum mode, GLint start, GLsizei count,
				     GLsizei primcount)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 0, start, count, 0, NULL, primcount,
				 1, 0, 0))
		return;
	gbx_glDrawArraysInstancedEXT(mode, start, count, primcount);
}

GB_API void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first,
					      GLsizei count,
					      GLsizei instancecount,
					      GLuint baseinstance)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 0, first, count, 0, NULL,
				 instancecount, 1, 0, baseinstance))
		return;
	gbx_glDrawArraysInstancedBaseInstance(mode, first, count,
					      instancecount, baseinstance);
}

GB_API void glDrawElementsInstancedARB(GLenum mode, GLsizei count, GLenum type,
				       const void *indices, GLsizei primcount)
{
	glDrawElementsInstanced(mode, count, type, indices, primcount);
}

GB_API void glDrawElementsInstancedEXT(GLenum mode, GLsizei count, GLenum type,
				       const void *indices, GLsizei primcount)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices, primcount,
				 1, 0, 0))
		return;
	gbx_glDrawElementsInstancedEXT(mode, count, type, indices, primcount);
}

GB_API void glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end,
				   GLsizei count, GLenum type,
				   const void *indices)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (range_invalid(mode, start, end, count, type))
		return;
	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices, 0, 0, 0, 0))
		return;
	gbx_glDrawRangeElementsEXT(mode, start, end, count, type, indices);
}

GB_API void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
				     const void *indices, GLint basevertex)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices, 0, 0,
				 basevertex, 0))
		return;
	gbx_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

GB_API void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start,
					  GLuint end, GLsizei count,
					  GLenum type, const void *indices,
					  GLint basevertex)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (range_invalid(mode, start, end, count, type))
		return;
	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices, 0, 0,
				 basevertex, 0))
		return;
	gbx_glDrawRangeElementsBaseVertex(mode, start, end, count, type,
					  indices, basevertex);
}

GB_API void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count,
					      GLenum type, const void *indices,
					      GLsizei instancecount,
					      GLint basevertex)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices,
				 instancecount, 1, basevertex, 0))
		return;
	gbx_glDrawElementsInstancedBaseVertex(mode, count, type, indices,
					      instancecount, basevertex);
}

GB_API void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count,
						GLenum type,
						const void *indices,
						GLsizei instancecount,
						GLuint baseinstance)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices,
				 instancecount, 1, 0, baseinstance))
		return;
	gbx_glDrawElementsInstancedBaseInstance(mode, count, type, indices,
						instancecount, baseinstance);
}

GB_API void glDrawElementsInstancedBaseVertexBaseInstance(
	GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLsizei instancecount, GLint basevertex, GLuint baseinstance)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && gb_draw_client(st, mode, 1, 0, count, type, indices,
				 instancecount, 1, basevertex, baseinstance))
		return;
	gbx_glDrawElementsInstancedBaseVertexBaseInstance(
		mode, count, type, indices, instancecount, basevertex,
		baseinstance);
}

/* ---- indirect draws -------------------------------------------------------
 * With a buffer bound to GL_DRAW_INDIRECT_BUFFER the argument is an offset
 * and goes to the server as is.  The compatibility profile also accepts a
 * pointer to client memory there; the server cannot read that, so the command
 * structures are fetched here and the draws issued one by one as instanced
 * base-instance draws (which run through the client-array machinery too).
 * The structures are copied out with memcpy: client memory is not
 * necessarily 4-byte aligned. */
#define GB_DRAW_INDIRECT_BUFFER 0x8F3F

static int indirect_stride_ok(struct gb_ctxstate *st, GLsizei drawcount,
			      GLsizei stride, GLsizei natural, GLsizei *out)
{
	if (st && st->in_begin) {
		gb_cl_gl_error(0x0502);
		return 0;
	}
	if (drawcount < 0 || stride < 0 || (stride & 3)) {
		gb_cl_gl_error(0x0501);
		return 0;
	}
	*out = stride ? stride : natural;
	return 1;
}

static void indirect_arrays_client(GLenum mode, const char *p,
				   GLsizei drawcount, GLsizei stride)
{
	for (GLsizei i = 0; i < drawcount; i++) {
		uint32_t c[4];

		memcpy(c, p + (size_t)i * stride, sizeof c);
		glDrawArraysInstancedBaseInstance(mode, (GLint)c[2],
						  (GLsizei)c[0],
						  (GLsizei)c[1], c[3]);
	}
}

static void indirect_elements_client(struct gb_ctxstate *st, GLenum mode,
				     GLenum type, const char *p,
				     GLsizei drawcount, GLsizei stride)
{
	size_t isz;

	switch (type) {
	case 0x1401: isz = 1; break;
	case 0x1403: isz = 2; break;
	case 0x1405: isz = 4; break;
	default:
		gb_cl_gl_error(0x0500);
		return;
	}
	/* The index array must live in a buffer: indices are offsets into it. */
	if (!st->vao->element_buffer) {
		gb_cl_gl_error(0x0502);
		return;
	}
	for (GLsizei i = 0; i < drawcount; i++) {
		uint32_t c[5];

		memcpy(c, p + (size_t)i * stride, sizeof c);
		glDrawElementsInstancedBaseVertexBaseInstance(
			mode, (GLsizei)c[0], type,
			(const void *)(uintptr_t)(c[2] * isz), (GLsizei)c[1],
			(GLint)c[3], c[4]);
	}
}

/* Vertex arrays in client memory with the commands in a buffer: the number of
 * vertices to upload depends on buffer contents the client cannot see, so
 * the command structures are read back and the draws issued one by one.
 * Returns 1 when the draw was handled. */
static int indirect_expand_buffer(struct gb_ctxstate *st, GLenum mode,
				  GLenum type, int indexed,
				  const void *indirect, GLsizei drawcount,
				  GLsizei stride)
{
	struct gb_vao *v = st->vao;
	int client = 0;

	for (int i = 0; i < CA_COUNT; i++)
		if (is_client_array(&v->a[i]))
			client = 1;
	if (!client)
		return 0;
	if (st->in_begin) {
		gb_cl_gl_error(0x0502);
		return 1;
	}
	GLsizei natural = indexed ? 20 : 16, str;

	if (!indirect_stride_ok(st, drawcount, stride, natural, &str))
		return 1;
	if (drawcount == 0)
		return 1;
	uint64_t len = (uint64_t)(drawcount - 1) * (uint64_t)str + natural;

	if (len > (64u << 20)) {
		gb_cl_gl_error(0x0505);
		return 1;
	}
	uint8_t *tmp = calloc(1, (size_t)len);

	if (!tmp) {
		gb_cl_gl_error(0x0505);
		return 1;
	}
	glGetBufferSubData(GB_DRAW_INDIRECT_BUFFER, (GLintptr)(uintptr_t)indirect,
			   (GLsizeiptr)len, tmp);
	if (indexed)
		indirect_elements_client(st, mode, type, (const char *)tmp,
					 drawcount, str);
	else
		indirect_arrays_client(mode, (const char *)tmp, drawcount, str);
	free(tmp);
	return 1;
}

GB_API void glDrawArraysIndirect(GLenum mode, const void *indirect)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	GLsizei stride;

	if (!st || gb_state_bound(st, GB_DRAW_INDIRECT_BUFFER)) {
		if (!st || !indirect_expand_buffer(st, mode, 0, 0, indirect, 1,
						   0))
			gbx_glDrawArraysIndirect(mode, indirect);
		return;
	}
	if (gb_cl_es3_context(st)) { /* ES has no client-memory command structures */
		gb_cl_gl_error(0x0502);
		return;
	}
	if (!indirect_stride_ok(st, 1, 0, 16, &stride) || !indirect)
		return;
	indirect_arrays_client(mode, indirect, 1, stride);
}

GB_API void glDrawElementsIndirect(GLenum mode, GLenum type,
				   const void *indirect)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	GLsizei stride;

	if (!st || gb_state_bound(st, GB_DRAW_INDIRECT_BUFFER)) {
		if (!st || !indirect_expand_buffer(st, mode, type, 1, indirect,
						   1, 0))
			gbx_glDrawElementsIndirect(mode, type, indirect);
		return;
	}
	if (gb_cl_es3_context(st)) {
		gb_cl_gl_error(0x0502);
		return;
	}
	if (!indirect_stride_ok(st, 1, 0, 20, &stride) || !indirect)
		return;
	indirect_elements_client(st, mode, type, indirect, 1, stride);
}

GB_API void glMultiDrawArraysIndirect(GLenum mode, const void *indirect,
				      GLsizei drawcount, GLsizei stride)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (!st || gb_state_bound(st, GB_DRAW_INDIRECT_BUFFER)) {
		if (!st || !indirect_expand_buffer(st, mode, 0, 0, indirect,
						   drawcount, stride))
			gbx_glMultiDrawArraysIndirect(mode, indirect, drawcount,
						      stride);
		return;
	}
	if (gb_cl_es3_context(st)) {
		gb_cl_gl_error(0x0502);
		return;
	}
	if (!indirect_stride_ok(st, drawcount, stride, 16, &stride) ||
	    !indirect || !drawcount)
		return;
	indirect_arrays_client(mode, indirect, drawcount, stride);
}

GB_API void glMultiDrawElementsIndirect(GLenum mode, GLenum type,
					const void *indirect,
					GLsizei drawcount, GLsizei stride)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (!st || gb_state_bound(st, GB_DRAW_INDIRECT_BUFFER)) {
		if (!st || !indirect_expand_buffer(st, mode, type, 1, indirect,
						   drawcount, stride))
			gbx_glMultiDrawElementsIndirect(mode, type, indirect,
							drawcount, stride);
		return;
	}
	if (gb_cl_es3_context(st)) {
		gb_cl_gl_error(0x0502);
		return;
	}
	if (!indirect_stride_ok(st, drawcount, stride, 20, &stride) ||
	    !indirect || !drawcount)
		return;
	indirect_elements_client(st, mode, type, indirect, drawcount, stride);
}

/* The *Count variants read the draw count from the parameter buffer, and
 * require the command buffer too: client memory is an error. */
static int indirect_count_ok(struct gb_ctxstate *st)
{
	if (st && st->in_begin) {
		gb_cl_gl_error(0x0502);
		return 0;
	}
	if (st && !gb_state_bound(st, GB_DRAW_INDIRECT_BUFFER)) {
		gb_cl_gl_error(0x0502);
		return 0;
	}
	return 1;
}

GB_API void glMultiDrawArraysIndirectCount(GLenum mode, const void *indirect,
					   GLintptr drawcount,
					   GLsizei maxdrawcount, GLsizei stride)
{
	if (indirect_count_ok(gb_cur_state_raw()))
		gbx_glMultiDrawArraysIndirectCount(mode, indirect, drawcount,
						   maxdrawcount, stride);
}

GB_API void glMultiDrawElementsIndirectCount(GLenum mode, GLenum type,
					     const void *indirect,
					     GLintptr drawcount,
					     GLsizei maxdrawcount,
					     GLsizei stride)
{
	if (indirect_count_ok(gb_cur_state_raw()))
		gbx_glMultiDrawElementsIndirectCount(mode, type, indirect,
						     drawcount, maxdrawcount,
						     stride);
}

GB_API void glMultiDrawArraysIndirectCountARB(GLenum mode,
					      const void *indirect,
					      GLintptr drawcount,
					      GLsizei maxdrawcount,
					      GLsizei stride)
{
	glMultiDrawArraysIndirectCount(mode, indirect, drawcount, maxdrawcount,
				       stride);
}

GB_API void glMultiDrawElementsIndirectCountARB(GLenum mode, GLenum type,
						const void *indirect,
						GLintptr drawcount,
						GLsizei maxdrawcount,
						GLsizei stride)
{
	glMultiDrawElementsIndirectCount(mode, type, indirect, drawcount,
					 maxdrawcount, stride);
}

/* A multi-draw with a negative primcount or any negative count is an
 * INVALID_VALUE error that draws nothing (not "draw the valid ones"). */
static int multidraw_counts_ok(GLsizei drawcount, const GLsizei *count)
{
	if (drawcount < 0) {
		gb_cl_gl_error(0x0501);
		return 0;
	}
	for (GLsizei i = 0; count && i < drawcount; i++)
		if (count[i] < 0) {
			gb_cl_gl_error(0x0501);
			return 0;
		}
	return 1;
}

GB_API void glMultiDrawArrays(GLenum mode, const GLint *first,
			      const GLsizei *count, GLsizei drawcount)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (st && !multidraw_counts_ok(drawcount, count))
		return;
	if (st && drawcount > 0 && first && count) {
		int client = 0;
		for (int i = 0; i < CA_COUNT; i++)
			if (is_client_array(&st->vao->a[i]))
				client = 1;
		if (client) {
			for (GLsizei i = 0; i < drawcount; i++)
				glDrawArrays(mode, first[i], count[i]);
			return;
		}
	}
	gbx_glMultiDrawArrays(mode, first, count, drawcount);
}

GB_API void glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
				const void *const *indices, GLsizei drawcount)
{
	if (!multidraw_counts_ok(drawcount, count))
		return;
	if (!count || !indices)
		return;
	for (GLsizei i = 0; i < drawcount; i++)
		glDrawElements(mode, count[i], type, indices[i]);
}

GB_API void glMultiDrawElementsEXT(GLenum mode, const GLsizei *count,
				   GLenum type, const void *const *indices,
				   GLsizei drawcount)
{
	glMultiDrawElements(mode, count, type, indices, drawcount);
}

GB_API void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count,
					  GLenum type,
					  const void *const *indices,
					  GLsizei drawcount,
					  const GLint *basevertex)
{
	if (!multidraw_counts_ok(drawcount, count))
		return;
	if (!count || !indices || !basevertex)
		return;
	for (GLsizei i = 0; i < drawcount; i++)
		glDrawElementsBaseVertex(mode, count[i], type, indices[i],
					 basevertex[i]);
}

/* Ask the driver for an array pointer the client does not shadow (a buffer
 * offset, or state set through the direct-state-access calls). */
static void *driver_pointer(uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	void *p = NULL;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_GetPointer, 32,
				   GB_CMD_F_WANT_REPLY))
		return NULL;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, a);
	gb_w_u32(&w, b);
	gb_w_u32(&w, c);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_GetPointer))
		return NULL;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint64_t v = gb_r_u64(&r);
	if (!r.err)
		p = (void *)(uintptr_t)v;
	gb_in_release(&in);
	return p;
}

GB_API void glGetVertexAttribPointerv(GLuint index, GLenum pname,
				      void **pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (pname != 0x8645 /* VERTEX_ATTRIB_ARRAY_POINTER */) {
		gb_cl_gl_error(0x0500);
		return;
	}
	if (st && !st->max_attribs) {
		GLint m = 0;

		gbx_glGetIntegerv(0x8869 /* MAX_VERTEX_ATTRIBS */, &m);
		st->max_attribs = m > 0 ? m : GB_MAX_GENERIC;
	}
	if (index >= GB_MAX_GENERIC || !pointer ||
	    (st && index >= (GLuint)st->max_attribs)) {
		gb_cl_gl_error(0x0501);
		return;
	}
	if (st && client_pointer(&st->vao->a[CA_GENERIC0 + index]))
		*pointer = (void *)st->vao->a[CA_GENERIC0 + index].ptr;
	else
		*pointer = driver_pointer(3, index, pname, 0);
}

GB_API void glGetVertexAttribPointervARB(GLuint index, GLenum pname,
					 void **pointer)
{
	glGetVertexAttribPointerv(index, pname, pointer);
}

/* ---- glGetPointerv --------------------------------------------------------------------- */

GB_API void glGetPointerv(GLenum pname, void **params)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	int slot = -1;

	if (!params)
		return;
	if (st && st->in_begin) {
		gb_cl_gl_error(0x0502);
		return;
	}
	switch (pname) {
	case 0x808E: slot = CA_VERTEX; break;
	case 0x808F: slot = CA_NORMAL; break;
	case 0x8090: slot = CA_COLOR; break;
	case 0x8091: slot = CA_INDEX; break;
	case 0x8092:
		slot = CA_TEX0 + (st ? (int)(st->client_active_tex %
					     GB_MAX_TEXUNITS) : 0);
		break;
	case 0x8093: slot = CA_EDGEFLAG; break;
	case 0x8456: slot = CA_FOGCOORD; break;
	case 0x845D: slot = CA_SECONDARY; break;
	case 0x8244: /* DEBUG_CALLBACK_FUNCTION (KHR/ARB) */
		*params = st ? (void *)st->debug_cb : NULL;
		return;
	case 0x8245: /* DEBUG_CALLBACK_USER_PARAM */
		*params = st ? (void *)st->debug_user : NULL;
		return;
	default:
		gb_cl_gl_error(0x0500);
		*params = NULL;
		return;
	}
	if (st && client_pointer(&st->vao->a[slot]))
		*params = (void *)st->vao->a[slot].ptr;
	else
		*params = driver_pointer(0, pname, 0, 0);
}

/* ---- EXT_direct_state_access client-array entry points ---------------------------------- */

/* pointer enum (+ index) -> array slot, -1 if not a pointer query */
static int pointer_slot(GLenum pname, GLuint index)
{
	switch (pname) {
	case 0x808E: return CA_VERTEX;
	case 0x808F: return CA_NORMAL;
	case 0x8090: return CA_COLOR;
	case 0x8091: return CA_INDEX;
	case 0x8092: return index < GB_MAX_TEXUNITS ? CA_TEX0 + (int)index : -1;
	case 0x8093: return CA_EDGEFLAG;
	case 0x8456: return CA_FOGCOORD;
	case 0x845D: return CA_SECONDARY;
	case 0x8645: /* VERTEX_ATTRIB_ARRAY_POINTER */
		return index < GB_MAX_GENERIC ? CA_GENERIC0 + (int)index : -1;
	default: return -1;
	}
}

GB_API void glGetPointeri_vEXT(GLenum pname, GLuint index, void **params)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	/* the indexed query only knows the per-unit / per-attribute pointers */
	int slot = (pname == 0x8092 || pname == 0x8645)
			   ? pointer_slot(pname, index) : -1;

	if (!params)
		return;
	if (slot < 0 || !st) {
		gb_cl_gl_error(0x0500);
		*params = NULL;
		return;
	}
	if (client_pointer(&st->vao->a[slot]))
		*params = (void *)st->vao->a[slot].ptr;
	else
		*params = driver_pointer(1, pname, index, 0);
}

GB_API void glGetPointerIndexedvEXT(GLenum pname, GLuint index, void **params)
{
	glGetPointeri_vEXT(pname, index, params);
}

GB_API void glGetVertexArrayPointeri_vEXT(GLuint vaobj, GLuint index,
					  GLenum pname, void **param)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	struct gb_vao *v = st ? vao_get(st, vaobj, 0) : NULL;
	int slot = pointer_slot(pname, index);

	if (!param)
		return;
	if (slot < 0) {
		gb_cl_gl_error(0x0500);
		*param = NULL;
		return;
	}
	if (v && client_pointer(&v->a[slot]))
		*param = (void *)v->a[slot].ptr;
	else
		*param = driver_pointer(2, vaobj, index, pname);
}

GB_API void glGetVertexArrayPointervEXT(GLuint vaobj, GLenum pname,
					void **param)
{
	glGetVertexArrayPointeri_vEXT(vaobj, 0, pname, param);
}

GB_API void glMultiTexCoordPointerEXT(GLenum texunit, GLint size, GLenum type,
				      GLsizei stride, const void *pointer)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	GLuint unit = texunit - 0x84C0;

	if (st && unit < GB_MAX_TEXUNITS) {
		int r_ = gb_arr_pointer(st, CA_TEX0 + (int)unit, size, type, 0, 0, 0, stride,
			   pointer);

		if (r_ == 1)
			return;
		if (r_ == 2)
			pointer = NULL;
	}
	gbx_glMultiTexCoordPointerEXT(texunit, size, type, stride, pointer);
}

/* ---- legacy multi-draw extensions the registry does not list ---------------------------- */

GB_API void glBlendEquationSeparateATI(GLenum modeRGB, GLenum modeA)
{
	glBlendEquationSeparate(modeRGB, modeA);
}

GB_API void glMultiDrawArraysSUN(GLenum mode, GLint *first, GLsizei *count,
				 GLsizei primcount)
{
	glMultiDrawArrays(mode, first, count, primcount);
}

GB_API void glMultiDrawElementsSUN(GLenum mode, const GLsizei *count,
				   GLenum type, const void *const *indices,
				   GLsizei primcount)
{
	glMultiDrawElements(mode, count, type, indices, primcount);
}

/* IBM_multimode_draw_arrays: one primitive mode per draw, `modestride` bytes
 * apart in the mode array */
GB_API void glMultiModeDrawArraysIBM(const GLenum *mode, const GLint *first,
				     const GLsizei *count, GLsizei primcount,
				     GLint modestride)
{
	if (primcount < 0 || modestride < 0) {
		gb_cl_gl_error(0x0501);
		return;
	}
	if (!mode || !first || !count)
		return;
	for (GLsizei i = 0; i < primcount; i++)
		glDrawArrays(*(const GLenum *)((const char *)mode +
					       (size_t)i * (size_t)modestride),
			     first[i], count[i]);
}

GB_API void glMultiModeDrawElementsIBM(const GLenum *mode, const GLsizei *count,
				       GLenum type, const void *const *indices,
				       GLsizei primcount, GLint modestride)
{
	if (primcount < 0 || modestride < 0) {
		gb_cl_gl_error(0x0501);
		return;
	}
	if (!mode || !count || !indices)
		return;
	for (GLsizei i = 0; i < primcount; i++)
		glDrawElements(*(const GLenum *)((const char *)mode +
						 (size_t)i * (size_t)modestride),
			       count[i], type, indices[i]);
}

/* ---- EXT_vertex_array pointer spellings (extra `count`, unused here) ------------------- */

GB_API void glVertexPointerEXT(GLint size, GLenum type, GLsizei stride,
			       GLsizei count, const void *pointer)
{
	glVertexPointer(size, type, stride, pointer);
}

GB_API void glNormalPointerEXT(GLenum type, GLsizei stride, GLsizei count,
			       const void *pointer)
{
	glNormalPointer(type, stride, pointer);
}

GB_API void glColorPointerEXT(GLint size, GLenum type, GLsizei stride,
			      GLsizei count, const void *pointer)
{
	glColorPointer(size, type, stride, pointer);
}

GB_API void glIndexPointerEXT(GLenum type, GLsizei stride, GLsizei count,
			      const void *pointer)
{
	glIndexPointer(type, stride, pointer);
}

GB_API void glTexCoordPointerEXT(GLint size, GLenum type, GLsizei stride,
				 GLsizei count, const void *pointer)
{
	glTexCoordPointer(size, type, stride, pointer);
}

GB_API void glEdgeFlagPointerEXT(GLsizei stride, GLsizei count,
				 const GLboolean *pointer)
{
	glEdgeFlagPointer(stride, pointer);
}

/* ---- selection / feedback ------------------------------------------------------------ */

#define GB_RM_FEEDBACK 0x1C01
#define GB_RM_SELECT 0x1C02

static void select_send(uint32_t kind, int32_t size, uint32_t type)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_SelectBuffer, 32, 0))
		return;
	gb_w_u32(&w, kind);
	gb_w_u32(&w, (uint32_t)size);
	gb_w_u32(&w, type);
	gb_cl_end(cx, &o);
}

GB_API void glSelectBuffer(GLsizei size, GLuint *buffer)
{
	struct gb_ctxstate *st = gb_cur_state();

	/* the server's own buffer is what the driver writes; the driver raises
	 * any error (negative size, select mode active) */
	if (st && size >= 0 && st->render_mode != GB_RM_SELECT) {
		st->sel_ptr = buffer;
		st->sel_size = size;
	}
	select_send(0, size, 0);
}

GB_API void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st && size >= 0 && st->render_mode != GB_RM_FEEDBACK &&
	    type >= 0x0600 && type <= 0x0604 /* GL_2D .. GL_4D_COLOR_TEXTURE */) {
		st->fb_ptr = buffer;
		st->fb_size = size;
	}
	select_send(1, size, type);
}

GB_API GLint glRenderMode(GLenum mode)
{
	struct gb_ctxstate *st = gb_cur_state();
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	GLint ret = 0;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_RenderMode, 32,
				   GB_CMD_F_WANT_REPLY))
		return 0;
	gb_w_u32(&w, mode);
	gb_w_u32(&w, st ? st->render_mode : 0);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_RenderMode))
		return 0;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	ret = (GLint)gb_r_u32(&r);
	uint32_t newmode = gb_r_u32(&r), n = gb_r_u32(&r);
	const void *words = n ? gb_r_blob(&r, 4u * n) : NULL;

	if (getenv("GB_TRACE_OPS")) {
		fprintf(stderr, "glRenderMode(%#x) prev=%#x -> ret=%d new=%#x "
			"words=%u:", mode, st ? st->render_mode : 0, ret,
			newmode, n);
		for (uint32_t i = 0; words && i < n && i < 24; i++)
			fprintf(stderr, " %u", ((const uint32_t *)words)[i]);
		fprintf(stderr, "\n");
	}

	if (!r.err && st) {
		uint32_t prev = st->render_mode;
		/* the finished mode's results land in the app's buffer */
		if (words && prev == GB_RM_SELECT && st->sel_ptr)
			memcpy(st->sel_ptr, words,
			       4u * (n < (uint32_t)st->sel_size
					     ? n : (uint32_t)st->sel_size));
		else if (words && prev == GB_RM_FEEDBACK && st->fb_ptr)
			memcpy(st->fb_ptr, words,
			       4u * (n < (uint32_t)st->fb_size
					     ? n : (uint32_t)st->fb_size));
		st->render_mode = newmode == 0x1C00 ? 0 : newmode;
	}
	gb_in_release(&in);
	return ret;
}

/* ---- glInterleavedArrays ----------------------------------------------------------------- */

GB_API void glInterleavedArrays(GLenum format, GLsizei stride,
				const void *pointer)
{
	/* per the OpenGL 2.1 spec table 2.5 */
	struct {
		GLenum fmt;
		int st, sc, sv;      /* texcoord size, color size, vertex size */
		int has_c, has_n;
		int c_ubyte;         /* color is 4 x UNSIGNED_BYTE */
	} f[] = {
		{ 0x2A20, 0, 0, 2, 0, 0, 0 }, /* V2F */
		{ 0x2A21, 0, 0, 3, 0, 0, 0 }, /* V3F */
		{ 0x2A22, 0, 4, 2, 1, 0, 1 }, /* C4UB_V2F */
		{ 0x2A23, 0, 4, 3, 1, 0, 1 }, /* C4UB_V3F */
		{ 0x2A24, 0, 3, 3, 1, 0, 0 }, /* C3F_V3F */
		{ 0x2A25, 0, 0, 3, 0, 1, 0 }, /* N3F_V3F */
		{ 0x2A26, 0, 4, 3, 1, 1, 0 }, /* C4F_N3F_V3F */
		{ 0x2A27, 2, 0, 3, 0, 0, 0 }, /* T2F_V3F */
		{ 0x2A28, 4, 0, 4, 0, 0, 0 }, /* T4F_V4F */
		{ 0x2A29, 2, 4, 3, 1, 0, 1 }, /* T2F_C4UB_V3F */
		{ 0x2A2A, 2, 3, 3, 1, 0, 0 }, /* T2F_C3F_V3F */
		{ 0x2A2B, 2, 0, 3, 0, 1, 0 }, /* T2F_N3F_V3F */
		{ 0x2A2C, 2, 4, 3, 1, 1, 0 }, /* T2F_C4F_N3F_V3F */
		{ 0x2A2D, 4, 4, 4, 1, 1, 0 }, /* T4F_C4F_N3F_V4F */
	};
	const struct { GLenum fmt; int st, sc, sv, has_c, has_n, c_ubyte; } *e =
		NULL;

	for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++)
		if (f[i].fmt == format)
			e = (const void *)&f[i];
	if (!e || stride < 0) {
		gb_cl_gl_error(e ? 0x0501 : 0x0500);
		return;
	}
	const int F = 4;
	int off = 0, pt = 0, pc = 0, pn = 0, pv;
	if (e->st) {
		pt = off;
		off += e->st * F;
	}
	if (e->has_c) {
		pc = off;
		off += e->c_ubyte ? 4 : e->sc * F;
	}
	if (e->has_n) {
		pn = off;
		off += 3 * F;
	}
	pv = off;
	off += e->sv * F;
	GLsizei s = stride ? stride : (GLsizei)off;
	const char *base = pointer;

	glDisableClientState(GL_EDGE_FLAG_ARRAY_);
	glDisableClientState(GL_INDEX_ARRAY_);
	glDisableClientState(GL_FOG_COORD_ARRAY_);
	glDisableClientState(GL_SECONDARY_COLOR_ARRAY_);
	if (e->st) {
		glEnableClientState(GL_TEXTURE_COORD_ARRAY_);
		glTexCoordPointer(e->st, 0x1406, s, base + pt);
	} else {
		glDisableClientState(GL_TEXTURE_COORD_ARRAY_);
	}
	if (e->has_c) {
		glEnableClientState(GL_COLOR_ARRAY_);
		glColorPointer(e->c_ubyte ? 4 : e->sc,
			       e->c_ubyte ? 0x1401 : 0x1406, s, base + pc);
	} else {
		glDisableClientState(GL_COLOR_ARRAY_);
	}
	if (e->has_n) {
		glEnableClientState(GL_NORMAL_ARRAY_);
		glNormalPointer(0x1406, s, base + pn);
	} else {
		glDisableClientState(GL_NORMAL_ARRAY_);
	}
	glEnableClientState(GL_VERTEX_ARRAY_);
	glVertexPointer(e->sv, 0x1406, s, base + pv);
}

/* ---- glArrayElement: emulate with immediate-mode calls -------------------------------------- */

static const uint8_t *elem(const struct gb_carray *a, uint32_t i, int eb)
{
	int64_t stride = a->stride ? a->stride : eb;
	return (const uint8_t *)a->ptr + (int64_t)i * stride;
}

GB_API void glArrayElement(GLint i)
{
	/* valid between glBegin and glEnd, so the raw state is needed */
	struct gb_ctxstate *st = gb_cur_state_raw();
	const struct gb_carray *a;
	int eb;

	/* NV_primitive_restart: the restart index restarts the primitive
	 * instead of fetching a vertex */
	if (st && st->restart_on && (uint32_t)i == st->restart_index) {
		glPrimitiveRestartNV();
		return;
	}
	if (!st || i < 0) {
		gbx_glArrayElement(i);
		return;
	}
	struct gb_vao *v = st->vao;
	int any_client = 0, any_buffer = 0;
	for (int s = 0; s < CA_COUNT; s++)
		if (v->a[s].enabled) {
			if (v->a[s].buffer)
				any_buffer = 1;
			else
				any_client = 1;
		}
	if (!any_client || any_buffer) {
		/* Nothing we can emulate, or mixed: the driver has the buffer
		 * contents; client arrays in the mix are not supported. */
		if (any_client)
			gb_cl_unsupported("glArrayElement mixing client and "
					  "buffer arrays");
		gbx_glArrayElement(i);
		return;
	}

#define EL(slot_) (a = &v->a[slot_], a->enabled ? ((eb = \
	gb_attrib_elem_bytes(a->type, a->size)) > 0) : 0)
	/* edge flag, texcoords, color, secondary, fog, index, normal, vertex */
	if (EL(CA_EDGEFLAG)) {
		glEdgeFlagv((const GLboolean *)elem(a, (uint32_t)i, 1));
	}
	for (int u = 0; u < GB_MAX_TEXUNITS; u++) {
		if (!EL(CA_TEX0 + u))
			continue;
		const void *p = elem(a, (uint32_t)i, eb);
		GLenum t = 0x84C0 + (GLenum)u;
		switch (a->type) {
		case 0x1406:
			a->size == 1 ? glMultiTexCoord1fv(t, p) :
			a->size == 2 ? glMultiTexCoord2fv(t, p) :
			a->size == 3 ? glMultiTexCoord3fv(t, p) :
				       glMultiTexCoord4fv(t, p);
			break;
		case 0x140A:
			a->size == 1 ? glMultiTexCoord1dv(t, p) :
			a->size == 2 ? glMultiTexCoord2dv(t, p) :
			a->size == 3 ? glMultiTexCoord3dv(t, p) :
				       glMultiTexCoord4dv(t, p);
			break;
		case 0x1404:
			a->size == 1 ? glMultiTexCoord1iv(t, p) :
			a->size == 2 ? glMultiTexCoord2iv(t, p) :
			a->size == 3 ? glMultiTexCoord3iv(t, p) :
				       glMultiTexCoord4iv(t, p);
			break;
		case 0x1402:
			a->size == 1 ? glMultiTexCoord1sv(t, p) :
			a->size == 2 ? glMultiTexCoord2sv(t, p) :
			a->size == 3 ? glMultiTexCoord3sv(t, p) :
				       glMultiTexCoord4sv(t, p);
			break;
		}
	}
	if (EL(CA_COLOR)) {
		const void *p = elem(a, (uint32_t)i, eb);
		int c4 = a->size == 4;
		switch (a->type) {
		case 0x1401: c4 ? glColor4ubv(p) : glColor3ubv(p); break;
		case 0x1400: c4 ? glColor4bv(p) : glColor3bv(p); break;
		case 0x1403: c4 ? glColor4usv(p) : glColor3usv(p); break;
		case 0x1402: c4 ? glColor4sv(p) : glColor3sv(p); break;
		case 0x1405: c4 ? glColor4uiv(p) : glColor3uiv(p); break;
		case 0x1404: c4 ? glColor4iv(p) : glColor3iv(p); break;
		case 0x1406: c4 ? glColor4fv(p) : glColor3fv(p); break;
		case 0x140A: c4 ? glColor4dv(p) : glColor3dv(p); break;
		}
	}
	if (EL(CA_SECONDARY)) {
		const void *p = elem(a, (uint32_t)i, eb);
		switch (a->type) {
		case 0x1401: glSecondaryColor3ubv(p); break;
		case 0x1406: glSecondaryColor3fv(p); break;
		case 0x140A: glSecondaryColor3dv(p); break;
		case 0x1404: glSecondaryColor3iv(p); break;
		case 0x1402: glSecondaryColor3sv(p); break;
		}
	}
	if (EL(CA_FOGCOORD)) {
		const void *p = elem(a, (uint32_t)i, eb);
		a->type == 0x140A ? glFogCoorddv(p) : glFogCoordfv(p);
	}
	if (EL(CA_INDEX)) {
		const void *p = elem(a, (uint32_t)i, eb);
		switch (a->type) {
		case 0x1401: glIndexubv(p); break;
		case 0x1402: glIndexsv(p); break;
		case 0x1404: glIndexiv(p); break;
		case 0x1406: glIndexfv(p); break;
		case 0x140A: glIndexdv(p); break;
		}
	}
	if (EL(CA_NORMAL)) {
		const void *p = elem(a, (uint32_t)i, eb);
		switch (a->type) {
		case 0x1400: glNormal3bv(p); break;
		case 0x1402: glNormal3sv(p); break;
		case 0x1404: glNormal3iv(p); break;
		case 0x1406: glNormal3fv(p); break;
		case 0x140A: glNormal3dv(p); break;
		}
	}
	if (EL(CA_VERTEX)) {
		const void *p = elem(a, (uint32_t)i, eb);
		int n = a->size;
		switch (a->type) {
		case 0x1406:
			n == 2 ? glVertex2fv(p) : n == 3 ? glVertex3fv(p) :
							   glVertex4fv(p);
			break;
		case 0x140A:
			n == 2 ? glVertex2dv(p) : n == 3 ? glVertex3dv(p) :
							   glVertex4dv(p);
			break;
		case 0x1404:
			n == 2 ? glVertex2iv(p) : n == 3 ? glVertex3iv(p) :
							   glVertex4iv(p);
			break;
		case 0x1402:
			n == 2 ? glVertex2sv(p) : n == 3 ? glVertex3sv(p) :
							   glVertex4sv(p);
			break;
		}
	}
#undef EL
}

/* ---- glPushClientAttrib / glPopClientAttrib ---------------------------------------------- */

GB_API void glPushClientAttrib(GLbitfield mask)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st && st->stack_depth < GB_CLIENT_ATTRIB_STACK) {
		struct gb_attrib_save *sv = calloc(1, sizeof(*sv));
		if (sv) {
			sv->mask = mask;
			memcpy(sv->a, st->vao->a, sizeof(sv->a));
			sv->element_buffer = st->vao->element_buffer;
			sv->array_buffer = st->array_buffer;
			sv->client_active_tex = st->client_active_tex;
			sv->pack = st->pack;
			sv->unpack = st->unpack;
			st->stack[st->stack_depth++] = sv;
		}
	}
	gbx_glPushClientAttrib(mask);
}

GB_API void glPopClientAttrib(void)
{
	struct gb_ctxstate *st = gb_cur_state();

	if (st && st->stack_depth > 0) {
		struct gb_attrib_save *sv = st->stack[--st->stack_depth];
		if (sv->mask & 0x2) { /* CLIENT_VERTEX_ARRAY_BIT */
			memcpy(st->vao->a, sv->a, sizeof(sv->a));
			st->vao->element_buffer = sv->element_buffer;
			st->array_buffer = sv->array_buffer;
			st->client_active_tex = sv->client_active_tex;
		}
		if (sv->mask & 0x1) { /* CLIENT_PIXEL_STORE_BIT */
			st->pack = sv->pack;
			st->unpack = sv->unpack;
		}
		free(sv);
	}
	gbx_glPopClientAttrib();
}

/* ---- EXT_memory_object_fd / EXT_semaphore_fd ---------------------------------------------- */

/* The fd goes to the server as a token; on success the driver owns the
 * server's copy. */
GB_API void glImportMemoryFdEXT(GLuint memory, GLuint64 size, GLenum handleType,
				GLint fd)
{
	gbx_glImportMemoryFdEXT(memory, size, handleType,
				(GLint)gb_cl_fd_put(fd));
}

GB_API void glImportSemaphoreFdEXT(GLuint semaphore, GLenum handleType, GLint fd)
{
	gbx_glImportSemaphoreFdEXT(semaphore, handleType,
				   (GLint)gb_cl_fd_put(fd));
}

/* ---- NV_command_list: client-memory command streams ------------------------------------ */

GB_API void glListDrawCommandsStatesClientNV(GLuint list, GLuint segment,
					     const void **indirects,
					     const GLsizei *sizes,
					     const GLuint *states,
					     const GLuint *fbos, GLuint count)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint64_t total = 0;

	if (!cx || count > 4096 || (count && (!indirects || !sizes || !states ||
					      !fbos)))
		return;
	for (GLuint i = 0; i < count; i++) {
		if (sizes[i] < 0 || sizes[i] > (1 << 24) ||
		    (sizes[i] && !indirects[i])) {
			gb_cl_gl_error(0x0501);
			return;
		}
		total += GB_ALIGN_UP((uint32_t)sizes[i], 8u);
	}
	if (total > GB_MAX_ARRAY_BYTES) {
		gb_cl_gl_error(0x0505);
		return;
	}
	uint32_t sz = GB_ALIGN_UP(16 + 12 + 12 * count, 8u) + (uint32_t)total;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_ListDrawClient, sz, 0))
		return;
	gb_w_u32(&w, list);
	gb_w_u32(&w, segment);
	gb_w_u32(&w, count);
	for (GLuint i = 0; i < count; i++)
		gb_w_u32(&w, (uint32_t)sizes[i]);
	for (GLuint i = 0; i < count; i++)
		gb_w_u32(&w, states[i]);
	for (GLuint i = 0; i < count; i++)
		gb_w_u32(&w, fbos[i]);
	for (GLuint i = 0; i < count; i++)
		if (sizes[i])
			memcpy(gb_w_blob(&w, (uint32_t)sizes[i]), indirects[i],
			       (size_t)sizes[i]);
	gb_cl_end(cx, &o);
}

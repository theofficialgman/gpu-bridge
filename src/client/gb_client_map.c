/*
 * gb_client_map.c - mapped buffer objects (glMapBuffer*, glUnmapBuffer,
 * glFlushMappedBufferRange, glGet*BufferPointerv).
 *
 * The server cannot hand the application a pointer into GPU-visible memory
 * in another address space, so a mapping is a *shadow copy* on the client:
 *
 *   map     allocate `length` bytes; fetch the current contents from the
 *           server only if the application can observe them (READ) or
 *           partially overwrite them (WRITE without INVALIDATE/EXPLICIT)
 *   unmap   upload the shadow with glBufferSubData (write mappings)
 *   flush   with MAP_FLUSH_EXPLICIT_BIT, upload just the flushed range
 *
 * This preserves GL's mapping semantics for everything except *persistent*
 * mappings (GL_MAP_PERSISTENT_BIT), whose whole point is that the GPU and
 * the application share memory live; those are refused with an error
 * rather than silently going stale.
 */
#define _GNU_SOURCE
#include <stdio.h>

#include "gb_client_impl.h"

#define MAP_READ 0x0001
#define MAP_WRITE 0x0002
#define MAP_INVALIDATE_RANGE 0x0004
#define MAP_INVALIDATE_BUFFER 0x0008
#define MAP_FLUSH_EXPLICIT 0x0010
#define MAP_UNSYNCHRONIZED 0x0020
#define MAP_PERSISTENT 0x0040
#define MAP_COHERENT 0x0080

#define GL_BUFFER_SIZE_ 0x8764
#define GL_BUFFER_ACCESS_ 0x88BB
#define GL_BUFFER_MAPPED_ 0x88BC
#define GL_BUFFER_MAP_POINTER_ 0x88BD
#define GL_BUFFER_ACCESS_FLAGS_ 0x911F
#define GL_BUFFER_MAP_LENGTH_ 0x9120
#define GL_BUFFER_MAP_OFFSET_ 0x9121

/* ---- server round trips ---------------------------------------------------------- */

/* Read `len` bytes of a buffer (by target binding, or by name) into dst. */
static int buffer_read(int named, uint32_t id, int64_t off, int64_t len,
		       void *dst)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	int ok = 0;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, GB_OPX_BufferRead, 40,
				   GB_CMD_F_WANT_REPLY))
		return -1;
	gb_w_u32(&w, named ? 1u : 0u);
	gb_w_u32(&w, id);
	gb_w_u64(&w, (uint64_t)off);
	gb_w_u64(&w, (uint64_t)len);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_BufferRead))
		return -1;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t rl = gb_r_u32(&r);
	if (rl == (uint32_t)len) {
		const void *src = gb_r_blob(&r, rl);
		if (src && !r.err) {
			memcpy(dst, src, rl);
			ok = 1;
		}
	}
	gb_in_release(&in);
	return ok ? 0 : -1;
}

static void buffer_write(const struct gb_map *m, int64_t rel_off,
			 const void *p, int64_t n)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	int64_t off = m->offset + rel_off;

	if (n <= 0 || !cx)
		return;
	/* by mapping on the server: immutable storage without
	 * GL_DYNAMIC_STORAGE_BIT does not accept glBufferSubData */
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_BufferWrite,
			    GB_ALIGN_UP(40u, 8u) + (uint32_t)n, 0))
		return;
	gb_w_u32(&w, m->target ? 0u : 1u);
	gb_w_u32(&w, m->target ? m->target : m->buffer);
	gb_w_u64(&w, (uint64_t)off);
	gb_w_u64(&w, (uint64_t)n);
	memcpy(gb_w_blob(&w, (uint32_t)n), p, (size_t)n);
	gb_cl_end(cx, &o);
}

static int64_t buffer_size(uint32_t target, uint32_t named_buf)
{
	GLint v = 0;

	if (target) {
		glGetBufferParameteriv(target, GL_BUFFER_SIZE_, &v);
	} else {
#ifdef GB_LIB_GL
		glGetNamedBufferParameteriv(named_buf, GL_BUFFER_SIZE_, &v);
#endif
	}
	return v;
}

/* ---- the four operations --------------------------------------------------------------- */

static uint32_t legacy_access_to_flags(GLenum access)
{
	switch (access) {
	case 0x88B8: return MAP_READ;               /* READ_ONLY */
	case 0x88B9: return MAP_WRITE;              /* WRITE_ONLY */
	case 0x88BA: return MAP_READ | MAP_WRITE;   /* READ_WRITE */
	default: return 0;
	}
}

/* ---- persistent mappings ------------------------------------------------------ */

#define PERSIST_BLOCK 4096

/* Send what the application changed in a persistent mapping since the last
 * sync. Ranges of a FLUSH_EXPLICIT mapping go out at the flush instead. */
static void persist_up_one(struct gb_map *m)
{
	const uint8_t *sh = m->shadow;
	uint8_t *sn = m->snap;
	int64_t n = m->length;

	if (!sn || !(m->flags & MAP_WRITE) || (m->flags & MAP_FLUSH_EXPLICIT))
		return;
	for (int64_t o = 0; o < n;) {
		int64_t e = o + PERSIST_BLOCK < n ? o + PERSIST_BLOCK : n;

		if (!memcmp(sh + o, sn + o, (size_t)(e - o))) {
			o = e;
			continue;
		}
		int64_t s = o;

		/* extend the run over following changed blocks */
		o = e;
		while (o < n) {
			e = o + PERSIST_BLOCK < n ? o + PERSIST_BLOCK : n;
			if (!memcmp(sh + o, sn + o, (size_t)(e - o)))
				break;
			o = e;
		}
		buffer_write(m, s, sh + s, o - s);
		memcpy(sn + s, sh + s, (size_t)(o - s));
	}
}

/* Refresh the application's view of a persistent read mapping from the
 * server's buffer (the GPU may have written it). Blocks the application
 * itself changed since the last sync are left alone. */
static void persist_down_one(struct gb_map *m)
{
	uint8_t *sh = m->shadow, *sn = m->snap;
	int64_t n = m->length;
	uint8_t *fresh;

	if (!sn || !(m->flags & MAP_READ))
		return;
	fresh = malloc((size_t)n);
	if (!fresh)
		return;
	if (buffer_read(m->target == 0, m->target ? m->target : m->buffer,
			m->offset, n, fresh)) {
		free(fresh);
		return;
	}
	for (int64_t o = 0; o < n; o += PERSIST_BLOCK) {
		int64_t e = o + PERSIST_BLOCK < n ? o + PERSIST_BLOCK : n;

		if (!memcmp(sh + o, sn + o, (size_t)(e - o)))
			memcpy(sh + o, fresh + o, (size_t)(e - o));
		memcpy(sn + o, fresh + o, (size_t)(e - o));
	}
	free(fresh);
}

void gb_cl_persist_sync(struct gb_cl *cx, int what)
{
	struct gb_ctxstate *st = cx ? cx->cur : NULL;
	static __thread int busy; /* the uploads below are hooked commands too */

	if (!st || !st->npersist || busy)
		return;
	busy = 1;
	for (int i = 0; i < GB_MAX_MAPS; i++) {
		struct gb_map *m = &st->maps[i];

		if (!m->buffer || !(m->flags & MAP_PERSISTENT))
			continue;
		if (what & GB_PS_UP)
			persist_up_one(m);
	}
	if (what & GB_PS_DOWN)
		for (int i = 0; i < GB_MAX_MAPS; i++) {
			struct gb_map *m = &st->maps[i];

			if (m->buffer && (m->flags & MAP_PERSISTENT))
				persist_down_one(m);
		}
	busy = 0;
}

/* Buffer binding points. Anything else is GL_INVALID_ENUM, before any state
 * is consulted. */
static int target_valid(uint32_t t)
{
	switch (t) {
	case 0x8892: case 0x8893: case 0x88EB: case 0x88EC: case 0x8F36:
	case 0x8F37: case 0x8A11: case 0x8C8E: case 0x90D2: case 0x8C2A:
	case 0x8F3F: case 0x90EE: case 0x92C0: case 0x9192: case 0x80EE:
		return 1;
	default:
		return 0;
	}
}

/* target != 0: through a binding; else `named` is the buffer name.
 * whole != 0: legacy glMapBuffer (offset 0, length = buffer size). */
static void *do_map(uint32_t target, uint32_t named, int64_t offset,
		    int64_t length, uint32_t flags, int whole, int legacy)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	uint32_t buf = named;

	if (!st)
		return NULL;
	if (st->in_begin) {
		gb_cl_gl_error(0x0502);
		return NULL;
	}
	if (target && !target_valid(target)) {
		gb_cl_gl_error(0x0500);
		return NULL;
	}
	if (target)
		buf = gb_state_bound(st, target);
	if (!buf) {
		gb_cl_gl_error(0x0502); /* INVALID_OPERATION */
		return NULL;
	}
	if (legacy && !flags) {
		gb_cl_gl_error(0x0500); /* INVALID_ENUM */
		return NULL;
	}
	/* glMapBufferRange error rules. Value errors (range, unknown bits)
	 * come before state errors, as in the drivers. */
	int64_t size = buffer_size(target, named);
	if (whole) {
		offset = 0;
		length = size;
	}
	if (offset < 0 || length < 0 || (!whole && length == 0) ||
	    (!legacy && (flags & ~0xFFu)) || offset + length > size) {
		gb_cl_gl_error(0x0501);
		return NULL;
	}
	if (length > GB_MAX_ARRAY_BYTES) {
		gb_cl_gl_error(0x0505);
		return NULL;
	}
	if (!(flags & (MAP_READ | MAP_WRITE)) ||
	    ((flags & MAP_READ) &&
	     (flags & (MAP_INVALIDATE_RANGE | MAP_INVALIDATE_BUFFER |
		       MAP_UNSYNCHRONIZED))) ||
	    ((flags & MAP_FLUSH_EXPLICIT) && !(flags & MAP_WRITE))) {
		gb_cl_gl_error(0x0502); /* INVALID_OPERATION */
		return NULL;
	}
	if (gb_state_map(st, buf)) {
		gb_cl_gl_error(0x0502);
		return NULL;
	}
	if (flags & MAP_PERSISTENT) {
		/* the buffer must have been created with persistent storage */
		GLint sf = 0;

		if (target)
			glGetBufferParameteriv(target, 0x8220 /* STORAGE_FLAGS */,
					       &sf);
#ifdef GB_LIB_GL
		else
			glGetNamedBufferParameteriv(named, 0x8220, &sf);
#endif
		if (!(sf & MAP_PERSISTENT) ||
		    ((flags & MAP_READ) && !(sf & MAP_READ)) ||
		    ((flags & MAP_WRITE) && !(sf & MAP_WRITE))) {
			gb_cl_gl_error(0x0502);
			return NULL;
		}
	}

	struct gb_map *m = NULL;
	for (int i = 0; i < GB_MAX_MAPS && !m; i++)
		if (!st->maps[i].buffer)
			m = &st->maps[i];
	if (!m) {
		gb_cl_gl_error(0x0505);
		return NULL;
	}
	/* Mappings are aligned like the driver's: (pointer - offset) must be
	 * a multiple of GL_MIN_MAP_BUFFER_ALIGNMENT (at least 64). 256 covers
	 * every power-of-two the drivers report. */
	void *alloc = NULL;
	if (posix_memalign(&alloc, 256, (size_t)length + 256)) {
		gb_cl_gl_error(0x0505);
		return NULL;
	}
	memset(alloc, 0, (size_t)length + 256);
	void *shadow = (uint8_t *)alloc + (offset & 255);

	/* Contents are observable (READ) or only partly overwritten (plain
	 * WRITE): they must be current. INVALIDATE_* / FLUSH_EXPLICIT writes
	 * either don't care or are uploaded range by range. */
	int need_fetch = (flags & MAP_READ) ||
			 ((flags & MAP_WRITE) &&
			  !(flags & (MAP_INVALIDATE_RANGE |
				     MAP_INVALIDATE_BUFFER | MAP_FLUSH_EXPLICIT)));
	if (need_fetch &&
	    buffer_read(target == 0, target ? target : named, offset, length,
			shadow)) {
		free(alloc);
		gb_cl_gl_error(0x0505);
		return NULL;
	}

	m->buffer = buf;
	m->target = target;
	m->offset = offset;
	m->length = length;
	m->flags = flags;
	m->shadow = shadow;
	m->alloc = alloc;
	m->snap = NULL;
	if (flags & MAP_PERSISTENT) {
		m->snap = malloc((size_t)length);
		if (m->snap) {
			if (need_fetch)
				memcpy(m->snap, shadow, (size_t)length);
			else if (buffer_read(target == 0,
					     target ? target : named, offset,
					     length, m->snap))
				memset(m->snap, 0, (size_t)length);
			if (!need_fetch)
				memcpy(shadow, m->snap, (size_t)length);
			st->npersist++;
		} else {
			free(alloc);
			memset(m, 0, sizeof(*m));
			gb_cl_gl_error(0x0505);
			return NULL;
		}
	}
	return shadow;
}

static GLboolean do_unmap(uint32_t target, uint32_t named)
{
	struct gb_ctxstate *st = gb_cur_state_raw();
	uint32_t buf = named;
	struct gb_map *m;

	if (!st)
		return GL_FALSE;
	if (st->in_begin) {
		gb_cl_gl_error(0x0502);
		return GL_FALSE;
	}
	if (target && !target_valid(target)) {
		gb_cl_gl_error(0x0500);
		return GL_FALSE;
	}
	if (target)
		buf = gb_state_bound(st, target);
	m = buf ? gb_state_map(st, buf) : NULL;
	if (!m) {
		gb_cl_gl_error(0x0502);
		return GL_FALSE;
	}
	if (m->flags & MAP_PERSISTENT) {
		persist_up_one(m);
		st->npersist--;
		free(m->snap);
	} else if ((m->flags & MAP_WRITE) && !(m->flags & MAP_FLUSH_EXPLICIT)) {
		buffer_write(m, 0, m->shadow, m->length);
	}
	free(m->alloc);
	memset(m, 0, sizeof(*m));
	return GL_TRUE;
}

static void do_flush(uint32_t target, uint32_t named, int64_t offset,
		     int64_t length)
{
	struct gb_ctxstate *st = gb_cur_state();
	uint32_t buf = named;
	struct gb_map *m;

	if (!st)
		return;
	if (target && !target_valid(target)) {
		gb_cl_gl_error(0x0500);
		return;
	}
	if (target)
		buf = gb_state_bound(st, target);
	m = buf ? gb_state_map(st, buf) : NULL;
	if (!m || !(m->flags & MAP_FLUSH_EXPLICIT)) {
		gb_cl_gl_error(0x0502);
		return;
	}
	if (offset < 0 || length < 0 || offset + length > m->length) {
		gb_cl_gl_error(0x0501);
		return;
	}
	buffer_write(m, offset, (const uint8_t *)m->shadow + offset, length);
	if (m->snap)
		memcpy((uint8_t *)m->snap + offset,
		       (const uint8_t *)m->shadow + offset, (size_t)length);
}

static void do_pointer(uint32_t target, uint32_t named, GLenum pname,
		       void **params)
{
	struct gb_ctxstate *st = gb_cur_state();
	uint32_t buf = named;
	struct gb_map *m = NULL;

	if (target && !target_valid(target)) {
		gb_cl_gl_error(0x0500);
		return;
	}
	if (pname != GL_BUFFER_MAP_POINTER_) {
		gb_cl_gl_error(0x0500);
		return;
	}
	if (!params)
		return;
	if (st) {
		if (target)
			buf = gb_state_bound(st, target);
		if (!buf) { /* the reserved name 0 is bound */
			gb_cl_gl_error(0x0502);
			return;
		}
		m = gb_state_map(st, buf);
	}
	*params = m ? m->shadow : NULL;
}

/* ---- public entry points ------------------------------------------------------------------ */

GB_API void *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length,
			      GLbitfield access)
{
	return do_map(target, 0, offset, length, access, 0, 0);
}

GB_API GLboolean glUnmapBuffer(GLenum target)
{
	return do_unmap(target, 0);
}

GB_API void glFlushMappedBufferRange(GLenum target, GLintptr offset,
				     GLsizeiptr length)
{
	do_flush(target, 0, offset, length);
}

GB_API void glGetBufferPointerv(GLenum target, GLenum pname, void **params)
{
	do_pointer(target, 0, pname, params);
}

/* BUFFER_MAPPED etc. describe the client-side mapping, not the server's
 * (which is never mapped). */
static int map_param_buf(uint32_t buf, GLenum pname, int64_t *out)
{
	struct gb_ctxstate *st = gb_cur_state();
	struct gb_map *m;

	if (!st || !buf)
		return 0;
	m = gb_state_map(st, buf);
	switch (pname) {
	case GL_BUFFER_MAPPED_:
		*out = m ? 1 : 0;
		return 1;
	case GL_BUFFER_ACCESS_:
		if (!m)
			return 0;
		*out = (m->flags & (MAP_READ | MAP_WRITE)) == MAP_READ ? 0x88B8
		     : (m->flags & (MAP_READ | MAP_WRITE)) == MAP_WRITE ? 0x88B9
								     : 0x88BA;
		return 1;
	case GL_BUFFER_ACCESS_FLAGS_:
		if (!m)
			return 0;
		*out = m->flags;
		return 1;
	case GL_BUFFER_MAP_LENGTH_:
		if (!m)
			return 0;
		*out = m->length;
		return 1;
	case GL_BUFFER_MAP_OFFSET_:
		if (!m)
			return 0;
		*out = m->offset;
		return 1;
	default:
		return 0;
	}
}

static int map_param(GLenum target, GLenum pname, int64_t *out)
{
	struct gb_ctxstate *st = gb_cur_state();

	return st ? map_param_buf(gb_state_bound(st, target), pname, out) : 0;
}

GB_API void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
	int64_t v;

	if (params && map_param(target, pname, &v)) {
		*params = (GLint)v;
		return;
	}
	gbx_glGetBufferParameteriv(target, pname, params);
}

GB_API void glGetBufferParameteri64v(GLenum target, GLenum pname,
				     GLint64 *params)
{
	int64_t v;

	if (params && map_param(target, pname, &v)) {
		*params = v;
		return;
	}
	gbx_glGetBufferParameteri64v(target, pname, params);
}

#ifdef GB_LIB_GL
GB_API void *glMapBuffer(GLenum target, GLenum access)
{
	return do_map(target, 0, 0, 0, legacy_access_to_flags(access), 1, 1);
}

GB_API void *glMapBufferARB(GLenum target, GLenum access)
{
	return glMapBuffer(target, access);
}

GB_API GLboolean glUnmapBufferARB(GLenum target)
{
	return glUnmapBuffer(target);
}

GB_API void glGetBufferPointervARB(GLenum target, GLenum pname, void **params)
{
	glGetBufferPointerv(target, pname, params);
}

GB_API void *glMapNamedBuffer(GLuint buffer, GLenum access)
{
	return do_map(0, buffer, 0, 0, legacy_access_to_flags(access), 1, 1);
}

GB_API void *glMapNamedBufferEXT(GLuint buffer, GLenum access)
{
	return glMapNamedBuffer(buffer, access);
}

GB_API void *glMapNamedBufferRange(GLuint buffer, GLintptr offset,
				   GLsizeiptr length, GLbitfield access)
{
	return do_map(0, buffer, offset, length, access, 0, 0);
}

GB_API void *glMapNamedBufferRangeEXT(GLuint buffer, GLintptr offset,
				      GLsizeiptr length, GLbitfield access)
{
	return glMapNamedBufferRange(buffer, offset, length, access);
}

GB_API GLboolean glUnmapNamedBuffer(GLuint buffer)
{
	return do_unmap(0, buffer);
}

GB_API GLboolean glUnmapNamedBufferEXT(GLuint buffer)
{
	return glUnmapNamedBuffer(buffer);
}

GB_API void glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset,
					  GLsizeiptr length)
{
	do_flush(0, buffer, offset, length);
}

GB_API void glFlushMappedNamedBufferRangeEXT(GLuint buffer, GLintptr offset,
					     GLsizeiptr length)
{
	glFlushMappedNamedBufferRange(buffer, offset, length);
}

GB_API void glGetNamedBufferPointerv(GLuint buffer, GLenum pname,
				     void **params)
{
	do_pointer(0, buffer, pname, params);
}

GB_API void glGetNamedBufferPointervEXT(GLuint buffer, GLenum pname,
					void **params)
{
	glGetNamedBufferPointerv(buffer, pname, params);
}
#endif /* GB_LIB_GL */

/* ---- commands that are errors while their buffer is mapped ---------------------
 * The server's buffer is never really mapped, so the driver cannot raise
 * these; the client knows which buffers it has mapped. */

static int mapped_busy(uint32_t target, uint32_t named)
{
	struct gb_ctxstate *st = gb_cur_state();
	uint32_t buf = named;
	struct gb_map *m;

	if (!st)
		return 0;
	if (target)
		buf = gb_state_bound(st, target);
	m = buf ? gb_state_map(st, buf) : NULL;
	if (m && !(m->flags & MAP_PERSISTENT)) {
		gb_cl_gl_error(0x0502);
		return 1;
	}
	return 0;
}

GB_API void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
			    const void *data)
{
	if (!mapped_busy(target, 0))
		gbx_glBufferSubData(target, offset, size, data);
}

GB_API void glGetBufferSubData(GLenum target, GLintptr offset,
			       GLsizeiptr size, void *data)
{
	if (!mapped_busy(target, 0))
		gbx_glGetBufferSubData(target, offset, size, data);
}

GB_API void glCopyBufferSubData(GLenum readTarget, GLenum writeTarget,
				GLintptr readOffset, GLintptr writeOffset,
				GLsizeiptr size)
{
	if (!mapped_busy(readTarget, 0) && !mapped_busy(writeTarget, 0))
		gbx_glCopyBufferSubData(readTarget, writeTarget, readOffset,
					writeOffset, size);
}

#ifdef GB_LIB_GL
GB_API void glClearBufferData(GLenum target, GLenum internalformat,
			      GLenum format, GLenum type, const void *data)
{
	if (!mapped_busy(target, 0))
		gbx_glClearBufferData(target, internalformat, format, type,
				      data);
}

GB_API void glClearBufferSubData(GLenum target, GLenum internalformat,
				 GLintptr offset, GLsizeiptr size,
				 GLenum format, GLenum type, const void *data)
{
	if (!mapped_busy(target, 0))
		gbx_glClearBufferSubData(target, internalformat, offset, size,
					 format, type, data);
}

GB_API void glNamedBufferSubData(GLuint buffer, GLintptr offset,
				 GLsizeiptr size, const void *data)
{
	if (!mapped_busy(0, buffer))
		gbx_glNamedBufferSubData(buffer, offset, size, data);
}

GB_API void glGetNamedBufferSubData(GLuint buffer, GLintptr offset,
				    GLsizeiptr size, void *data)
{
	if (!mapped_busy(0, buffer))
		gbx_glGetNamedBufferSubData(buffer, offset, size, data);
}

GB_API void glCopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer,
				     GLintptr readOffset, GLintptr writeOffset,
				     GLsizeiptr size)
{
	if (!mapped_busy(0, readBuffer) && !mapped_busy(0, writeBuffer))
		gbx_glCopyNamedBufferSubData(readBuffer, writeBuffer,
					     readOffset, writeOffset, size);
}

GB_API void glClearNamedBufferData(GLuint buffer, GLenum internalformat,
				   GLenum format, GLenum type,
				   const void *data)
{
	if (!mapped_busy(0, buffer))
		gbx_glClearNamedBufferData(buffer, internalformat, format, type,
					   data);
}

GB_API void glClearNamedBufferSubData(GLuint buffer, GLenum internalformat,
				      GLintptr offset, GLsizeiptr size,
				      GLenum format, GLenum type,
				      const void *data)
{
	if (!mapped_busy(0, buffer))
		gbx_glClearNamedBufferSubData(buffer, internalformat, offset,
					      size, format, type, data);
}
#endif

#ifdef GB_LIB_GL
GB_API void glGetNamedBufferParameteriv(GLuint buffer, GLenum pname,
					GLint *params)
{
	int64_t v;

	if (params && map_param_buf(buffer, pname, &v)) {
		*params = (GLint)v;
		return;
	}
	gbx_glGetNamedBufferParameteriv(buffer, pname, params);
}

GB_API void glGetNamedBufferParameterivEXT(GLuint buffer, GLenum pname,
					   GLint *params)
{
	int64_t v;

	if (params && map_param_buf(buffer, pname, &v)) {
		*params = (GLint)v;
		return;
	}
	gbx_glGetNamedBufferParameterivEXT(buffer, pname, params);
}

GB_API void glGetNamedBufferParameteri64v(GLuint buffer, GLenum pname,
					  GLint64 *params)
{
	int64_t v;

	if (params && map_param_buf(buffer, pname, &v)) {
		*params = v;
		return;
	}
	gbx_glGetNamedBufferParameteri64v(buffer, pname, params);
}
#endif

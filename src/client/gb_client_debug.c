/* SPDX-License-Identifier: MIT */
/*
 * gb_client_debug.c - GL_KHR_debug / ARB_debug_output.
 *
 * The driver calls the callback on the server. The server queues each message
 * as an upcall (see gb_opx.h); this library delivers it to the application's
 * callback in the calling thread, before the offending call returns when it
 * was synchronous (and, for GL_DEBUG_OUTPUT_SYNCHRONOUS, even for otherwise
 * asynchronous calls: cx->sync_dbg turns each of them into a round trip).
 */
#define _GNU_SOURCE
#include "gb_client_impl.h"

typedef void (*gb_dbgproc)(GLenum, GLenum, GLuint, GLenum, GLsizei,
			   const GLchar *, const void *);

#define GL_DEBUG_OUTPUT_SYNCHRONOUS_ 0x8242

static void send_enable(struct gb_cl *cx, int on)
{
	struct gb_out o;
	struct gb_w w;

	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_DebugCallback, 24, 0))
		return;
	gb_w_u32(&w, on ? 1u : 0u);
	gb_cl_end(cx, &o);
}

void gb_cl_debug_update_sync(struct gb_cl *cx)
{
	struct gb_ctxstate *st;

	if (!cx)
		return;
	st = cx->cur;

	cx->sync_dbg = st && st->debug_cb && st->debug_sync;
}

static void set_callback(gb_dbgproc cb, const void *user)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_ctxstate *st = gb_cur_state_raw();

	if (!cx || !st)
		return;
	st->debug_cb = cb;
	st->debug_user = user;
	gb_cl_debug_update_sync(cx);
	send_enable(cx, cb != NULL);
	if (cb)
		gb_cl_upcall_helper_start(cx);
}

GB_API void glDebugMessageCallback(GLDEBUGPROC callback, const void *userParam)
{
	set_callback((gb_dbgproc)callback, userParam);
}

GB_API void glDebugMessageCallbackKHR(GLDEBUGPROCKHR callback,
				      const void *userParam)
{
	set_callback((gb_dbgproc)callback, userParam);
}

GB_API void glDebugMessageCallbackARB(GLDEBUGPROCARB callback,
				      const void *userParam)
{
	set_callback((gb_dbgproc)callback, userParam);
}

/* ---- calls with an explicit-length string ----------------------------------------- */

static void debug_str(uint32_t sub, uint32_t a, uint32_t b, uint32_t c,
		      uint32_t d, GLsizei length, const GLchar *s)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;
	uint32_t slen = GB_NULL_LEN;

	if (!cx)
		return;
	if (s) {
		size_t n = length < 0 ? strlen(s) : (size_t)length;

		if (n > (1u << 16)) { /* far beyond any real label/message */
			gb_cl_gl_error(0x0501); /* INVALID_VALUE */
			return;
		}
		slen = (uint32_t)n;
	}
	uint32_t sz = 16 + 24;
	if (slen != GB_NULL_LEN)
		sz = GB_ALIGN_UP(sz, 8u) + slen;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_DebugStr, sz, 0))
		return;
	gb_w_u32(&w, sub);
	gb_w_u32(&w, a);
	gb_w_u32(&w, b);
	gb_w_u32(&w, c);
	gb_w_u32(&w, d);
	gb_w_u32(&w, slen);
	if (slen != GB_NULL_LEN)
		memcpy(gb_w_blob(&w, slen), s, slen);
	gb_cl_end(cx, &o);
}

GB_API void glDebugMessageInsert(GLenum source, GLenum type, GLuint id,
				 GLenum severity, GLsizei length,
				 const GLchar *buf)
{
	debug_str(0, source, type, id, severity, length, buf);
}

GB_API void glDebugMessageInsertKHR(GLenum source, GLenum type, GLuint id,
				    GLenum severity, GLsizei length,
				    const GLchar *buf)
{
	glDebugMessageInsert(source, type, id, severity, length, buf);
}

GB_API void glDebugMessageInsertARB(GLenum source, GLenum type, GLuint id,
				    GLenum severity, GLsizei length,
				    const GLchar *buf)
{
	glDebugMessageInsert(source, type, id, severity, length, buf);
}

GB_API void glObjectLabel(GLenum identifier, GLuint name, GLsizei length,
			  const GLchar *label)
{
	debug_str(1, identifier, name, 0, 0, length, label);
}

GB_API void glObjectLabelKHR(GLenum identifier, GLuint name, GLsizei length,
			     const GLchar *label)
{
	glObjectLabel(identifier, name, length, label);
}

GB_API void glPushDebugGroup(GLenum source, GLuint id, GLsizei length,
			     const GLchar *message)
{
	debug_str(2, source, id, 0, 0, length, message);
}

GB_API void glPushDebugGroupKHR(GLenum source, GLuint id, GLsizei length,
				const GLchar *message)
{
	glPushDebugGroup(source, id, length, message);
}

/* ptr names a sync object: its client-side value is the bridge handle */
GB_API void glObjectPtrLabel(const void *ptr, GLsizei length,
			     const GLchar *label)
{
	debug_str(3, (uint32_t)(uintptr_t)ptr, 0, 0, 0, length, label);
}

GB_API void glObjectPtrLabelKHR(const void *ptr, GLsizei length,
				const GLchar *label)
{
	glObjectPtrLabel(ptr, length, label);
}

GB_API void glGetObjectPtrLabel(const void *ptr, GLsizei bufSize,
				GLsizei *length, GLchar *label)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (length)
		*length = 0;
	if (!cx || bufSize < 0)
		return;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_ObjPtrLabel, 24,
			    GB_CMD_F_WANT_REPLY))
		return;
	gb_w_u32(&w, (uint32_t)(uintptr_t)ptr);
	gb_w_u32(&w, (uint32_t)bufSize);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_ObjPtrLabel))
		return;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t n = gb_r_u32(&r);
	(void)gb_r_u32(&r);
	if (bufSize == 0) { /* length query */
		if (length && !r.err)
			*length = (GLsizei)n;
		gb_in_release(&in);
		return;
	}
	const char *s = n ? gb_r_blob(&r, n) : "";

	if (!r.err && s && label && bufSize > 0) {
		uint32_t c = n < (uint32_t)bufSize - 1 ? n : (uint32_t)bufSize - 1;

		memcpy(label, s, c);
		label[c] = 0;
		if (length)
			*length = (GLsizei)c;
	}
	gb_in_release(&in);
}

GB_API void glGetObjectPtrLabelKHR(const void *ptr, GLsizei bufSize,
				   GLsizei *length, GLchar *label)
{
	glGetObjectPtrLabel(ptr, bufSize, length, label);
}

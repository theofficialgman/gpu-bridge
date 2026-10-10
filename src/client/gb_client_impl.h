/*
 * gb_client_impl.h - internal interface of the client library (the arm32
 * side): channel/connection runtime used by generated stubs and by the
 * hand-written EGL/GLES layer.
 */
#ifndef GB_CLIENT_IMPL_H
#define GB_CLIENT_IMPL_H

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "gb_chan.h"
#include "gb_codec.h"
#include "gb_ctl.h"
#include "gb_gl_sizes.h"
#include "gb_handle_kinds.h"
#include "gb_ops.h"
#include "gb_opx.h"
#include "gb_client_gen.h"

#define GB_MAX_ATTRIBS 32

struct gb_attrib {
	uint8_t enabled;
	uint8_t normalized;
	int32_t size;
	uint32_t type;
	int32_t stride;
	uint32_t buffer;       /* ARRAY_BUFFER bound when the pointer was set */
	const void *ptr;       /* client pointer, or buffer offset */
};

/* Shadow of the GL state needed to size/route calls. One per EGLContext. */
struct gb_ctxstate {
	struct gb_ctxstate *next;
	uint32_t ctx_id;
	uint32_t array_buffer, element_buffer;
	uint32_t pack_buffer, unpack_buffer;
	uint32_t vao;
	struct gb_pixstore pack, unpack;
	struct gb_attrib attr[GB_MAX_ATTRIBS];
};

/* One per client thread. */
struct gb_cl {
	struct gb_chan chan;
	uint32_t gen;               /* connection generation (fork safety) */
	int lost;                   /* server gone: fail fast */
	struct gb_ctxstate *cur;    /* state of this thread's current context */
	EGLint egl_error;           /* locally generated EGL error (or 0) */
	GLenum gl_error;            /* locally generated GL error (or 0) */
};

struct gb_proc {
	const char *name;
	void *addr;
};
extern const struct gb_proc gb_proc_table[];

struct gb_ext {
	const char *name;
	int ncmds;
	const char *const *cmds;
};
extern const struct gb_ext gb_ext_table[];

struct gb_cl *gb_cl_get(void);
int gb_cl_begin(struct gb_cl *cx, struct gb_out *o, uint32_t op,
		uint32_t total, uint32_t flags);
void gb_cl_end(struct gb_cl *cx, struct gb_out *o);
int gb_cl_wait_reply(struct gb_cl *cx, struct gb_in *in, uint32_t op);
void gb_cl_set_error(struct gb_cl *cx, uint32_t op);
void gb_cl_unsupported(const char *what);
void gb_cl_unsupported_egl(const char *what);
void gb_cl_unsupported_gl(const char *what);

static inline int64_t gb_capcount(int64_t n)
{
	return n > ((int64_t)1 << 40) ? ((int64_t)1 << 40) : n;
}

int64_t gb_cl_pname_count(uint32_t op, uint32_t pname);
int64_t gb_cl_pixel_bytes(uint32_t op, uint32_t format, uint32_t type,
			  int64_t w, int64_t h);

#define GB_COUNT_PNAME(op, pname) gb_cl_pname_count((op), (uint32_t)(pname))
#define GB_BYTES_PIXELS(op, f, t, w, h) \
	gb_cl_pixel_bytes((op), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), \
			  (int64_t)(h))

#endif /* GB_CLIENT_IMPL_H */

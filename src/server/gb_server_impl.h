/*
 * gb_server_impl.h - internal interface of the 64-bit render server (gbd),
 * shared by the generated decoders and the hand-written handlers.
 */
#ifndef GB_SERVER_IMPL_H
#define GB_SERVER_IMPL_H

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
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

struct gb_tmp_node;

/* Per client thread: one channel and one worker. */
struct gb_sth {
	struct gb_chan chan;
	pthread_t thread;
	uint32_t id;
	struct gb_tmp_node *tmp; /* per-message scratch allocations */
	struct gb_scratch *scratch; /* client-array upload buffers per ctx */
};

typedef void (*gb_srv_fn)(struct gb_sth *, const struct gb_cmd_hdr *,
			  struct gb_r *);
extern const gb_srv_fn gb_srv_table[GB_OP_COUNT];

/* ---- native driver access ------------------------------------------------ */
int gb_native_init(void);
void *gb_native_sym(const char *name);

/* Declares and lazily binds `fn_<name>` to the native entry point. */
#define GB_NATIVE(name, ret, params)                                       \
	static ret(*fn_##name) params;                                     \
	static int bound_##name;                                           \
	if (!bound_##name) {                                               \
		fn_##name = (ret(*) params)gb_native_sym(#name);           \
		bound_##name = 1;                                          \
	}

/* ---- handles ------------------------------------------------------------- */
uint32_t gb_p2h(enum gb_handle_kind k, const void *p);
void *gb_h2p(enum gb_handle_kind k, uint32_t id);
void gb_h_free(enum gb_handle_kind k, uint32_t id);

/* ---- scratch allocation (reset after every message) ---------------------- */
void *gb_tmp(struct gb_sth *t, size_t bytes);
void *gb_tmp_zero(struct gb_sth *t, size_t bytes);
void gb_tmp_reset(struct gb_sth *t);

/* ---- replies -------------------------------------------------------------- */
int gb_sv_reply_begin(struct gb_sth *t, const struct gb_cmd_hdr *req,
		      struct gb_out *o, uint32_t size);
void gb_sv_reply_end(struct gb_sth *t, struct gb_out *o);
void gb_sv_proto_error(struct gb_sth *t, const struct gb_cmd_hdr *req);

/* ---- helpers used by generated size macros -------------------------------- */
int gb_sv_geti(void *ctx, uint32_t pname);
int64_t gb_sv_pname_count(uint32_t op, uint32_t pname);
int64_t gb_sv_pixel_bytes(uint32_t op, uint32_t format, uint32_t type,
			  int64_t w, int64_t h);

#define GB_COUNT_PNAME(op, pname) gb_sv_pname_count((op), (uint32_t)(pname))
#define GB_BYTES_PIXELS(op, f, t, w, h) \
	gb_sv_pixel_bytes((op), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), \
			  (int64_t)(h))

static inline int64_t gb_capcount(int64_t n)
{
	return n > ((int64_t)1 << 40) ? ((int64_t)1 << 40) : n;
}

/* ---- manual ops ----------------------------------------------------------- */
void gb_srv_manual_dispatch(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r);
void gb_srv_scratch_free(struct gb_sth *t);

/* Log if GB_DEBUG is set. */
void gb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int gb_debug_enabled(void);

/* With GB_DEBUG: report EGL calls that returned false/NULL and why. */
#define GB_EGL_TRACE(name, retval)                                         \
	do {                                                               \
		if (gb_debug_enabled() && !(retval)) {                     \
			GB_NATIVE(eglGetError, EGLint, (void));            \
			gb_log("%s failed, eglGetError=0x%x", name,        \
			       fn_eglGetError ? fn_eglGetError() : -1);    \
		}                                                          \
	} while (0)

#endif /* GB_SERVER_IMPL_H */

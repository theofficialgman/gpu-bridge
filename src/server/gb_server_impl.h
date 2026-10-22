/* SPDX-License-Identifier: MIT */
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

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>

/* ES-only types the generated decoders mention (gl2ext.h cannot be included
 * next to the desktop headers). */
typedef void *GLeglImageOES;
typedef int GLclampx; /* GLES1 fixed point */

#include "gb_carr.h"
#include "gb_chan.h"
#include "gb_codec.h"
#include "gb_ctl.h"
#include "gb_gl_sizes.h"
#include "gb_handle_kinds.h"
#include "gb_ops.h"
#include "gb_opx.h"

struct gb_tmp_node;
struct gb_upmsg;
struct gb_deferred;

/* Per client thread: one channel and one worker. */
/* client channels (threads) a server registers over its lifetime */
#define GB_MAX_SERVER_THREADS 8192

struct gb_sth {
	struct gb_chan chan;
	pthread_t thread;
	uint32_t id;
	struct gb_tmp_node *tmp; /* per-message scratch allocations */
	struct gb_scratch *scratch; /* client-array upload buffers per ctx */
	void *ctx_key;              /* cached current EGL/GLX context */
	int ctx_valid;              /* ctx_key is current (see CTX_CHANGERS) */
	unsigned err_stash[8];      /* errors popped from the driver that cannot
				     * be raised again; glGetError returns them */
	int err_nstash;
	/* upcalls (see gb_opx.h): queued from any thread, flushed by the worker */
	pthread_mutex_t up_lock;
	struct gb_upmsg *up_head, *up_tail;
	_Atomic int up_pending;     /* up_head is not empty: lets the worker skip the lock */
	uint32_t up_seq;            /* sequence of the last blocking upcall */
	int cur_req_sync;           /* the request being handled wants a reply */
	struct gb_deferred *defer_head, *defer_tail;
	int32_t px_dim[3];          /* w, h, d of the last server-sized pixel
				     * getter (the client scatters rows by it) */
	int taken_fds[8];           /* client fds adopted for the current request */
	int ntaken;
	/* GB_STATS accounting (written by the worker, read at exit) */
	uint64_t st_msgs, st_busy_ns, st_idle_ns, st_slow_ns, st_slow_msgs;
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

/* ---- file descriptors across the process boundary ------------------------- */
/* A client fd named in a request arrives on the control socket under a token.
 * gb_sv_fd_take() waits for it and returns a server-side fd that is closed
 * when the request has been handled (unless gb_sv_fd_keep()). -1 on failure. */
int gb_sv_fd_take(struct gb_sth *t, uint32_t token);
void gb_sv_fd_keep(struct gb_sth *t, int fd); /* ownership moved to the driver */
void gb_sv_fd_release(struct gb_sth *t);
/* Send an fd to the client; returns the token the reply carries (0: failed). */
uint32_t gb_sv_fd_give(int fd);
/* Attribute list whose fd-valued entries carry tokens: returns a copy (in the
 * per-message scratch) with the fds swapped in. wsz is 4 or 8. */
const void *gb_sv_xlat_fd_attribs(struct gb_sth *t, const void *list,
				  int64_t n, int wsz);
void gb_sv_fd_keep_all(struct gb_sth *t); /* the driver owns them now */

/* ---- upcalls (server -> client) ------------------------------------------ */
/* Queue a non-blocking upcall; safe from any thread (driver callbacks). It is
 * written to the client before the next reply / after the current request. */
void gb_sv_upcall_post(struct gb_sth *t, uint32_t id, const void *payload,
		       uint32_t len);
/* Worker thread only: write queued upcalls into the response ring. */
void gb_sv_upcall_flush(struct gb_sth *t);
/* Worker thread only: blocking upcall; returns the reply length or -1 (also
 * -1 immediately if the current request is asynchronous: the client is not
 * listening and a wait could deadlock against a full request ring). */
int gb_sv_upcall_call(struct gb_sth *t, uint32_t id, const void *payload,
		      uint32_t len, void *out, uint32_t outmax);
/* The thread whose worker is running this code (for callbacks that carry no
 * user pointer, e.g. EGL_ANDROID_blob_cache). */
struct gb_sth *gb_sv_this_thread(void);

/* ---- replies -------------------------------------------------------------- */
int gb_sv_reply_begin(struct gb_sth *t, const struct gb_cmd_hdr *req,
		      struct gb_out *o, uint32_t size);
void gb_sv_reply_end(struct gb_sth *t, struct gb_out *o);
void gb_sv_proto_error(struct gb_sth *t, const struct gb_cmd_hdr *req);

/* ---- X11 (loaded with dlopen; no link-time dependency) -------------------- */
void gb_x_init(void);
Display *gb_x_open(const char *name);
Display *gb_x_open_inst(const char *name, uint32_t inst);
XVisualInfo *gb_x_visual(Display *dpy, uint32_t visualid, uint32_t screen);
void gb_x_free(void *p);

/* ---- helpers used by generated size macros -------------------------------- */
int gb_sv_geti(void *ctx, uint32_t pname);
int64_t gb_sv_pixel_bytes(int mode, uint32_t format, uint32_t type, int64_t w,
			  int64_t h, int64_t d);
int64_t gb_sv_bitmap_bytes(int mode, int64_t w, int64_t h);

/* Server-sized out arrays: byte count computed from native GL state. */
int64_t gb_sv_sz_uniform(struct gb_sth *t, int wsz, uint32_t program,
			 int32_t location);
int64_t gb_sv_sz_pixels_tex(struct gb_sth *t, uint32_t target, int32_t level,
			    uint32_t format, uint32_t type);
int64_t gb_sv_sz_ctex(struct gb_sth *t, uint32_t target, int32_t level);
/* ARB_imaging getters: which = 0 color table, 1 convolution, 2/3 separable
 * row/column, 4 histogram, 5 minmax */
int64_t gb_sv_sz_imaging(struct gb_sth *t, int which, uint32_t target,
			 uint32_t format, uint32_t type);
int64_t gb_sv_sz_progstring_nv(struct gb_sth *t, uint32_t id, uint32_t pname);
/* EXT_direct_state_access variants: the texture is named, or addressed by
 * texture unit */
int64_t gb_sv_sz_pixels_dsa(struct gb_sth *t, int multi, uint32_t tex_or_unit,
			    uint32_t target, int32_t level, uint32_t format,
			    uint32_t type);
int64_t gb_sv_sz_ctex_dsa(struct gb_sth *t, int multi, uint32_t tex_or_unit,
			  uint32_t target, int32_t level);
int64_t gb_sv_sz_progstring_named(struct gb_sth *t, uint32_t program,
				  uint32_t target);
int64_t gb_sv_sz_pixelmap(struct gb_sth *t, int wsz, uint32_t map);
int64_t gb_sv_sz_map(struct gb_sth *t, int wsz, uint32_t target,
		     uint32_t query);
int64_t gb_sv_sz_uniformblock(struct gb_sth *t, int wsz, uint32_t program,
			      uint32_t block, uint32_t pname);
int64_t gb_sv_sz_progstring(struct gb_sth *t, uint32_t target,
			    uint32_t pname);
int64_t gb_sv_sz_subr_uniform(struct gb_sth *t, int wsz, uint32_t program,
			      uint32_t shadertype, uint32_t index,
			      uint32_t pname);
int64_t gb_sv_sz_atomic_buf(struct gb_sth *t, int wsz, uint32_t program,
			    uint32_t buffer, uint32_t pname);

#define GB_COUNT_PNAME(op, cls, pname) \
	gb_pname_count_class((cls), (uint32_t)(pname), gb_sv_geti, NULL)
int64_t gb_sv_compressed_bytes(int64_t size, int64_t w, int64_t h, int64_t d);
#define GB_BYTES_cbytes1(m, sz, w) \
	gb_sv_compressed_bytes((int64_t)(sz), (int64_t)(w), 1, 1)
#define GB_BYTES_cbytes2(m, sz, w, h) \
	gb_sv_compressed_bytes((int64_t)(sz), (int64_t)(w), (int64_t)(h), 1)
#define GB_BYTES_cbytes3(m, sz, w, h, d) \
	gb_sv_compressed_bytes((int64_t)(sz), (int64_t)(w), (int64_t)(h), \
			       (int64_t)(d))
#define GB_BYTES_pixels1(m, f, t, w) \
	gb_sv_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), 1, GB_DEPTH_2D)
#define GB_BYTES_pixels1t(m, tg, f, t, w) \
	gb_sv_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), 1, \
			  GB_DEPTH_2D)
#define GB_BYTES_pixels2(m, f, t, w, h) \
	gb_sv_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), \
			  (int64_t)(h), GB_DEPTH_2D)
#define GB_BYTES_pixels3(m, f, t, w, h, d) \
	gb_sv_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), \
			  (int64_t)(h), (int64_t)(d))
#define GB_BYTES_pixel1(m, f, t) \
	gb_sv_pixel_bytes(GB_PM_UNPACK, (uint32_t)(f), (uint32_t)(t), 1, 1, \
			GB_DEPTH_2D)
#define GB_BYTES_bitmap(m, w, h) \
	gb_sv_bitmap_bytes((m), (int64_t)(w), (int64_t)(h))

/* arguments: (thread, op, element size, ...) */
#define GB_SZS_uniform(t, op, wsz, p, l) gb_sv_sz_uniform((t), (wsz), (p), (l))
#define GB_SZS_pixels_tex(t, op, wsz, tg, lv, f, ty) \
	gb_sv_sz_pixels_tex((t), (tg), (lv), (f), (ty))
#define GB_SZS_ctex(t, op, wsz, tg, lv) gb_sv_sz_ctex((t), (tg), (lv))
#define GB_SZS_img_ct(t, op, wsz, tg, f, ty) gb_sv_sz_imaging((t), 0, (tg), (f), (ty))
#define GB_SZS_img_conv(t, op, wsz, tg, f, ty) gb_sv_sz_imaging((t), 1, (tg), (f), (ty))
#define GB_SZS_img_sep_row(t, op, wsz, tg, f, ty) gb_sv_sz_imaging((t), 2, (tg), (f), (ty))
#define GB_SZS_img_sep_col(t, op, wsz, tg, f, ty) gb_sv_sz_imaging((t), 3, (tg), (f), (ty))
#define GB_SZS_img_hist(t, op, wsz, tg, f, ty) gb_sv_sz_imaging((t), 4, (tg), (f), (ty))
#define GB_SZS_img_minmax(t, op, wsz, tg, f, ty) gb_sv_sz_imaging((t), 5, (tg), (f), (ty))
#define GB_SZS_progstring_nv(t, op, wsz, id, pn) \
	gb_sv_sz_progstring_nv((t), (id), (pn))
#define GB_SZS_pixels_dsa(t, op, wsz, tx, tg, lv, f, ty) \
	gb_sv_sz_pixels_dsa((t), 0, (tx), (tg), (lv), (f), (ty))
#define GB_SZS_pixels_multi(t, op, wsz, tx, tg, lv, f, ty) \
	gb_sv_sz_pixels_dsa((t), 1, (tx), (tg), (lv), (f), (ty))
#define GB_SZS_ctex_dsa(t, op, wsz, tx, tg, lv) \
	gb_sv_sz_ctex_dsa((t), 0, (tx), (tg), (lv))
#define GB_SZS_ctex_multi(t, op, wsz, tx, tg, lv) \
	gb_sv_sz_ctex_dsa((t), 1, (tx), (tg), (lv))
#define GB_SZS_progstring_named(t, op, wsz, pg, tg) \
	gb_sv_sz_progstring_named((t), (pg), (tg))
#define GB_SZS_pixelmap(t, op, wsz, m) gb_sv_sz_pixelmap((t), (wsz), (m))
#define GB_SZS_map(t, op, wsz, tg, q) gb_sv_sz_map((t), (wsz), (tg), (q))
#define GB_SZS_uniformblock(t, op, wsz, p, b, pn) \
	gb_sv_sz_uniformblock((t), (wsz), (p), (b), (pn))
#define GB_SZS_progstring(t, op, wsz, tg, pn) \
	gb_sv_sz_progstring((t), (tg), (pn))
#define GB_SZS_subr_uniform(t, op, wsz, p, st, i, pn) \
	gb_sv_sz_subr_uniform((t), (wsz), (p), (st), (i), (pn))
#define GB_SZS_atomic_buf(t, op, wsz, p, b, pn) \
	gb_sv_sz_atomic_buf((t), (wsz), (p), (b), (pn))

static inline int64_t gb_capcount(int64_t n)
{
	return n > ((int64_t)1 << 40) ? ((int64_t)1 << 40) : n;
}

/* ---- manual ops ----------------------------------------------------------- */
void gb_srv_manual_dispatch(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r);
void gb_srv_scratch_free(struct gb_sth *t);
void gb_sv_scratch_drop(void *ctx);
/* the application is deleting these buffer names (maybe our scratch ones) */
void gb_sv_scratch_buffers_deleted(int32_t n, const uint32_t *names);
int gb_sv_ctx_is_es(void);

/* Raise GL_INVALID_VALUE on the current context without side effects (used
 * when an argument cannot be sized and the call is dropped). */
void gb_sv_raise_invalid(void);
void gb_sv_raise_enum(void);
void gb_sv_raise_operation(void);
void gb_sv_raise_egl_param(void);

/* The errors a call left pending (glGetError drained), kept so they can be
 * put back: see the getter handling in the generated server. */
struct gb_errs {
	int n;
	unsigned e[8];
};
void gb_sv_err_pop(struct gb_errs *e);
void gb_sv_err_raise(const struct gb_errs *e);
int gb_sv_stash_take(struct gb_sth *t, unsigned *out);

/* GB_STATS: time spent inside the native driver call, per opcode. */
extern int gb_stats_on;
uint64_t gb_stat_mono_ns(void);
void gb_stat_native_add(uint32_t op, uint64_t ns);
static inline uint64_t gb_stat_now(void)
{
	return gb_stats_on ? gb_stat_mono_ns() : 0;
}
static inline void gb_stat_native(uint32_t op, uint64_t t0)
{
	if (gb_stats_on)
		gb_stat_native_add(op, gb_stat_mono_ns() - t0);
}

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

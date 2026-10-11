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
struct gb_sth {
	struct gb_chan chan;
	pthread_t thread;
	uint32_t id;
	struct gb_tmp_node *tmp; /* per-message scratch allocations */
	struct gb_scratch *scratch; /* client-array upload buffers per ctx */
	void *ctx_key;              /* cached current EGL/GLX context */
	int ctx_valid;              /* ctx_key is current (see CTX_CHANGERS) */
	/* upcalls (see gb_opx.h): queued from any thread, flushed by the worker */
	pthread_mutex_t up_lock;
	struct gb_upmsg *up_head, *up_tail;
	uint32_t up_seq;            /* sequence of the last blocking upcall */
	int cur_req_sync;           /* the request being handled wants a reply */
	struct gb_deferred *defer_head, *defer_tail;
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
int64_t gb_sv_sz_pixelmap(struct gb_sth *t, int wsz, uint32_t map);
int64_t gb_sv_sz_uniformblock(struct gb_sth *t, int wsz, uint32_t program,
			      uint32_t block, uint32_t pname);
int64_t gb_sv_sz_progstring(struct gb_sth *t, uint32_t target,
			    uint32_t pname);

#define GB_COUNT_PNAME(op, cls, pname) \
	gb_pname_count_class((cls), (uint32_t)(pname), gb_sv_geti, NULL)
#define GB_BYTES_pixels1(m, f, t, w) \
	gb_sv_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), 1, GB_DEPTH_2D)
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
#define GB_SZS_pixelmap(t, op, wsz, m) gb_sv_sz_pixelmap((t), (wsz), (m))
#define GB_SZS_uniformblock(t, op, wsz, p, b, pn) \
	gb_sv_sz_uniformblock((t), (wsz), (p), (b), (pn))
#define GB_SZS_progstring(t, op, wsz, tg, pn) \
	gb_sv_sz_progstring((t), (tg), (pn))

static inline int64_t gb_capcount(int64_t n)
{
	return n > ((int64_t)1 << 40) ? ((int64_t)1 << 40) : n;
}

/* ---- manual ops ----------------------------------------------------------- */
void gb_srv_manual_dispatch(struct gb_sth *t, const struct gb_cmd_hdr *h,
			    struct gb_r *r);
void gb_srv_scratch_free(struct gb_sth *t);

/* Raise GL_INVALID_VALUE on the current context without side effects (used
 * when an argument cannot be sized and the call is dropped). */
void gb_sv_raise_invalid(void);
void gb_sv_raise_enum(void);

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

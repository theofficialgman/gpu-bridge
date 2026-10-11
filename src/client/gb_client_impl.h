/*
 * gb_client_impl.h - internal interface of the client libraries (the arm32
 * side): channel/connection runtime used by the generated stubs and by the
 * hand-written EGL/GLES/GLX/GL layers.
 *
 * Compiled into the single bridge library, which exports EGL, GLES, GLX and
 * desktop GL and is installed under all of their sonames.
 */
#ifndef GB_CLIENT_IMPL_H
#define GB_CLIENT_IMPL_H

#define _GNU_SOURCE
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
/* One library exports every API; desktop-only helpers are always built. */
#define GB_LIB_GL 1
#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* ES-only types the generated stubs mention (gl2ext.h cannot be included
 * next to the desktop headers). */
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROCKHR)(GLenum, GLenum, GLuint, GLenum, GLsizei,
			       const GLchar *, const void *);

/* Is the byte at p readable? (probe without faulting; see gb_gl_sizes.h) */
static inline int gb_cl_addr_readable(const void *p)
{
	char c;
	struct iovec l = { &c, 1 }, r = { (void *)p, 1 };
	ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);

	if (n == 1)
		return 1;
	return errno != EFAULT; /* EPERM/ENOSYS: cannot tell, assume fine */
}
#define GB_ATTRIB_PAGE_OK(p) gb_cl_addr_readable(p)

#include "gb_carr.h"
#include "gb_chan.h"
#include "gb_codec.h"
#include "gb_ctl.h"
#include "gb_gl_sizes.h"
#include "gb_handle_kinds.h"
#include "gb_ops.h"
#include "gb_opx.h"
#include "gb_client_all_gen.h"

#define GB_API __attribute__((visibility("default")))

/* ---- shadow of the client-visible GL state --------------------------------- */

struct gb_carray {
	uint8_t enabled, normalized, integer, is_long;
	int32_t size;
	uint32_t type;
	int32_t stride;
	uint32_t buffer;   /* ARRAY_BUFFER bound when the pointer was set */
	uint32_t divisor;
	const void *ptr;   /* client pointer (buffer == 0) or buffer offset */
};

/* A vertex array object: array state + element array binding. */
struct gb_vao {
	struct gb_vao *next;
	uint32_t name;
	uint32_t element_buffer;
	struct gb_carray a[CA_COUNT];
};

#define GB_CLIENT_ATTRIB_STACK 16
#define GB_MAX_MAPS 16
#define GB_BUF_TARGETS 16

/* One live glMapBuffer*(): the client holds a shadow copy of the range. */
struct gb_map {
	uint32_t buffer;     /* buffer object name (0 = free slot) */
	uint32_t target;     /* target it was mapped through (0 = named API) */
	int64_t offset, length;
	uint32_t flags;      /* GL_MAP_* bits (legacy glMapBuffer translated) */
	void *shadow;
};

struct gb_ctxstate {
	struct gb_ctxstate *next;
	uint32_t ctx_id;
	uint32_t array_buffer, pack_buffer, unpack_buffer;
	uint32_t bound[GB_BUF_TARGETS]; /* other generic buffer targets */
	struct gb_map maps[GB_MAX_MAPS];
	struct gb_vao *vao;     /* current VAO (&vao0 for the default) */
	struct gb_vao vao0;
	struct gb_vao *vaos;    /* named VAOs seen so far */
	struct gb_pixstore pack, unpack;
	uint32_t client_active_tex; /* texture unit index */
	int8_t core;                /* -1 unknown, 0 compat/ES, 1 core profile */
	/* Between glBegin and glEnd nearly every other command is an error and
	 * must not touch the shadow: gb_cur_state() hides the state then, so
	 * wrappers forward the call and the driver raises INVALID_OPERATION.
	 * list_mode: GL_COMPILE / GL_COMPILE_AND_EXECUTE while a list is open
	 * (a glBegin that is only compiled does not start immediate mode). */
	uint8_t in_begin;
	uint8_t save_begin;  /* a glBegin was seen while compiling a list */
	uint16_t list_mode;
	/* glRenderMode(GL_SELECT / GL_FEEDBACK): the application's buffers */
	uint32_t render_mode;       /* 0 = GL_RENDER */
	GLuint *sel_ptr;
	GLsizei sel_size;
	GLfloat *fb_ptr;
	GLsizei fb_size;
	/* glPushClientAttrib / glPopClientAttrib */
	struct gb_attrib_save {
		uint32_t mask;
		struct gb_carray a[CA_COUNT];
		uint32_t element_buffer, array_buffer, client_active_tex;
		struct gb_pixstore pack, unpack;
	} *stack[GB_CLIENT_ATTRIB_STACK];
	int stack_depth;
};

/* One per client thread. */
struct gb_cl {
	struct gb_chan chan;
	uint32_t gen;               /* connection generation (fork safety) */
	int lost;                   /* server gone: fail fast */
	struct gb_ctxstate *cur;    /* state of this thread's current context */
	int32_t egl_error;          /* locally generated EGL error (or 0) */
	uint32_t gl_error;          /* locally generated GL error (or 0) */
	Display *cur_xdpy;          /* Display the current GLX context uses */
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

/* ---- runtime (gb_client.c) ---------------------------------------------------- */
struct gb_cl *gb_cl_get(void);
int gb_cl_begin(struct gb_cl *cx, struct gb_out *o, uint32_t op,
		uint32_t total, uint32_t flags);
void gb_cl_end(struct gb_cl *cx, struct gb_out *o);
int gb_cl_wait_reply(struct gb_cl *cx, struct gb_in *in, uint32_t op);
void gb_cl_set_error(struct gb_cl *cx, uint32_t op);
void gb_cl_unsupported(const char *what);
void gb_cl_unsupported_egl(const char *what);
void gb_cl_unsupported_gl(const char *what);
void gb_cl_unsupported_glx(const char *what);

/* locally generated API errors (merged into glGetError / eglGetError) */
void gb_cl_gl_error(uint32_t e);
void gb_cl_egl_error(int32_t e);

/* hand-written message helper: header written, cursor at the first field */
int gb_cl_msg_begin(struct gb_cl *cx, struct gb_out *o, struct gb_w *w,
		    uint32_t op, uint32_t size, uint32_t flags);

/* Interned, never-freed strings: results must outlive the call. */
const char *gb_cl_intern(const char *s, size_t len);
/* Round-trip string fetch (GetString/QueryString/GLXString ops). */
const char *gb_cl_fetch_string(uint32_t op, uint32_t a, uint32_t b);
/* Space-separated extension list reduced to what this library implements. */
const char *gb_cl_filter_ext(const char *list);
/* Filtered GL extension list of the current context. */
struct gb_extlist {
	const char *joined;
	const char **names;
	int n;
};
const struct gb_extlist *gb_cl_ext_list(void);

/* X11 helpers (resolved with dlsym: no link-time libX11 dependency) */
uint32_t gb_cl_xdpy_id(Display *dpy);
XVisualInfo *gb_cl_xvisual(Display *dpy, uint32_t vid, uint32_t scr);
const char *gb_cl_xdisplay_string(Display *dpy);
/* Make the app's pending X requests visible to the server's connection. */
void gb_cl_xsync(Display *dpy);

/* ---- shadow state (gb_client_arrays.c) ------------------------------------------ */
struct gb_ctxstate *gb_state_for(uint32_t ctx_id);
struct gb_ctxstate *gb_cur_state(void);
struct gb_ctxstate *gb_cur_state_raw(void); /* even between glBegin/glEnd */
int gb_cl_buf_bound(struct gb_cl *cx, int which);
/* Buffer currently bound to a generic target (0 if none/unknown target). */
uint32_t gb_state_bound(struct gb_ctxstate *st, uint32_t target);
/* Record a binding to a generic buffer target. */
void gb_state_bind(struct gb_ctxstate *st, uint32_t target, uint32_t buffer);
/* Active mapping of `buffer`, or NULL. */
struct gb_map *gb_state_map(struct gb_ctxstate *st, uint32_t buffer);

/* Returns 1 if the draw was handled (client arrays/indices involved or an
 * error was raised); 0 means: issue the ordinary generated call (any pointer
 * argument is then a buffer offset). */
int gb_draw_client(struct gb_ctxstate *st, uint32_t mode, int indexed,
		   int32_t first, int32_t count, uint32_t itype,
		   const void *indices, int32_t instances, int is_instanced,
		   int32_t basevertex, uint32_t baseinstance);
/* Record a *Pointer() call. Returns 1 if it was a client-memory pointer
 * (shadowed only; nothing is sent), 0 if a buffer offset (send it). */
int gb_arr_pointer(struct gb_ctxstate *st, int slot, int size, uint32_t type,
		   int normalized, int integer, int is_long, int stride,
		   const void *ptr);
void gb_arr_enable(struct gb_ctxstate *st, int slot, int on);

/* ---- size-rule macros used by generated code ------------------------------------- */
static inline int64_t gb_capcount(int64_t n)
{
	return n > ((int64_t)1 << 40) ? ((int64_t)1 << 40) : n;
}

int gb_cl_geti(void *ctx, uint32_t pname);
int64_t gb_cl_pixel_bytes(int mode, uint32_t format, uint32_t type, int64_t w,
			  int64_t h, int64_t d);
int64_t gb_cl_bitmap_bytes(int mode, int64_t w, int64_t h);

#define GB_COUNT_PNAME(op, cls, pname) \
	gb_pname_count_class((cls), (uint32_t)(pname), gb_cl_geti, NULL)
#define GB_BYTES_pixels1(m, f, t, w) \
	gb_cl_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), 1, GB_DEPTH_2D)
#define GB_BYTES_pixels2(m, f, t, w, h) \
	gb_cl_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), \
			  (int64_t)(h), GB_DEPTH_2D)
#define GB_BYTES_pixels3(m, f, t, w, h, d) \
	gb_cl_pixel_bytes((m), (uint32_t)(f), (uint32_t)(t), (int64_t)(w), \
			  (int64_t)(h), (int64_t)(d))
#define GB_BYTES_pixel1(m, f, t) \
	gb_cl_pixel_bytes(GB_PM_UNPACK, (uint32_t)(f), (uint32_t)(t), 1, 1, \
			GB_DEPTH_2D)
#define GB_BYTES_bitmap(m, w, h) \
	gb_cl_bitmap_bytes((m), (int64_t)(w), (int64_t)(h))

#endif /* GB_CLIENT_IMPL_H */

/*
 * gbd.c - gpu-bridge render server (64-bit).
 *
 * Started by the client library with the control socket on a given fd
 * (`gbd --fd N`). Loads the real vendor EGL/GLES libraries and executes the
 * client's API calls on their behalf. One worker thread per client thread.
 *
 * Environment:
 *   GB_NATIVE_EGL, GB_NATIVE_GLES2   library names (default libEGL.so.1,
 *                                    libGLESv2.so.2)
 *   GB_EGL_PLATFORM=surfaceless      use EGL_MESA_platform_surfaceless for
 *                                    the default display (headless dev/CI)
 *   GB_DEBUG=1                       log to stderr
 */
#define _GNU_SOURCE
#define GB_OPS_NAMES 1 /* gb_op_names[] for GB_STATS */
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gb_server_impl.h"
#include "gb_es_intro.h"

static int g_debug;
static void *g_egl, *g_gles, *g_gl;

void gb_log(const char *fmt, ...)
{
	if (!g_debug)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "gbd[%d]: ", (int)getpid());
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

int gb_debug_enabled(void)
{
	return g_debug;
}

/* ---- native libraries ----------------------------------------------------- */

int gb_native_init(void)
{
	const char *e = getenv("GB_NATIVE_EGL");
	const char *g = getenv("GB_NATIVE_GLES2");
	const char *gl = getenv("GB_NATIVE_GL");

	gb_x_init(); /* XInitThreads must come before any other Xlib use */
	g_egl = dlopen(e ? e : "libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	g_gl = dlopen(gl ? gl : "libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	g_gles = dlopen(g ? g : "libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	/* Never use ourselves as the "native" driver (see gb_client.c). */
	void **libs[] = { &g_egl, &g_gles, &g_gl };
	for (unsigned i = 0; i < 3; i++) {
		if (*libs[i] && dlsym(*libs[i], "gb_bridge_client_marker")) {
			fprintf(stderr, "gbd: refusing to use the gpu-bridge "
				"client library as the native driver "
				"(check LD_LIBRARY_PATH / GB_NATIVE_*)\n");
			dlclose(*libs[i]);
			*libs[i] = NULL;
		}
	}
	if (!g_egl && !g_gl) {
		fprintf(stderr, "gbd: cannot load EGL or GL: %s\n", dlerror());
		return -1;
	}
	if (!g_gles)
		fprintf(stderr, "gbd: warning: no GLESv2: %s\n", dlerror());
	return 0;
}

static void *gl_lookup(const char *name)
{
	void *p = NULL;

	if (g_gles)
		p = dlsym(g_gles, name);
	if (!p && g_egl)
		p = dlsym(g_egl, name);
	if (!p && g_gl)
		p = dlsym(g_gl, name);
	if (!p && g_egl) {
		void *(*gpa)(const char *) = dlsym(g_egl, "eglGetProcAddress");
		if (gpa)
			p = gpa(name);
	}
	if (!p && g_gl) {
		void *(*gpa)(const char *) = dlsym(g_gl, "glXGetProcAddressARB");
		if (gpa)
			p = gpa(name);
	}
	return p;
}

void *gb_native_sym(const char *name)
{
	void *p = NULL;
	int is_egl = !strncmp(name, "egl", 3);
	int is_glx = !strncmp(name, "glX", 3);

	/*
	 * Namespace-aware on purpose: GLVND's glXGetProcAddress* hands back a
	 * dispatch stub for ANY name (it cannot know what a vendor exports), so
	 * it must never be asked about egl* names.
	 */
	if (is_egl) {
		if (g_egl)
			p = dlsym(g_egl, name);
		if (!p && g_egl) {
			void *(*gpa)(const char *) =
				dlsym(g_egl, "eglGetProcAddress");
			if (gpa)
				p = gpa(name);
		}
		return p;
	}
	if (is_glx) {
		if (g_gl)
			p = dlsym(g_gl, name);
		return p;
	}
	/* OpenGL / OpenGL ES entry points */
	/*
	 * An ES context older than 3.2 may only offer a core-era function under
	 * its extension spelling (glDrawElementsBaseVertexOES): the client
	 * folds OES/EXT spellings into the core name, so a context that lacks
	 * the core function must be given the extension one. Newer contexts
	 * and desktop GL keep the exact name.
	 */
	static int es_ver = -1; /* 0: not ES (or undecided), else 10*major+minor */

	if (es_ver < 0) {
		const char *(*gs)(unsigned) = gl_lookup("glGetString");
		const char *v = gs ? gs(0x1F02 /* VERSION */) : NULL;

		if (v) { /* only decided once a context answered */
			es_ver = 0;
			if (!strncmp(v, "OpenGL ES", 9)) {
				while (*v && (*v < '0' || *v > '9'))
					v++;
				es_ver = (v[0] - '0') * 10 +
					 ((v[1] == '.' && v[2] >= '0') ?
						  v[2] - '0' : 0);
			}
		}
	}
	if (es_ver > 0) {
		int late = 0;

		for (int i = 0; gb_es_intro[i].name; i++)
			if (!strcmp(gb_es_intro[i].name, name)) {
				late = gb_es_intro[i].ver > es_ver;
				break;
			}
		if (late) {
			static const char *const sfx[] = { "OES", "EXT", "KHR" };
			char buf[160];
			size_t n = strlen(name);

			if (n + 4 < sizeof buf)
				for (unsigned i = 0; i < 3 && !p; i++) {
					memcpy(buf, name, n);
					strcpy(buf + n, sfx[i]);
					p = gl_lookup(buf);
				}
		}
	}
	if (!p)
		p = gl_lookup(name);
	if (!p) {
		/*
		 * The same function under another vendor suffix: an ES 2
		 * context only has glDrawElementsBaseVertexOES where the
		 * client (and the desktop spelling) says glDrawElementsBaseVertex.
		 */
		static const char *const sfx[] = { "OES", "EXT", "KHR", "ARB",
						   "NV" };
		char buf[160];
		size_t n = strlen(name);

		if (n + 4 < sizeof buf)
			for (unsigned i = 0; i < sizeof sfx / sizeof sfx[0] && !p;
			     i++) {
				memcpy(buf, name, n);
				strcpy(buf + n, sfx[i]);
				p = gl_lookup(buf);
			}
	}
	return p;
}

/* ---- scratch allocation --------------------------------------------------- */

struct gb_tmp_node {
	struct gb_tmp_node *next;
	max_align_t pad[0];
};

void *gb_tmp(struct gb_sth *t, size_t bytes)
{
	/* 16-aligned payload after the node header. */
	struct gb_tmp_node *n = malloc(sizeof(*n) + 16 + bytes);
	if (!n)
		return NULL;
	n->next = t->tmp;
	t->tmp = n;
	uintptr_t p = ((uintptr_t)(n + 1) + 15) & ~(uintptr_t)15;
	return (void *)p;
}

void *gb_tmp_zero(struct gb_sth *t, size_t bytes)
{
	void *p = gb_tmp(t, bytes);
	if (p)
		memset(p, 0, bytes);
	return p;
}

void gb_tmp_reset(struct gb_sth *t)
{
	while (t->tmp) {
		struct gb_tmp_node *n = t->tmp;
		t->tmp = n->next;
		free(n);
	}
}

/* ---- replies -------------------------------------------------------------- */

int gb_sv_reply_begin(struct gb_sth *t, const struct gb_cmd_hdr *req,
		      struct gb_out *o, uint32_t size)
{
	int rc;

	gb_sv_upcall_flush(t); /* debug messages precede the reply */
	rc = gb_out_begin(o, &t->chan.rsp, size);
	if (rc)
		return rc;
	struct gb_cmd_hdr h = { req->opcode, size, req->seq, GB_CMD_F_REPLY };
	memcpy(o->p, &h, sizeof(h));
	return 0;
}

void gb_sv_reply_end(struct gb_sth *t, struct gb_out *o)
{
	gb_out_end(o);
}

void gb_sv_proto_error(struct gb_sth *t, const struct gb_cmd_hdr *req)
{
	gb_log("protocol error: opcode %u size %u", req->opcode, req->size);
	if (!(req->flags & GB_CMD_F_WANT_REPLY))
		return;
	struct gb_out o;
	if (gb_out_begin(&o, &t->chan.rsp, sizeof(struct gb_cmd_hdr)))
		return;
	struct gb_cmd_hdr h = { req->opcode, sizeof(h), req->seq,
				GB_CMD_F_REPLY | GB_CMD_F_ERROR };
	memcpy(o.p, &h, sizeof(h));
	gb_out_end(&o);
}

/* ---- size helpers (native state) ------------------------------------------ */

int gb_sv_geti(void *ctx, uint32_t pname)
{
	GB_NATIVE(glGetIntegerv, void, (GLenum, GLint *));
	GLint v = 0;
	(void)ctx;
	if (fn_glGetIntegerv)
		fn_glGetIntegerv(pname, &v);
	return v;
}

/* ---- worker --------------------------------------------------------------- */

int gb_stats_on;

void gb_sv_raise_invalid(void)
{
	GB_NATIVE(glDeleteTextures, void, (GLsizei, const GLuint *));

	if (fn_glDeleteTextures)
		fn_glDeleteTextures(-1, NULL); /* INVALID_VALUE, no effect */
}

void gb_sv_raise_egl_param(void)
{
	GB_NATIVE(eglBindAPI, EGLBoolean, (EGLenum));

	if (fn_eglBindAPI)
		fn_eglBindAPI(0); /* EGL_BAD_PARAMETER, binds nothing */
}

void gb_sv_raise_operation(void)
{
	GB_NATIVE(glUnmapBuffer, GLboolean, (GLenum));
	GB_NATIVE(glEndTransformFeedback, void, (void));
	GB_NATIVE(glEndList, void, (void));

	/* Unmapping a buffer that is not mapped is INVALID_OPERATION and has no
	 * effect (the server never leaves a buffer mapped). Ending a feedback
	 * or list is the same error only when none is open. */
	if (fn_glUnmapBuffer)
		fn_glUnmapBuffer(0x8892 /* ARRAY_BUFFER */);
	else if (fn_glEndTransformFeedback)
		fn_glEndTransformFeedback();
	else if (fn_glEndList)
		fn_glEndList();
}

void gb_sv_err_pop(struct gb_errs *e)
{
	GB_NATIVE(glGetError, GLenum, (void));

	e->n = 0;
	if (!fn_glGetError)
		return;
	while (e->n < 8) {
		GLenum c = fn_glGetError();

		if (c == 0)
			break;
		e->e[e->n++] = c;
	}
}

static __thread struct gb_sth *t_this; /* set in worker_main */

/* A GL keeps one flag per error code: the stash never holds a code twice. */
static void stash_add(struct gb_sth *t, unsigned code)
{
	for (int i = 0; i < t->err_nstash; i++)
		if (t->err_stash[i] == code)
			return;
	if (t->err_nstash < 8)
		t->err_stash[t->err_nstash++] = code;
}

/*
 * glGetError: the oldest stashed error, with every other report of the same
 * code folded into it (the driver may have raised it again since it was
 * stashed). Other codes the driver holds move to the stash, in order.
 */
int gb_sv_stash_take(struct gb_sth *t, unsigned *out)
{
	struct gb_errs e;

	if (t->err_nstash <= 0)
		return 0;
	*out = t->err_stash[0];
	memmove(t->err_stash, t->err_stash + 1,
		(size_t)(--t->err_nstash) * sizeof t->err_stash[0]);
	gb_sv_err_pop(&e);
	for (int i = 0; i < e.n; i++)
		if (e.e[i] != *out)
			stash_add(t, e.e[i]);
	return 1;
}

/*
 * The errors a getter's own call raised were taken out of the driver to find
 * out whether it failed. They are kept and returned by the next glGetError,
 * in order, instead of being raised again: a synthetic call would make the
 * driver report them a second time through the debug-message callback.
 */
void gb_sv_err_raise(const struct gb_errs *e)
{
	for (int i = 0; i < e->n; i++)
		if (t_this)
			stash_add(t_this, e->e[i]);
}

void gb_sv_raise_enum(void)
{
	GB_NATIVE(glBindTexture, void, (GLenum, GLuint));

	if (fn_glBindTexture)
		fn_glBindTexture(0xFFFF, 0); /* INVALID_ENUM, no effect */
}

/* The last opcodes each worker executed, dumped from a crash handler: the
 * driver crashes inside symbol-less proprietary code, so "what was the app
 * doing" is otherwise unrecoverable. */
#define RECENT_OPS 16
static __thread uint32_t t_recent[RECENT_OPS];
static __thread unsigned t_recent_n;
static __thread uint32_t t_recent_cur;

static void crash_handler(int sig, siginfo_t *si, void *uc)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf),
			 "gbd: fatal signal %d at address %p; last %d ops "
			 "(oldest first):\n", sig, si->si_addr, RECENT_OPS);
	(void)!write(2, buf, (size_t)n);
	for (unsigned i = 0; i < RECENT_OPS; i++) {
		uint32_t op = t_recent[(t_recent_n + i) % RECENT_OPS];
		const char *nm = op && op < GB_OP_COUNT ? gb_op_names[op]
							 : (op ? "opx/unknown" : "-");
		n = snprintf(buf, sizeof(buf), "   %2u: %s\n", i, nm);
		(void)!write(2, buf, (size_t)n);
	}
	if (t_recent_cur && t_recent_cur < GB_OP_COUNT) {
		n = snprintf(buf, sizeof(buf), "   executing: %s\n",
			     gb_op_names[t_recent_cur]);
		(void)!write(2, buf, (size_t)n);
	}
	_exit(128 + sig);
}

static void install_crash_handler(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
}

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t gb_stat_mono_ns(void)
{
	return mono_ns();
}

static struct {
	uint64_t calls, ns;
} g_native[GB_OP_COUNT];

void gb_stat_native_add(uint32_t op, uint64_t ns)
{
	if (op < GB_OP_COUNT) {
		g_native[op].calls++;
		g_native[op].ns += ns;
	}
}

/* ---- fds ------------------------------------------------------------------ */

static pthread_mutex_t g_fd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fd_cv = PTHREAD_COND_INITIALIZER;
static struct { uint32_t token; int fd; } g_fds[64];
static int g_ctl_sock = -1;
static uint32_t g_give_token;

static void fd_store(uint32_t token, int fd)
{
	pthread_mutex_lock(&g_fd_mu);
	for (unsigned i = 0; i < 64; i++)
		if (!g_fds[i].token) {
			g_fds[i].token = token;
			g_fds[i].fd = fd;
			pthread_cond_broadcast(&g_fd_cv);
			pthread_mutex_unlock(&g_fd_mu);
			return;
		}
	pthread_mutex_unlock(&g_fd_mu);
	close(fd); /* table full: the request will fail to find it */
}

int gb_sv_fd_take(struct gb_sth *t, uint32_t token)
{
	struct timespec ts;
	int fd = -1;

	if (!token)
		return -1;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 5;
	pthread_mutex_lock(&g_fd_mu);
	for (;;) {
		for (unsigned i = 0; i < 64; i++)
			if (g_fds[i].token == token) {
				fd = g_fds[i].fd;
				g_fds[i].token = 0;
				goto done;
			}
		if (pthread_cond_timedwait(&g_fd_cv, &g_fd_mu, &ts))
			break;
	}
done:
	pthread_mutex_unlock(&g_fd_mu);
	if (fd >= 0 && t && t->ntaken < 8)
		t->taken_fds[t->ntaken++] = fd;
	else if (fd >= 0 && t) {
		close(fd);
		fd = -1;
	}
	return fd;
}

void gb_sv_fd_keep(struct gb_sth *t, int fd)
{
	for (int i = 0; i < t->ntaken; i++)
		if (t->taken_fds[i] == fd)
			t->taken_fds[i] = -1;
}

void gb_sv_fd_release(struct gb_sth *t)
{
	for (int i = 0; i < t->ntaken; i++)
		if (t->taken_fds[i] >= 0)
			close(t->taken_fds[i]);
	t->ntaken = 0;
}

void gb_sv_fd_keep_all(struct gb_sth *t)
{
	for (int i = 0; i < t->ntaken; i++)
		t->taken_fds[i] = -1;
}

static int fd_attrib_key(int64_t k)
{
	return k == 0x3272 || k == 0x3275 || k == 0x3278 || k == 0x3440 ||
	       k == 0x3145 || k == 0x333C;
}

const void *gb_sv_xlat_fd_attribs(struct gb_sth *t, const void *list,
				  int64_t n, int wsz)
{
	void *c;

	if (!list || n <= 0)
		return list;
	c = gb_tmp(t, (size_t)n * (size_t)wsz);
	if (!c)
		return list;
	memcpy(c, list, (size_t)n * (size_t)wsz);
	for (int64_t i = 0; i + 1 < n; i += 2) {
		int64_t key = wsz == 8 ? ((int64_t *)c)[i] : ((int32_t *)c)[i];
		int64_t val = wsz == 8 ? ((int64_t *)c)[i + 1]
				       : ((int32_t *)c)[i + 1];

		if (!fd_attrib_key(key) || val == -1)
			continue; /* -1: EGL_NO_NATIVE_FENCE_FD_ANDROID */
		int fd = gb_sv_fd_take(t, (uint32_t)val);

		if (wsz == 8)
			((int64_t *)c)[i + 1] = fd;
		else
			((int32_t *)c)[i + 1] = fd;
	}
	return c;
}

uint32_t gb_sv_fd_give(int fd)
{
	struct gb_ctl_msg m = { .magic = GB_CTL_MAGIC, .type = GB_CTL_FD_GIVE };
	uint32_t tok = __atomic_add_fetch(&g_give_token, 1, __ATOMIC_RELAXED);

	if (fd < 0 || g_ctl_sock < 0)
		return 0;
	m.a[0] = tok;
	return gb_ctl_send(g_ctl_sock, &m, fd) ? 0 : tok;
}

/* ---- upcalls ---------------------------------------------------------------- */

struct gb_upmsg {
	struct gb_upmsg *next;
	uint32_t id, len;
	uint8_t data[];
};

struct gb_deferred {
	struct gb_deferred *next;
	uint32_t size;
	uint8_t data[];
};


struct gb_sth *gb_sv_this_thread(void)
{
	return t_this;
}

#define GB_UP_MAX_PAYLOAD (GB_CHAN_RING_BYTES / 8)

void gb_sv_upcall_post(struct gb_sth *t, uint32_t id, const void *payload,
		       uint32_t len)
{
	struct gb_upmsg *m;

	if (!t || len > GB_UP_MAX_PAYLOAD)
		return;
	m = malloc(sizeof(*m) + len);
	if (!m)
		return;
	m->next = NULL;
	m->id = id;
	m->len = len;
	if (len)
		memcpy(m->data, payload, len);
	pthread_mutex_lock(&t->up_lock);
	if (t->up_tail)
		t->up_tail->next = m;
	else
		t->up_head = m;
	t->up_tail = m;
	pthread_mutex_unlock(&t->up_lock);
}

/* Write one upcall record; non-blocking: dropped if the ring is too full
 * (the client may not be draining and the worker must never block here). */
static int upcall_write(struct gb_sth *t, uint32_t id, const void *payload,
			uint32_t len, uint32_t flags, uint32_t seq)
{
	uint32_t size = GB_ALIGN_UP((uint32_t)sizeof(struct gb_cmd_hdr) + len, 8u);
	struct gb_out o;

	if (gb_ring_free(&t->chan.rsp) < size + 64)
		return -1;
	if (gb_out_begin(&o, &t->chan.rsp, size))
		return -1;
	struct gb_cmd_hdr h = { id, size, seq, flags | GB_CMD_F_UPCALL };
	memset(o.p, 0, size);
	memcpy(o.p, &h, sizeof(h));
	if (len)
		memcpy(o.p + sizeof(h), payload, len);
	gb_out_end(&o);
	return 0;
}

void gb_sv_upcall_flush(struct gb_sth *t)
{
	struct gb_upmsg *m;

	pthread_mutex_lock(&t->up_lock);
	m = t->up_head;
	t->up_head = t->up_tail = NULL;
	pthread_mutex_unlock(&t->up_lock);
	while (m) {
		struct gb_upmsg *n = m->next;

		if (upcall_write(t, m->id, m->data, m->len, 0, 0) &&
		    getenv("GB_TRACE_OPS"))
			fprintf(stderr, "s: upcall %#x DROPPED (ring full, %u bytes)\n",
				m->id, m->len);
		free(m);
		m = n;
	}
}

int gb_sv_upcall_call(struct gb_sth *t, uint32_t id, const void *payload,
		      uint32_t len, void *out, uint32_t outmax)
{
	uint32_t seq = ++t->up_seq;
	int result = -1;

	if (!t->cur_req_sync || len > GB_UP_MAX_PAYLOAD)
		return -1;
	gb_sv_upcall_flush(t); /* keep ordering with earlier messages */
	if (upcall_write(t, id, payload, len, GB_CMD_F_WANT_REPLY, seq))
		return -1;
	for (;;) {
		struct gb_in in;

		if (gb_in_recv(&in, &t->chan.req, -1))
			return -1;
		struct gb_cmd_hdr h;
		if (in.size >= sizeof(h))
			memcpy(&h, in.p, sizeof(h));
		else
			h.flags = 0;
		if ((h.flags & GB_CMD_F_UPCALL) && (h.flags & GB_CMD_F_REPLY) &&
		    h.seq == seq) {
			uint32_t n = in.size - (uint32_t)sizeof(h);

			if (n > outmax)
				n = outmax;
			if (out && n)
				memcpy(out, in.p + sizeof(h), n);
			result = (int)n;
			gb_in_release(&in);
			return result;
		}
		/* An ordinary request that was already queued: it must run
		 * after this callback returns, not inside the driver. */
		struct gb_deferred *d = malloc(sizeof(*d) + in.size);
		if (d) {
			d->next = NULL;
			d->size = in.size;
			memcpy(d->data, in.p, in.size);
			if (t->defer_tail)
				t->defer_tail->next = d;
			else
				t->defer_head = d;
			t->defer_tail = d;
		}
		gb_in_release(&in);
	}
}

static void *worker_main(void *arg)
{
	struct gb_sth *t = arg;
	static int stats = -1;

	if (stats < 0)
		stats = getenv("GB_STATS") != NULL;
	/* GB_TRACE_OPS: one line per request on stderr (debugging aid) */
	int trace_ops = getenv("GB_TRACE_OPS") != NULL;
	/* GB_TRACE_ERRORS: name the request after which a GL error appeared
	 * (the error is put back, so the application still sees it) */
	int trace_errs = getenv("GB_TRACE_ERRORS") != NULL;

	t_this = t;
	for (;;) {
		struct gb_in in;
		struct gb_deferred *dcur = NULL;
		uint64_t t0 = stats ? mono_ns() : 0;
		int rc;

		if (t->defer_head) { /* requests read while awaiting an upcall */
			struct gb_deferred *d = t->defer_head;

			t->defer_head = d->next;
			if (!t->defer_head)
				t->defer_tail = NULL;
			memset(&in, 0, sizeof(in));
			in.p = d->data;
			in.size = d->size;
			in.heap = 0;
			rc = 0;
			dcur = d; /* freed after the message is handled */
		} else {
			rc = gb_in_recv(&in, &t->chan.req, -1);
		}
		uint64_t t1 = stats ? mono_ns() : 0;
		if (rc) {
			gb_log("channel %u ends (%d)", t->id, rc);
			break;
		}
		if (stats)
			t->st_idle_ns += t1 - t0;
		struct gb_cmd_hdr h;
		if (in.size < sizeof(h)) {
			if (dcur)
				free(dcur);
			else
				gb_in_release(&in);
			break;
		}
		memcpy(&h, in.p, sizeof(h)); /* copy: ring memory is untrusted */
		struct gb_r r;
		gb_r_init(&r, in.p, in.size);

		t_recent_cur = h.opcode;
		t_recent[t_recent_n++ % RECENT_OPS] = h.opcode;
		t->cur_req_sync = !!(h.flags & GB_CMD_F_WANT_REPLY);
		if (trace_ops) {
			char tb[96];
			int tn = snprintf(tb, sizeof(tb), "op %s\n",
					  h.opcode && h.opcode < GB_OP_COUNT
						  ? gb_op_names[h.opcode]
						  : "opx");
			(void)!write(2, tb, (size_t)tn);
		}
		if (h.size != in.size) {
			gb_sv_proto_error(t, &h);
		} else if (h.opcode > 0 && h.opcode < GB_OP_COUNT &&
			   gb_srv_table[h.opcode]) {
			gb_srv_table[h.opcode](t, &h, &r);
		} else if (h.opcode > GB_OPX_BASE && h.opcode < GB_OPX_END) {
			gb_srv_manual_dispatch(t, &h, &r);
		} else {
			gb_sv_proto_error(t, &h);
		}
		if (trace_errs && h.opcode < GB_OP_COUNT) {
			struct gb_errs e;

			gb_sv_err_pop(&e);
			for (int i = 0; i < e.n; i++) {
				char tb[128];
				int tn = snprintf(tb, sizeof(tb),
						  "ERR 0x%x after %s\n", e.e[i],
						  h.opcode ? gb_op_names[h.opcode]
							   : "opx");
				(void)!write(2, tb, (size_t)tn);
			}
			gb_sv_err_raise(&e);
		}
		gb_tmp_reset(t);
		gb_sv_fd_release(t);
		gb_sv_upcall_flush(t); /* messages from asynchronous requests */
		if (dcur)
			free(dcur);
		else
			gb_in_release(&in);
		if (stats) {
			uint64_t dt = mono_ns() - t1;
			t->st_busy_ns += dt;
			t->st_msgs++;
			if (dt > 200000) { /* handlers taking > 200 us */
				t->st_slow_ns += dt;
				t->st_slow_msgs++;
			}
		}
	}
	gb_srv_scratch_free(t);
	/* the client's channel is over: give back its descriptor and mapping
	 * (a long-lived server serves thousands of short-lived client threads) */
	gb_chan_destroy(&t->chan);
	return NULL;
}

static struct gb_sth *g_threads[GB_MAX_SERVER_THREADS];
static int g_nthreads;

struct gb_sth *gb_sv_thread(int i)
{
	return i >= 0 && i < g_nthreads ? g_threads[i] : NULL;
}

static void report_native(void)
{
	uint64_t total = 0, calls = 0;
	char used[GB_OP_COUNT] = { 0 };

	for (unsigned i = 0; i < GB_OP_COUNT; i++) {
		total += g_native[i].ns;
		calls += g_native[i].calls;
	}
	fprintf(stderr, "gbd native driver time: %.1f ms over %llu calls "
		"(%.2f us/call)\n  top by time:\n", total / 1e6,
		(unsigned long long)calls,
		calls ? total / 1e3 / (double)calls : 0.0);
	for (int n = 0; n < 12; n++) {
		int best = -1;
		uint64_t bv = 0;
		for (unsigned i = 0; i < GB_OP_COUNT; i++)
			if (!used[i] && g_native[i].ns > bv) {
				bv = g_native[i].ns;
				best = (int)i;
			}
		if (best < 0)
			break;
		used[best] = 1;
		fprintf(stderr, "   %-32s %9llu calls %9.1f ms %7.2f us/call\n",
			gb_op_names[best],
			(unsigned long long)g_native[best].calls, bv / 1e6,
			bv / 1e3 / (double)g_native[best].calls);
	}
}

static void report_stats(void)
{
	if (!getenv("GB_STATS"))
		return;
	report_native();
	for (int i = 0; i < g_nthreads; i++) {
		struct gb_sth *t = g_threads[i];
		clockid_t cid;
		struct timespec cpu = { 0, 0 };
		if (!pthread_getcpuclockid(t->thread, &cid))
			clock_gettime(cid, &cpu);
		fprintf(stderr, "gbd thread %u: CPU time actually used %.1f ms "
			"(vs busy wall time below)\n", t->id,
			cpu.tv_sec * 1e3 + cpu.tv_nsec / 1e6);
		fprintf(stderr, "gbd thread %u: %llu msgs, busy %.1f ms, "
			"idle (waiting for client) %.1f ms, %.2f us/msg; "
			"%llu slow msgs (>200us) took %.1f ms\n", t->id,
			(unsigned long long)t->st_msgs, t->st_busy_ns / 1e6,
			t->st_idle_ns / 1e6,
			t->st_msgs ? t->st_busy_ns / 1e3 / (double)t->st_msgs
				   : 0.0,
			(unsigned long long)t->st_slow_msgs,
			t->st_slow_ns / 1e6);
	}
}

/* ---- control loop --------------------------------------------------------- */

static int serve(int sock)
{
	struct gb_ctl_msg m, reply = { .magic = GB_CTL_MAGIC };
	uint32_t next_id = 1;
	int fd;

	g_ctl_sock = sock;

	for (;;) {
		fd = -1;
		int rc = gb_ctl_recv(sock, &m, &fd);
		if (rc == -ECONNRESET || rc == -EPIPE) {
			gb_log("client gone, exiting");
			report_stats();
			return 0;
		}
		if (rc) {
			gb_log("ctl recv: %d", rc);
			return 1;
		}
		reply.seq = m.seq;
		reply.status = 0;
		memset(reply.a, 0, sizeof(reply.a));

		switch (m.type) {
		case GB_CTL_HELLO:
			reply.type = GB_CTL_HELLO_REPLY;
			reply.a[0] = GB_PROTO_VERSION;
			reply.a[1] = sizeof(void *) * 8;
			reply.a[2] = GB_OPS_HASH;
			if (m.a[0] != GB_PROTO_VERSION || m.a[2] != GB_OPS_HASH) {
				gb_log("version/ops-hash mismatch (client %llu/%08llx)",
				       (unsigned long long)m.a[0],
				       (unsigned long long)m.a[2]);
				reply.status = -EPROTO;
			}
			gb_ctl_send(sock, &reply, -1);
			break;

		case GB_CTL_NEW_CHANNEL: {
			reply.type = GB_CTL_NEW_CHANNEL_REPLY;
			struct gb_sth *t = calloc(1, sizeof(*t));
			if (fd < 0 || !t) {
				reply.status = -EINVAL;
				free(t);
			} else {
				int arc = gb_chan_attach(&t->chan, fd,
							 (size_t)m.a[0]);
				if (arc) {
					reply.status = arc;
					close(fd);
					free(t);
				} else {
					t->id = next_id++;
					pthread_mutex_init(&t->up_lock, NULL);
					if (g_nthreads < GB_MAX_SERVER_THREADS)
						g_threads[g_nthreads++] = t;
					reply.a[0] = t->id;
					pthread_create(&t->thread, NULL,
						       worker_main, t);
					pthread_detach(t->thread);
				}
			}
			gb_ctl_send(sock, &reply, -1);
			break;
		}
		case GB_CTL_FD_PUT:
			if (fd >= 0)
				fd_store((uint32_t)m.a[0], fd);
			break;
		case GB_CTL_BYE:
			return 0;
		default:
			if (fd >= 0)
				close(fd);
			break;
		}
	}
}

int main(int argc, char **argv)
{
	int sock = -1;

	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--fd"))
			sock = atoi(argv[i + 1]);
	if (sock < 0) {
		fprintf(stderr, "usage: gbd --fd <control-socket-fd>\n"
				"(started by the gpu-bridge client library)\n");
		return 2;
	}
	g_debug = getenv("GB_DEBUG") != NULL;
	install_crash_handler();
	gb_stats_on = getenv("GB_STATS") != NULL;

	if (gb_native_init())
		return 3;
	int rc = serve(sock);

	/*
	 * The client is gone (or the protocol broke). Leave at once: a worker
	 * may be inside a driver call that never returns (a pathological
	 * shader link, minutes at 1 GB) and the driver's atexit teardown would
	 * wait for it, leaving an orphaned server that holds the test's pipes
	 * and its memory.
	 */
	fflush(NULL);
	_exit(rc);
}

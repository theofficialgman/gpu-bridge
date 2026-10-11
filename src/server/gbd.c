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
	int rc = gb_out_begin(o, &t->chan.rsp, size);
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

static void *worker_main(void *arg)
{
	struct gb_sth *t = arg;
	static int stats = -1;

	if (stats < 0)
		stats = getenv("GB_STATS") != NULL;
	/* GB_TRACE_OPS: one line per request on stderr (debugging aid) */
	int trace_ops = getenv("GB_TRACE_OPS") != NULL;

	for (;;) {
		struct gb_in in;
		uint64_t t0 = stats ? mono_ns() : 0;
		int rc = gb_in_recv(&in, &t->chan.req, -1);
		uint64_t t1 = stats ? mono_ns() : 0;
		if (rc) {
			gb_log("channel %u ends (%d)", t->id, rc);
			break;
		}
		if (stats)
			t->st_idle_ns += t1 - t0;
		struct gb_cmd_hdr h;
		if (in.size < sizeof(h)) {
			gb_in_release(&in);
			break;
		}
		memcpy(&h, in.p, sizeof(h)); /* copy: ring memory is untrusted */
		struct gb_r r;
		gb_r_init(&r, in.p, in.size);

		t_recent_cur = h.opcode;
		t_recent[t_recent_n++ % RECENT_OPS] = h.opcode;
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
		gb_tmp_reset(t);
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
	return NULL;
}

static struct gb_sth *g_threads[64];
static int g_nthreads;

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
					if (g_nthreads < 64)
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
	return serve(sock);
}

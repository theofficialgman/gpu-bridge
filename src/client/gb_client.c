/*
 * gb_client.c - connection runtime of the client library.
 *
 * Lazily (on the first API call): create a socketpair, spawn `gbd`, do the
 * HELLO handshake. Each calling thread gets its own channel (request +
 * response ring in a memfd) and a matching server worker thread.
 *
 * Environment:
 *   GB_SERVER        path of the gbd executable (default: next to this
 *                    library, then /usr/libexec/gpu-bridge/gbd)
 *   GB_DEBUG=1       log connection events and protocol errors
 *   GB_LOG_MISSING=1 log each unsupported entry point on first use
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <time.h>

#define GB_OPS_NAMES 1 /* want gb_op_names[] for GB_STATS */
#include "gb_client_impl.h"

extern char **environ;

static struct {
	pthread_mutex_t mu;
	int sock;
	pid_t pid;
	uint32_t gen;
	int failed;
	int debug, log_missing;
	pthread_key_t key;
	int key_made;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .sock = -1 };

static __thread struct gb_cl *t_cl;

static void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbg(const char *fmt, ...)
{
	if (!G.debug)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "gb32[%d]: ", (int)getpid());
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void thread_exit_cleanup(void *p)
{
	struct gb_cl *c = p;

	if (!c)
		return;
	if (c->gen == G.gen) {
		gb_chan_close(&c->chan); /* lets the server worker exit */
		gb_chan_destroy(&c->chan);
	}
	free(c);
}

static void atfork_child(void)
{
	/* The child must not share the parent's server or rings: bump the
	 * generation so every inherited channel is treated as stale and the
	 * child reconnects (spawning its own gbd) on first use. */
	G.gen++;
	G.sock = -1;
	G.pid = 0;
	G.failed = 0;
	pthread_mutex_init(&G.mu, NULL);
}

static int find_server(char *out, size_t cap)
{
	const char *env = getenv("GB_SERVER");
	Dl_info di;

	if (env) {
		snprintf(out, cap, "%s", env);
		return access(out, X_OK);
	}
	if (dladdr((void *)gb_cl_get, &di) && di.dli_fname) {
		char tmp[4096];
		snprintf(tmp, sizeof(tmp), "%s", di.dli_fname);
		snprintf(out, cap, "%s/gbd", dirname(tmp));
		if (!access(out, X_OK))
			return 0;
	}
	snprintf(out, cap, "/usr/libexec/gpu-bridge/gbd");
	return access(out, X_OK);
}

/* gbd refuses to use any "native" GL library that exports this symbol: it is
 * one of ours (e.g. our libGL.so.1 shim found via LD_LIBRARY_PATH), and
 * loading it would make the server spawn a server, recursively. */
GB_API const char gb_bridge_client_marker[] = "gpu-bridge client library";

/* Copy of the environment without this library's directory in
 * LD_LIBRARY_PATH, so the server loads the real drivers, not us. */
static char **server_environ(void)
{
	Dl_info di;
	char dir[4096] = "";
	size_t n = 0, i, j;
	char **env;

	if (dladdr((void *)gb_cl_get, &di) && di.dli_fname) {
		char tmp[4096];
		snprintf(tmp, sizeof(tmp), "%s", di.dli_fname);
		snprintf(dir, sizeof(dir), "%s", dirname(tmp));
	}
	while (environ[n])
		n++;
	env = calloc(n + 1, sizeof(char *));
	if (!env)
		return environ;
	for (i = j = 0; i < n; i++) {
		const char *e = environ[i];
		if (!strncmp(e, "LD_LIBRARY_PATH=", 16) && dir[0]) {
			char *v = strdup(e + 16), *out, *save = NULL, *tok;
			size_t cap = strlen(e) + 32;
			out = malloc(cap);
			if (!v || !out) {
				env[j++] = (char *)e;
				free(v);
				free(out);
				continue;
			}
			snprintf(out, cap, "LD_LIBRARY_PATH=");
			int first = 1;
			for (tok = strtok_r(v, ":", &save); tok;
			     tok = strtok_r(NULL, ":", &save)) {
				if (!strcmp(tok, dir))
					continue;
				if (!first)
					strncat(out, ":", cap - strlen(out) - 1);
				strncat(out, tok, cap - strlen(out) - 1);
				first = 0;
			}
			free(v);
			env[j++] = out;
			continue;
		}
		env[j++] = (char *)e;
	}
	env[j] = NULL;
	return env;
}

static int connect_locked(void)
{
	int sv[2];
	char path[4352], fdarg[16];

	if (find_server(path, sizeof(path))) {
		fprintf(stderr, "gpu-bridge: cannot find gbd (set GB_SERVER)\n");
		return -1;
	}
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0)
		return -1;
	fcntl(sv[0], F_SETFD, FD_CLOEXEC);

	snprintf(fdarg, sizeof(fdarg), "%d", sv[1]);
	char *argv[] = { path, (char *)"--fd", fdarg, NULL };
	char **senv = server_environ();
	int rc = posix_spawn(&G.pid, path, NULL, NULL, argv, senv);
	if (senv != environ)
		free(senv); /* leaks the rewritten LD_LIBRARY_PATH string once */
	close(sv[1]);
	if (rc) {
		close(sv[0]);
		fprintf(stderr, "gpu-bridge: spawn %s failed: %d\n", path, rc);
		return -1;
	}
	dbg("spawned %s pid %d", path, (int)G.pid);

	struct gb_ctl_msg m = { .magic = GB_CTL_MAGIC, .type = GB_CTL_HELLO };
	m.a[0] = GB_PROTO_VERSION;
	m.a[1] = sizeof(void *) * 8;
	m.a[2] = GB_OPS_HASH;
	if (gb_ctl_send(sv[0], &m, -1) || gb_ctl_recv(sv[0], &m, NULL) ||
	    m.type != GB_CTL_HELLO_REPLY || m.status) {
		fprintf(stderr, "gpu-bridge: server handshake failed "
				"(server not started or version mismatch)\n");
		close(sv[0]);
		waitpid(G.pid, NULL, WNOHANG);
		return -1;
	}
	dbg("connected; server is %llu-bit", (unsigned long long)m.a[1]);
	G.sock = sv[0];
	return 0;
}

/* ---- control-socket mailbox -------------------------------------------------
 * Several kinds of message come back on the control socket (channel replies,
 * fds the server returns); whoever reads one that is not theirs stashes it. */
static pthread_mutex_t g_ctl_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	struct gb_ctl_msg m;
	int fd;
	int used;
} g_stash[16];

static int ctl_wait(uint32_t type, uint64_t token, struct gb_ctl_msg *out,
		    int *fd_out)
{
	int rc = 0;

	pthread_mutex_lock(&g_ctl_mu);
	for (;;) {
		for (unsigned i = 0; i < 16; i++)
			if (g_stash[i].used && g_stash[i].m.type == type &&
			    (type != GB_CTL_FD_GIVE || g_stash[i].m.a[0] == token)) {
				*out = g_stash[i].m;
				if (fd_out)
					*fd_out = g_stash[i].fd;
				else if (g_stash[i].fd >= 0)
					close(g_stash[i].fd);
				g_stash[i].used = 0;
				goto out;
			}
		struct gb_ctl_msg m;
		int fd = -1;

		rc = gb_ctl_recv(G.sock, &m, &fd);
		if (rc)
			goto out;
		if (m.type == type &&
		    (type != GB_CTL_FD_GIVE || m.a[0] == token)) {
			*out = m;
			if (fd_out)
				*fd_out = fd;
			else if (fd >= 0)
				close(fd);
			goto out;
		}
		unsigned i;
		for (i = 0; i < 16 && g_stash[i].used; i++)
			;
		if (i == 16) { /* nobody collects these: drop the oldest */
			if (g_stash[0].fd >= 0)
				close(g_stash[0].fd);
			i = 0;
		}
		g_stash[i].m = m;
		g_stash[i].fd = fd;
		g_stash[i].used = 1;
	}
out:
	pthread_mutex_unlock(&g_ctl_mu);
	return rc;
}

/* Send a client fd to the server; the returned token names it in the
 * requests that follow (0 on failure). */
uint32_t gb_cl_fd_put(int fd)
{
	static uint32_t next;
	struct gb_ctl_msg m = { .magic = GB_CTL_MAGIC, .type = GB_CTL_FD_PUT };
	uint32_t tok = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);

	if (fd < 0 || !gb_cl_get())
		return 0;
	m.a[0] = tok;
	return gb_ctl_send(G.sock, &m, fd) ? 0 : tok;
}

/* Receive an fd the server returned under `token`; -1 on failure. */
int gb_cl_fd_get(uint32_t token)
{
	struct gb_ctl_msg m;
	int fd = -1;

	if (!token || ctl_wait(GB_CTL_FD_GIVE, token, &m, &fd))
		return -1;
	return fd;
}

static struct gb_cl *new_channel_locked(void)
{
	struct gb_cl *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	if (gb_chan_create(&c->chan, GB_CHAN_RING_BYTES, GB_CHAN_RING_BYTES)) {
		free(c);
		return NULL;
	}
	struct gb_ctl_msg m = { .magic = GB_CTL_MAGIC,
				.type = GB_CTL_NEW_CHANNEL };
	m.a[0] = c->chan.region_size;
	if (gb_ctl_send(G.sock, &m, c->chan.fd) ||
	    ctl_wait(GB_CTL_NEW_CHANNEL_REPLY, 0, &m, NULL) || m.status) {
		gb_chan_destroy(&c->chan);
		free(c);
		return NULL;
	}
	c->chan.req.watch_fd = c->chan.rsp.watch_fd = G.sock;
	c->gen = G.gen;
	return c;
}

struct gb_cl *gb_cl_get(void)
{
	struct gb_cl *c = t_cl;

	if (c && c->gen == G.gen)
		return c->lost ? NULL : c;

	pthread_mutex_lock(&G.mu);
	if (!G.key_made) {
		pthread_key_create(&G.key, thread_exit_cleanup);
		G.debug = getenv("GB_DEBUG") != NULL;
		G.log_missing = getenv("GB_LOG_MISSING") != NULL;
		pthread_atfork(NULL, NULL, atfork_child);
		G.key_made = 1;
	}
	if (G.failed) {
		pthread_mutex_unlock(&G.mu);
		return NULL;
	}
	if (G.sock < 0 && connect_locked()) {
		G.failed = 1;
		pthread_mutex_unlock(&G.mu);
		return NULL;
	}
	c = new_channel_locked();
	pthread_mutex_unlock(&G.mu);
	if (!c) {
		dbg("could not create channel");
		return NULL;
	}
	/* A stale channel from before a fork is leaked on purpose: it shares
	 * memory with the parent's server and must not be touched. */
	t_cl = c;
	pthread_setspecific(G.key, c);
	return c;
}

/* ---- GB_STATS: per-opcode call/stall accounting -------------------------------- */

#define STAT_SLOTS (GB_OP_COUNT + 64)
static int g_stats_on = -1;
static struct {
	uint64_t calls, syncs, wait_ns, bytes;
} g_stat[STAT_SLOTS];

static inline unsigned stat_slot(uint32_t op)
{
	if (op >= GB_OPX_BASE)
		return GB_OP_COUNT + (op - GB_OPX_BASE) % 64;
	return op < GB_OP_COUNT ? op : 0;
}

static inline uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *stat_name(unsigned slot)
{
	static const char *const opx[] = { "Opx_GetDisplay", "Opx_QueryString",
		"Opx_GetString", "Opx_ShaderSource", "Opx_DrawClient",
		"Opx_CreateWindowSurface", "Opx_OpenXDisplay", "Opx_GLXString",
		"Opx_GLXFBConfigs", "Opx_BufferRead", "Opx_EglDevice", "Opx_DeviceString",
		"Opx_SelectBuffer", "Opx_RenderMode", "Opx_Ping",
		"Opx_DebugCallback", "Opx_DebugStr", "Opx_BlobFuncs",
		"Opx_EglDebug", "Opx_ObjPtrLabel", "Opx_DriverString", "Opx_FdOp",
		"Opx_ObjString", "Opx_ListDrawClient",
		"Opx_ElementMax" };
	if (slot < GB_OP_COUNT)
		return gb_op_names[slot];
	unsigned i = slot - GB_OP_COUNT;
	return i < sizeof(opx) / sizeof(opx[0]) ? opx[i] : "?";
}

static void stats_report(void)
{
	if (g_stats_on != 1)
		return;
	uint64_t tc = 0, ts = 0, tw = 0;
	for (unsigned i = 0; i < STAT_SLOTS; i++) {
		tc += g_stat[i].calls;
		ts += g_stat[i].syncs;
		tw += g_stat[i].wait_ns;
	}
	fprintf(stderr, "\n== gpu-bridge stats: %llu calls, %llu synchronous, "
		"%.1f ms waiting on replies ==\n", (unsigned long long)tc,
		(unsigned long long)ts, tw / 1e6);
	/* top by wait time, then by call count */
	for (int pass = 0; pass < 2; pass++) {
		fprintf(stderr, "%s\n", pass ? " most called:" :
			" most time stalled on replies:");
		char used[STAT_SLOTS] = { 0 };
		for (int n = 0; n < 10; n++) {
			int best = -1;
			uint64_t bv = 0;
			for (unsigned i = 0; i < STAT_SLOTS; i++) {
				uint64_t v = pass ? g_stat[i].calls
						  : g_stat[i].wait_ns;
				if (!used[i] && v > bv) {
					bv = v;
					best = (int)i;
				}
			}
			if (best < 0)
				break;
			used[best] = 1;
			fprintf(stderr, "   %-34s calls %9llu  sync %9llu  "
				"wait %9.1f ms  %8.1f KB\n",
				stat_name((unsigned)best),
				(unsigned long long)g_stat[best].calls,
				(unsigned long long)g_stat[best].syncs,
				g_stat[best].wait_ns / 1e6,
				g_stat[best].bytes / 1024.0);
		}
	}
}

__attribute__((destructor)) static void stats_dtor(void)
{
	stats_report();
}

int gb_cl_begin(struct gb_cl *cx, struct gb_out *o, uint32_t op,
		uint32_t total, uint32_t flags)
{
	if (__builtin_expect(g_stats_on != 0, 0)) {
		if (g_stats_on < 0)
			g_stats_on = getenv("GB_STATS") != NULL;
		if (g_stats_on) {
			unsigned s = stat_slot(op);
			g_stat[s].calls++;
			g_stat[s].bytes += total;
			if (flags & GB_CMD_F_WANT_REPLY)
				g_stat[s].syncs++;
		}
	}
	int rc;

	if (__builtin_expect(gb_ring_peek(&cx->chan.rsp, &(uint32_t){0}) != NULL, 0) &&
	    !cx->in_upcall)
		gb_cl_poll_upcalls(cx);
	rc = gb_out_begin(o, &cx->chan.req, total);

	if (rc) {
		cx->lost = 1;
		dbg("send failed (%d): server lost", rc);
		return rc;
	}
	struct gb_cmd_hdr h = { op, total, ++cx->chan.seq, flags };
	memcpy(o->p, &h, sizeof(h));
	return 0;
}

/* ---- server -> client upcalls ------------------------------------------------- */

static void upcall_reply(struct gb_cl *cx, uint32_t id, uint32_t seq,
			 const void *data, uint32_t len)
{
	struct gb_out o;
	uint32_t size = GB_ALIGN_UP((uint32_t)sizeof(struct gb_cmd_hdr) + len, 8u);

	if (gb_out_begin(&o, &cx->chan.req, size)) {
		cx->lost = 1;
		return;
	}
	struct gb_cmd_hdr h = { id, size, seq, GB_CMD_F_UPCALL | GB_CMD_F_REPLY };
	memset(o.p, 0, size);
	memcpy(o.p, &h, sizeof(h));
	if (len)
		memcpy(o.p + sizeof(h), data, len);
	if (gb_out_end(&o))
		cx->lost = 1;
}

/* blob cache callbacks, set with eglSetBlobCacheFuncsANDROID (gb_client_gl.c) */
void gb_cl_blob_set(const void *key, long ksz, const void *val, long vsz);
long gb_cl_blob_get(const void *key, long ksz, void *val, long vmax);
void gb_cl_egl_debug_dispatch(uint32_t error, const char *command, uint32_t type,
			      uint64_t thread_label, uint64_t object_label,
			      const char *message);

static void dispatch_upcall(struct gb_cl *cx, const struct gb_in *in)
{
	struct gb_cmd_hdr h;
	struct gb_r r;

	memcpy(&h, in->p, sizeof(h));
	gb_r_init(&r, in->p, in->size); /* the reader skips the header itself */
	if (getenv("GB_TRACE_OPS"))
		fprintf(stderr, "c: upcall %#x size %u\n", h.opcode, in->size);
	cx->in_upcall++;
	switch (h.opcode) {
	case GB_UP_DebugMessage: {
		uint32_t src = gb_r_u32(&r), type = gb_r_u32(&r),
			 id = gb_r_u32(&r), sev = gb_r_u32(&r),
			 n = gb_r_u32(&r);
		(void)gb_r_u32(&r); /* pad: the blob below is 8-aligned */
		const char *msg = n ? gb_r_blob(&r, n) : "";
		struct gb_ctxstate *st = cx->cur;

		if (!r.err && msg && st && st->debug_cb)
			st->debug_cb(src, type, id, sev, (GLsizei)n, msg,
				     st->debug_user);
		break;
	}
	case GB_UP_EglDebug: {
		uint32_t error = gb_r_u32(&r), type = gb_r_u32(&r),
			 clen = gb_r_u32(&r), mlen = gb_r_u32(&r);
		uint64_t tl = gb_r_u64(&r), ol = gb_r_u64(&r);
		const char *cmd = gb_r_blob(&r, clen);
		const char *msg = gb_r_blob(&r, mlen);

		if (!r.err && cmd && msg)
			gb_cl_egl_debug_dispatch(error, cmd, type, tl, ol, msg);
		break;
	}
	case GB_UP_BlobSet: {
		uint32_t ksz = gb_r_u32(&r), vsz = gb_r_u32(&r);
		const void *k = gb_r_blob(&r, ksz);
		const void *v = gb_r_blob(&r, vsz);

		if (!r.err && k && v)
			gb_cl_blob_set(k, ksz, v, vsz);
		break;
	}
	case GB_UP_BlobGet: { /* blocking */
		uint32_t ksz = gb_r_u32(&r), vmax = gb_r_u32(&r);
		const void *k = gb_r_blob(&r, ksz);
		uint8_t *buf = NULL;
		long n = 0;

		if (!r.err && k && vmax <= (1u << 20) &&
		    (buf = malloc(vmax + 8)))
			n = gb_cl_blob_get(k, ksz, buf, vmax);
		if (n < 0 || (uint32_t)n > vmax)
			n = 0;
		if (h.flags & GB_CMD_F_WANT_REPLY)
			upcall_reply(cx, h.opcode, h.seq, buf, (uint32_t)n);
		free(buf);
		break;
	}
	default:
		if (h.flags & GB_CMD_F_WANT_REPLY)
			upcall_reply(cx, h.opcode, h.seq, NULL, 0);
		break;
	}
	cx->in_upcall--;
}

/* Deliver whatever the server has queued; never waits. Called at the start
 * of every call (cheap: one ring peek) and while waiting for a reply. */
void gb_cl_poll_upcalls(struct gb_cl *cx)
{
	for (;;) {
		struct gb_in in;
		uint32_t sz;
		const void *rec = gb_ring_peek(&cx->chan.rsp, &sz);

		if (!rec)
			return;
		if (gb_in_recv(&in, &cx->chan.rsp, 0))
			return;
		struct gb_cmd_hdr h;
		memcpy(&h, in.p, sizeof(h));
		if (h.flags & GB_CMD_F_UPCALL)
			dispatch_upcall(cx, &in);
		/* anything else cannot be pending: no request is outstanding */
		gb_in_release(&in);
	}
}

void gb_cl_end(struct gb_cl *cx, struct gb_out *o)
{
	uint32_t flags = ((const struct gb_cmd_hdr *)o->p)->flags;

	if (gb_out_end(o))
		cx->lost = 1;
	/* GL_DEBUG_OUTPUT_SYNCHRONOUS: the application expects its callback
	 * before the offending call returns; async calls are made to
	 * synchronise (the reply is preceded by the call's debug messages). */
	if (__builtin_expect(cx->sync_dbg, 0) && !cx->in_upcall &&
	    !(flags & GB_CMD_F_WANT_REPLY) && !cx->lost) {
		struct gb_out po;
		struct gb_w pw;
		struct gb_in pin;

		if (!gb_cl_msg_begin(cx, &po, &pw, GB_OPX_Ping, 16,
				     GB_CMD_F_WANT_REPLY)) {
			gb_cl_end(cx, &po);
			if (!gb_cl_wait_reply(cx, &pin, GB_OPX_Ping))
				gb_in_release(&pin);
		}
	}
}

int gb_cl_wait_reply(struct gb_cl *cx, struct gb_in *in, uint32_t op)
{
	uint64_t t0 = g_stats_on == 1 ? mono_ns() : 0;
	int rc;
	struct gb_cmd_hdr h;

	for (;;) {
		rc = gb_in_recv(in, &cx->chan.rsp, -1);
		if (rc)
			break;
		memcpy(&h, in->p, sizeof(h));
		if (!(h.flags & GB_CMD_F_UPCALL))
			break;
		dispatch_upcall(cx, in); /* callbacks run in the calling thread */
		gb_in_release(in);
	}

	if (g_stats_on == 1)
		g_stat[stat_slot(op)].wait_ns += mono_ns() - t0;

	if (rc) {
		cx->lost = 1;
		dbg("reply wait failed (%d): server lost", rc);
		return rc;
	}
	if (!(h.flags & GB_CMD_F_REPLY) || h.seq != cx->chan.seq ||
	    h.opcode != op) {
		dbg("reply desync: op %u/%u seq %u/%u", h.opcode, op, h.seq,
		    cx->chan.seq);
		gb_in_release(in);
		cx->lost = 1;
		return -EPROTO;
	}
	return 0;
}

void gb_cl_set_error(struct gb_cl *cx, uint32_t op)
{
	dbg("request/reply error for opcode %u", op);
}

void gb_cl_unsupported(const char *what)
{
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	static char seen[256][48];
	static int n;

	if (!G.log_missing && !getenv("GB_LOG_MISSING"))
		return;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < n; i++)
		if (!strncmp(seen[i], what, sizeof(seen[i]) - 1)) {
			pthread_mutex_unlock(&mu);
			return;
		}
	if (n < 256)
		snprintf(seen[n++], sizeof(seen[0]), "%s", what);
	pthread_mutex_unlock(&mu);
	fprintf(stderr, "gpu-bridge: unsupported: %s\n", what);
}

/* ---- locally generated errors / unsupported entry points -------------------- */

void gb_cl_gl_error(uint32_t e)
{
	struct gb_cl *cx = gb_cl_get();
	if (getenv("GB_TRACE_OPS"))
		fprintf(stderr, "c: local GL error %#x (pending %#x)\n", e,
			cx ? cx->gl_error : 0);
	if (cx && !cx->gl_error)
		cx->gl_error = e;
}

void gb_cl_egl_error(int32_t e)
{
	struct gb_cl *cx = gb_cl_get();
	if (cx && !cx->egl_error)
		cx->egl_error = e;
}

void gb_cl_unsupported_egl(const char *what)
{
	gb_cl_unsupported(what);
	gb_cl_egl_error(0x3003 /* EGL_BAD_ALLOC */);
}

void gb_cl_unsupported_gl(const char *what)
{
	gb_cl_unsupported(what);
	gb_cl_gl_error(0x0502 /* GL_INVALID_OPERATION */);
}

void gb_cl_unsupported_glx(const char *what)
{
	gb_cl_unsupported(what);
}

int gb_cl_msg_begin(struct gb_cl *cx, struct gb_out *o, struct gb_w *w,
		    uint32_t op, uint32_t size, uint32_t flags)
{
	if (gb_cl_begin(cx, o, op, GB_ALIGN_UP(size, 8u), flags))
		return -1;
	gb_w_init(w, o->p);
	return 0;
}

/* ---- interned strings and filtered extension lists ----------------------------- */

struct istr {
	struct istr *next;
	char s[];
};
static struct istr *g_istr;
static pthread_mutex_t g_str_mu = PTHREAD_MUTEX_INITIALIZER;

const char *gb_cl_intern(const char *s, size_t len)
{
	pthread_mutex_lock(&g_str_mu);
	for (struct istr *i = g_istr; i; i = i->next)
		if (!strncmp(i->s, s, len) && i->s[len] == 0) {
			pthread_mutex_unlock(&g_str_mu);
			return i->s;
		}
	struct istr *n = malloc(sizeof(*n) + len + 1);
	if (!n) {
		pthread_mutex_unlock(&g_str_mu);
		return NULL;
	}
	memcpy(n->s, s, len);
	n->s[len] = 0;
	n->next = g_istr;
	g_istr = n;
	pthread_mutex_unlock(&g_str_mu);
	return n->s;
}

const char *gb_cl_fetch_string(uint32_t op, uint32_t a, uint32_t b)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx || gb_cl_msg_begin(cx, &o, &w, op, 28, GB_CMD_F_WANT_REPLY))
		return NULL;
	gb_w_u32(&w, a);
	gb_w_u32(&w, b);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, op))
		return NULL;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t len = gb_r_u32(&r);
	const char *res = NULL;
	if (len != GB_NULL_LEN && len > 0 && len <= (1u << 20)) {
		const char *s = gb_r_blob(&r, len);
		if (s && !r.err && s[len - 1] == 0)
			res = gb_cl_intern(s, len - 1);
	}
	gb_in_release(&in);
	return res;
}

static int proc_present(const char *name)
{
	for (const struct gb_proc *p = gb_proc_table; p->name; p++)
		if (!strcmp(p->name, name))
			return 1;
	return 0;
}

/* An extension is advertised only if the registry knows it and every
 * command it adds is implemented. Unknown extensions are dropped. */
static int ext_allowed(const char *name, size_t len)
{
	for (const struct gb_ext *e = gb_ext_table; e->name; e++) {
		if (strlen(e->name) != len || strncmp(e->name, name, len))
			continue;
		for (int i = 0; i < e->ncmds; i++)
			if (!proc_present(e->cmds[i]))
				return 0;
		return 1;
	}
	return 0;
}

const char *gb_cl_filter_ext(const char *list)
{
	size_t cap = strlen(list) + 1;
	char *out = malloc(cap);
	size_t o = 0;

	if (!out)
		return NULL;
	out[0] = 0;
	for (const char *p = list; *p;) {
		while (*p == ' ')
			p++;
		const char *s = p;
		while (*p && *p != ' ')
			p++;
		size_t len = (size_t)(p - s);
		if (len && ext_allowed(s, len)) {
			if (o)
				out[o++] = ' ';
			memcpy(out + o, s, len);
			o += len;
			out[o] = 0;
		}
	}
	const char *r = gb_cl_intern(out, o);
	free(out);
	return r;
}

/* Filtered GL extension list, cached per context id. */
static unsigned g_ext_gen; /* bumped when a context is destroyed */

void gb_cl_ext_list_invalidate(void)
{
	__atomic_add_fetch(&g_ext_gen, 1, __ATOMIC_RELAXED);
}

const struct gb_extlist *gb_cl_ext_list(void)
{
	static struct {
		uint32_t ctx_id;
		unsigned gen;
		struct gb_extlist l;
	} cache;
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	struct gb_ctxstate *st = gb_cur_state();
	uint32_t id = st ? st->ctx_id : 0;
	unsigned gen = __atomic_load_n(&g_ext_gen, __ATOMIC_RELAXED);

	pthread_mutex_lock(&mu);
	if (cache.l.joined && cache.ctx_id == id && cache.gen == gen) {
		pthread_mutex_unlock(&mu);
		return &cache.l;
	}
	pthread_mutex_unlock(&mu);

	const char *native = gb_cl_fetch_string(GB_OPX_GetString, 0x1F03,
						0xFFFFFFFFu);
	if (!native)
		return NULL;
	const char *joined = gb_cl_filter_ext(native);
	if (!joined)
		return NULL;

	int n = 0;
	for (const char *p = joined; *p; p++)
		if (*p == ' ')
			n++;
	n += joined[0] ? 1 : 0;
	const char **names = calloc((size_t)n + 1, sizeof(char *));
	int k = 0;
	for (const char *p = joined; *p && names;) {
		const char *s = p;
		while (*p && *p != ' ')
			p++;
		names[k++] = gb_cl_intern(s, (size_t)(p - s));
		if (*p == ' ')
			p++;
	}
	pthread_mutex_lock(&mu);
	free(cache.l.names);
	cache.ctx_id = id;
	cache.gen = gen;
	cache.l.joined = joined;
	cache.l.names = names;
	cache.l.n = names ? k : 0;
	pthread_mutex_unlock(&mu);
	return &cache.l;
}

/* ---- X11 helpers ------------------------------------------------------------------ */

const char *gb_cl_xdisplay_string(Display *dpy)
{
	static char *(*xds)(Display *);

	if (!xds)
		xds = (char *(*)(Display *))dlsym(RTLD_DEFAULT,
						  "XDisplayString");
	return xds && dpy ? xds(dpy) : NULL;
}

uint32_t gb_cl_xdpy_id(Display *dpy)
{
	static struct {
		Display *dpy;
		uint32_t id;
	} tab[16];
	static int next;
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	struct gb_cl *cx;

	if (!dpy)
		return 0;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < 16; i++)
		if (tab[i].dpy == dpy && tab[i].id) {
			uint32_t id = tab[i].id;
			pthread_mutex_unlock(&mu);
			return id;
		}
	pthread_mutex_unlock(&mu);

	const char *name = gb_cl_xdisplay_string(dpy);
	cx = gb_cl_get();
	if (!name || !cx)
		return 0;

	struct gb_out o;
	struct gb_w w;
	uint32_t slen = (uint32_t)strlen(name) + 1;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_OpenXDisplay,
			    GB_ALIGN_UP(20, 8u) + slen, GB_CMD_F_WANT_REPLY))
		return 0;
	gb_w_u32(&w, slen);
	memcpy(gb_w_blob(&w, slen), name, slen);
	gb_cl_end(cx, &o);

	struct gb_in in;
	if (gb_cl_wait_reply(cx, &in, GB_OPX_OpenXDisplay))
		return 0;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint32_t id = (uint32_t)gb_r_u64(&r);
	gb_in_release(&in);

	if (id) {
		pthread_mutex_lock(&mu);
		tab[next].dpy = dpy;
		tab[next].id = id;
		next = (next + 1) % 16;
		pthread_mutex_unlock(&mu);
	}
	return id;
}

XVisualInfo *gb_cl_xvisual(Display *dpy, uint32_t vid, uint32_t scr)
{
	static XVisualInfo *(*getvi)(Display *, long, XVisualInfo *, int *);
	XVisualInfo tmpl;
	int n = 0;

	if (!getvi)
		getvi = (XVisualInfo *(*)(Display *, long, XVisualInfo *,
					  int *))dlsym(RTLD_DEFAULT,
						       "XGetVisualInfo");
	if (!getvi || !dpy)
		return NULL;
	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.visualid = vid;
	(void)scr;
	return getvi(dpy, VisualIDMask, &tmpl, &n);
}

/*
 * X request ordering. The server renders through its *own* X connection, so
 * X's per-connection ordering guarantee does not cover "client creates a
 * window" -> "server renders into it". Xlib buffers requests, so before any
 * call that names an X resource we make sure everything the app has issued
 * on `dpy` has been *processed* by the X server (a round trip, only when
 * something is actually outstanding).
 */
void gb_cl_xsync(Display *dpy)
{
	static unsigned long (*next_req)(Display *);
	static unsigned long (*last_proc)(Display *);
	static int (*xsync)(Display *, int);

	if (!dpy)
		return;
	if (!xsync) {
		next_req = (unsigned long (*)(Display *))dlsym(
			RTLD_DEFAULT, "XNextRequest");
		last_proc = (unsigned long (*)(Display *))dlsym(
			RTLD_DEFAULT, "XLastKnownRequestProcessed");
		xsync = (int (*)(Display *, int))dlsym(RTLD_DEFAULT,
						       "XSync");
	}
	if (!xsync)
		return;
	if (next_req && last_proc && next_req(dpy) - 1 <= last_proc(dpy))
		return; /* nothing outstanding */
	xsync(dpy, 0);
}

/* ---- fault-tolerant copy of application memory ---------------------------- */

/* volatile: the compiler must not drop the store around a memcpy it knows
 * cannot touch the variable; only the fault handler reads it */
static __thread sigjmp_buf *volatile copy_jmp;
static struct sigaction copy_prev[2]; /* SIGSEGV, SIGBUS */
static pthread_once_t copy_once = PTHREAD_ONCE_INIT;

static void copy_fault(int sig, siginfo_t *si, void *uc)
{
	if (copy_jmp) {
		sigset_t s;

		sigemptyset(&s);
		sigaddset(&s, SIGSEGV);
		sigaddset(&s, SIGBUS);
		pthread_sigmask(SIG_UNBLOCK, &s, NULL);
		siglongjmp(*copy_jmp, 1);
	}
	/* not ours: hand it to whoever was installed before us */
	const struct sigaction *p = &copy_prev[sig == SIGBUS];

	if (p->sa_flags & SA_SIGINFO) {
		p->sa_sigaction(sig, si, uc);
	} else if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) {
		p->sa_handler(sig);
	} else {
		signal(sig, SIG_DFL);
		raise(sig);
	}
}

static void copy_install(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = copy_fault;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &copy_prev[0]);
	sigaction(SIGBUS, &sa, &copy_prev[1]);
}

int gb_cl_copy_in(void *dst, const void *src, size_t n)
{
	sigjmp_buf jb;
	sigjmp_buf *volatile saved;

	pthread_once(&copy_once, copy_install);
	saved = copy_jmp;
	if (sigsetjmp(jb, 0)) {
		copy_jmp = saved;
		/* the destination may be partly written: make the rest zero by
		 * clearing all of it (the data is not meaningful anyway) */
		memset(dst, 0, n);
		return -1;
	}
	copy_jmp = &jb;
	memcpy(dst, src, n);
	copy_jmp = saved;
	return 0;
}

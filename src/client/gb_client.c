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
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
	int rc = posix_spawn(&G.pid, path, NULL, NULL, argv, environ);
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
	    gb_ctl_recv(G.sock, &m, NULL) || m.status) {
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

int gb_cl_begin(struct gb_cl *cx, struct gb_out *o, uint32_t op,
		uint32_t total, uint32_t flags)
{
	int rc = gb_out_begin(o, &cx->chan.req, total);

	if (rc) {
		cx->lost = 1;
		dbg("send failed (%d): server lost", rc);
		return rc;
	}
	struct gb_cmd_hdr h = { op, total, ++cx->chan.seq, flags };
	memcpy(o->p, &h, sizeof(h));
	return 0;
}

void gb_cl_end(struct gb_cl *cx, struct gb_out *o)
{
	if (gb_out_end(o))
		cx->lost = 1;
}

int gb_cl_wait_reply(struct gb_cl *cx, struct gb_in *in, uint32_t op)
{
	int rc = gb_in_recv(in, &cx->chan.rsp, -1);

	if (rc) {
		cx->lost = 1;
		dbg("reply wait failed (%d): server lost", rc);
		return rc;
	}
	struct gb_cmd_hdr h;
	memcpy(&h, in->p, sizeof(h));
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

/* ---- COMPSIZE helpers ------------------------------------------------------ */

static int cl_geti(void *ctx, uint32_t pname)
{
	GLint v = 0;
	glGetIntegerv(pname, &v);
	return v;
}

int64_t gb_cl_pname_count(uint32_t op, uint32_t pname)
{
	if (op == GB_OP_glGetIntegerv || op == GB_OP_glGetFloatv ||
	    op == GB_OP_glGetBooleanv || op == GB_OP_glGetInteger64v)
		return gb_pname_count_state(pname, cl_geti, NULL);
	return gb_pname_count_param(pname);
}

int64_t gb_cl_pixel_bytes(uint32_t op, uint32_t format, uint32_t type,
			  int64_t w, int64_t h)
{
	static const struct gb_pixstore def = { 4, 0, 0, 0 };
	struct gb_cl *cx = gb_cl_get();
	const struct gb_pixstore *ps = &def;

	if (cx && cx->cur)
		ps = op == GB_OP_glReadPixels ? &cx->cur->pack
					      : &cx->cur->unpack;
	return gb_pixel_bytes(format, type, w, h, ps);
}

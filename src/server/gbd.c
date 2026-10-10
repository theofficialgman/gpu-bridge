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
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gb_server_impl.h"

static int g_debug;
static void *g_egl, *g_gles;

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

	g_egl = dlopen(e ? e : "libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	g_gles = dlopen(g ? g : "libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl) {
		fprintf(stderr, "gbd: cannot load EGL: %s\n", dlerror());
		return -1;
	}
	if (!g_gles)
		fprintf(stderr, "gbd: warning: no GLESv2: %s\n", dlerror());
	return 0;
}

void *gb_native_sym(const char *name)
{
	void *p = NULL;

	if (g_egl)
		p = dlsym(g_egl, name);
	if (!p && g_gles)
		p = dlsym(g_gles, name);
	if (!p && g_egl) {
		void *(*gpa)(const char *) = dlsym(g_egl, "eglGetProcAddress");
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
	if (fn_glGetIntegerv)
		fn_glGetIntegerv(pname, &v);
	return v;
}

int64_t gb_sv_pname_count(uint32_t op, uint32_t pname)
{
	if (op == GB_OP_glGetIntegerv || op == GB_OP_glGetFloatv ||
	    op == GB_OP_glGetBooleanv || op == GB_OP_glGetInteger64v)
		return gb_pname_count_state(pname, gb_sv_geti, NULL);
	return gb_pname_count_param(pname);
}

int64_t gb_sv_pixel_bytes(uint32_t op, uint32_t format, uint32_t type,
			  int64_t w, int64_t h)
{
	int pack = op == GB_OP_glReadPixels;
	struct gb_pixstore ps;

	ps.alignment = gb_sv_geti(NULL, pack ? 0x0D05 : 0x0CF5);
	ps.row_length = gb_sv_geti(NULL, pack ? 0x0D02 : 0x0CF2);
	ps.skip_pixels = gb_sv_geti(NULL, pack ? 0x0D04 : 0x0CF4);
	ps.skip_rows = gb_sv_geti(NULL, pack ? 0x0D03 : 0x0CF3);
	return gb_pixel_bytes(format, type, w, h, &ps);
}

/* ---- worker --------------------------------------------------------------- */

static void *worker_main(void *arg)
{
	struct gb_sth *t = arg;

	for (;;) {
		struct gb_in in;
		int rc = gb_in_recv(&in, &t->chan.req, -1);
		if (rc) {
			gb_log("channel %u ends (%d)", t->id, rc);
			break;
		}
		struct gb_cmd_hdr h;
		if (in.size < sizeof(h)) {
			gb_in_release(&in);
			break;
		}
		memcpy(&h, in.p, sizeof(h)); /* copy: ring memory is untrusted */
		struct gb_r r;
		gb_r_init(&r, in.p, in.size);

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
	}
	gb_srv_scratch_free(t);
	return NULL;
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

	if (gb_native_init())
		return 3;
	return serve(sock);
}

/* SPDX-License-Identifier: MIT */
/*
 * test_chan_xabi.c - end-to-end transport test: a 32-bit client and a 64-bit
 * server speak the control protocol, share a channel memfd, exchange small
 * and chunked (BIG) messages, and the client detects server death.
 *
 *   test_chan_xabi64 <client32-path>      (64-bit build is the server)
 *   test_chan_xabi32 client <sockfd>      (spawned by the server)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gb_chan.h"
#include "gb_codec.h"
#include "gb_ctl.h"

#define OP_ECHO 0x1000u  /* payload: u32 n, then n bytes; reply: sum + bytes */
#define OP_EXIT 0x1001u  /* server exits immediately, no reply */

static uint32_t fill_byte(uint32_t i, uint32_t salt)
{
	return (i * 2654435761u + salt) >> 24;
}

/* ---- server (64-bit) ----------------------------------------------------- */

static int g_ctl_sock = -1;

static void *serve_channel(void *arg)
{
	struct gb_chan *ch = arg;
	for (;;) {
		struct gb_in in;
		if (gb_in_recv(&in, &ch->req, -1))
			return NULL;
		struct gb_cmd_hdr h;
		memcpy(&h, in.p, sizeof(h));
		if (h.opcode == OP_EXIT) {
			/* Simulated crash: peer end of the socketpair vanishes. */
			close(g_ctl_sock);
			return NULL;
		}

		struct gb_r r;
		gb_r_init(&r, in.p, in.size);
		uint32_t n = gb_r_u32(&r);
		const uint8_t *data = gb_r_blob(&r, n);
		if (r.err || !data) {
			fprintf(stderr, "server: bad echo request\n");
			_exit(3);
		}
		uint64_t sum = 0;
		for (uint32_t i = 0; i < n; i++)
			sum += data[i];

		/* hdr(16) + u64(8) + u32(4) -> blob starts at 32 */
		uint32_t total = GB_ALIGN_UP(32 + n, 8u);
		struct gb_out o;
		if (gb_out_begin(&o, &ch->rsp, total)) {
			fprintf(stderr, "server: reply alloc failed\n");
			_exit(4);
		}
		struct gb_cmd_hdr rh = { h.opcode, total, h.seq,
					 GB_CMD_F_REPLY };
		memcpy(o.p, &rh, sizeof(rh));
		struct gb_w w;
		gb_w_init(&w, o.p);
		gb_w_u64(&w, sum);
		gb_w_u32(&w, n);
		memcpy(gb_w_blob(&w, n), data, n);
		gb_out_end(&o);
		gb_in_release(&in);
	}
}

static int server_main(const char *client_path)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0)
		return 2;
	pid_t pid = fork();
	if (pid == 0) {
		close(sv[0]);
		char fdbuf[16];
		snprintf(fdbuf, sizeof(fdbuf), "%d", sv[1]);
		execl(client_path, client_path, "client", fdbuf, (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	g_ctl_sock = sv[0];

	struct gb_ctl_msg m, reply = { .magic = GB_CTL_MAGIC };
	int fd = -1;
	if (gb_ctl_recv(sv[0], &m, NULL) || m.type != GB_CTL_HELLO) {
		fprintf(stderr, "server: no HELLO\n");
		return 2;
	}
	fprintf(stderr, "  server: client ptr bits = %llu\n",
		(unsigned long long)m.a[1]);
	reply.type = GB_CTL_HELLO_REPLY;
	reply.a[0] = 1;
	reply.a[1] = sizeof(void *) * 8;
	gb_ctl_send(sv[0], &reply, -1);

	if (gb_ctl_recv(sv[0], &m, &fd) || m.type != GB_CTL_NEW_CHANNEL ||
	    fd < 0) {
		fprintf(stderr, "server: no NEW_CHANNEL\n");
		return 2;
	}
	static struct gb_chan ch;
	int rc = gb_chan_attach(&ch, fd, (size_t)m.a[0]);
	reply.type = GB_CTL_NEW_CHANNEL_REPLY;
	reply.status = rc;
	gb_ctl_send(sv[0], &reply, -1);
	if (rc) {
		fprintf(stderr, "server: attach failed %d\n", rc);
		return 2;
	}
	pthread_t t;
	pthread_create(&t, NULL, serve_channel, &ch);

	int st = 0;
	waitpid(pid, &st, 0);
	int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	printf("transport xabi: %s (client %s %d)\n", ok ? "ok" : "FAIL",
	       WIFEXITED(st) ? "exit" : "signal",
	       WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
	fflush(stdout);
	return ok ? 0 : 1;
}

/* ---- client (32-bit) ----------------------------------------------------- */

static int echo(struct gb_chan *c, uint32_t n, uint32_t salt)
{
	/* hdr(16) + u32 n(4) -> blob is 8-aligned, so it starts at 24 */
	uint32_t total = GB_ALIGN_UP(24 + n, 8u);
	struct gb_out o;
	int rc = gb_out_begin(&o, &c->req, total);
	if (rc)
		return rc;
	struct gb_cmd_hdr h = { OP_ECHO, total, ++c->seq, 0 };
	memcpy(o.p, &h, sizeof(h));
	struct gb_w w;
	gb_w_init(&w, o.p);
	gb_w_u32(&w, n);
	uint8_t *d = gb_w_blob(&w, n);
	uint64_t want = 0;
	for (uint32_t i = 0; i < n; i++) {
		d[i] = (uint8_t)fill_byte(i, salt);
		want += d[i];
	}
	if ((rc = gb_out_end(&o)))
		return rc;

	struct gb_in in;
	if ((rc = gb_in_recv(&in, &c->rsp, 5ll * 1000000000ll)))
		return rc;
	struct gb_r r;
	gb_r_init(&r, in.p, in.size);
	uint64_t sum = gb_r_u64(&r);
	uint32_t rn = gb_r_u32(&r);
	const uint8_t *rd = gb_r_blob(&r, rn);
	int ok = !r.err && sum == want && rn == n && rd;
	for (uint32_t i = 0; ok && i < n; i++)
		ok = rd[i] == (uint8_t)fill_byte(i, salt);
	gb_in_release(&in);
	return ok ? 0 : -EBADMSG;
}

static int client_main(int sock)
{
	struct gb_ctl_msg m = { .magic = GB_CTL_MAGIC, .type = GB_CTL_HELLO };
	m.a[0] = 1;
	m.a[1] = sizeof(void *) * 8;
	if (gb_ctl_send(sock, &m, -1) || gb_ctl_recv(sock, &m, NULL) ||
	    m.type != GB_CTL_HELLO_REPLY) {
		fprintf(stderr, "client: handshake failed\n");
		return 2;
	}
	fprintf(stderr, "  client: server ptr bits = %llu, my ptr bits = %zu\n",
		(unsigned long long)m.a[1], sizeof(void *) * 8);

	struct gb_chan ch;
	if (gb_chan_create(&ch, GB_CHAN_RING_BYTES, GB_CHAN_RING_BYTES)) {
		fprintf(stderr, "client: chan create failed\n");
		return 2;
	}
	ch.req.watch_fd = ch.rsp.watch_fd = sock;
	struct gb_ctl_msg nc = { .magic = GB_CTL_MAGIC,
				 .type = GB_CTL_NEW_CHANNEL };
	nc.a[0] = ch.region_size;
	if (gb_ctl_send(sock, &nc, ch.fd) || gb_ctl_recv(sock, &m, NULL) ||
	    m.status) {
		fprintf(stderr, "client: NEW_CHANNEL failed\n");
		return 2;
	}

	static const uint32_t sizes[] = {
		0, 1, 7, 100, 4096, 60000, 200000, 262144 /* = ring/4 */,
		262145, 1000000, 5000000, 30000000
	};
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		int rc = echo(&ch, sizes[i], i);
		printf("  echo %9u bytes: %s\n", sizes[i],
		       rc ? strerror(-rc) : "ok");
		if (rc)
			return 1;
	}
	/* Interleave many small ones to exercise wrap + wakeups. */
	for (uint32_t i = 0; i < 20000; i++)
		if (echo(&ch, 8 + (i % 97) * 8, i)) {
			printf("  small echo %u failed\n", i);
			return 1;
		}
	printf("  20000 small echoes ok\n");

	/* Server death: ask it to exit, then a blocking wait must not hang. */
	uint32_t total = 16;
	struct gb_out o;
	gb_out_begin(&o, &ch.req, total);
	struct gb_cmd_hdr h = { OP_EXIT, total, ++ch.seq, 0 };
	memcpy(o.p, &h, sizeof(h));
	gb_out_end(&o);
	struct gb_in in;
	int rc = gb_in_recv(&in, &ch.rsp, -1); /* infinite: relies on watch_fd */
	printf("  wait after server exit: %s\n",
	       rc == -EPIPE ? "EPIPE (ok)" : "UNEXPECTED");
	return rc == -EPIPE ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "client"))
		return client_main(atoi(argv[2]));
	if (argc == 2 && sizeof(void *) == 8)
		return server_main(argv[1]);
	fprintf(stderr, "usage: %s <client32-path>\n", argv[0]);
	return 2;
}

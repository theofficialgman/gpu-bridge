/* SPDX-License-Identifier: MIT */
/*
 * test_mbox.c - the ring mailbox (gb_mbox_*) between two threads that sleep
 * and wake at random: every request must see all records published before it
 * (ordering), every reply must arrive (no lost wake-up: a stall would last
 * the 100 ms peer watchdog, and the test fails on any single wait over 40 ms),
 * and the records pushed as wake-up kicks must be harmless.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gb_chan.h"
#include "gb_opx.h"

#define ITERS 60000

static struct gb_chan cli, srv;
static volatile int server_fail;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static unsigned rnd(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

static void *server(void *arg)
{
	unsigned seed = 7;
	uint32_t records = 0; /* real records consumed so far */

	srv.req.mbox_watch = 1;
	for (long done = 0; done < ITERS;) {
		struct gb_in in;
		int rc;

		if (rnd(&seed) % 16 == 0)
			usleep(rnd(&seed) % 300); /* let the client post to a server about to sleep */
		rc = gb_in_recv(&in, &srv.req, -1);
		if (rc == 1) {
			uint32_t q, op, a, ta;

			if (!gb_mbox_pending(&srv.req, &q, &op, &a, &ta)) {
				continue;
			}
			/* the request names the records published before it */
			if (a != records) {
				fprintf(stderr, "ORDER: client had sent %u, server consumed %u\n", a, records);
				server_fail = 1;
			}
			if (rnd(&seed) % 8 == 0)
				usleep(rnd(&seed) % 200);
			gb_mbox_reply(&srv.rsp, &srv.req, q, a + 1000);
			done++;
			continue;
		}
		if (rc) {
			fprintf(stderr, "recv %d\\n", rc);
			server_fail = 1;
			return NULL;
		}
		struct gb_cmd_hdr h;

		memcpy(&h, in.p, sizeof h);
		if (h.opcode != GB_OPX_Ping) /* kicks are pings; the rest count */
			records++;
		gb_in_release(&in);
	}
	return NULL;
}

int main(void)
{
	pthread_t th;
	unsigned seed = 99;
	uint32_t sent = 0;
	double worst = 0;

	if (gb_chan_create(&cli, 1 << 16, 1 << 16))
		return 1;
	if (gb_chan_attach(&srv, cli.fd, cli.region_size))
		return 1;
	pthread_create(&th, NULL, server, NULL);
	for (long i = 0; i < ITERS; i++) {
		unsigned n = rnd(&seed) % 4; /* some records first */

		for (unsigned k = 0; k < n; k++) {
			struct gb_out o;

			if (gb_out_begin(&o, &cli.req, 32))
				return 2;
			struct gb_cmd_hdr h = { GB_OPX_Ping + 1, 32, 0, 0 };

			memset(o.p, 0, 32);
			memcpy(o.p, &h, sizeof h);
			gb_out_end(&o);
			sent++;
		}
		if (rnd(&seed) % 16 == 0)
			usleep(rnd(&seed) % 300); /* so the server really sleeps */
		double t0 = now();
		uint32_t seq = gb_mbox_post(&cli.req, GB_MBOX_GETERROR, sent), val;
		int rc;

		while ((rc = gb_mbox_wait(&cli.rsp, seq, &val)) == 1) {
			struct gb_in in;

			while (!gb_in_recv(&in, &cli.rsp, 0)) /* kicks from the server */
				gb_in_release(&in);
		}
		double dt = now() - t0;

		if (dt > worst)
			worst = dt;
		if (rc || val != sent + 1000) {
			fprintf(stderr, "reply rc=%d val=%u want %u\n", rc, val, sent + 1000);
			return 3;
		}
	}
	pthread_join(th, NULL);
	printf("mbox: %d round trips, worst %.2f ms%s\n", ITERS, worst * 1e3,
	       server_fail ? " (SERVER FAILED)" : "");
	if (server_fail || worst > 0.040) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}

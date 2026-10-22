/* SPDX-License-Identifier: MIT */
/*
 * ring_pingpong.c - round trip through two gb_rings (request + reply, 32-byte
 * records, the spin/futex waits of the real channel) between two processes,
 * with nothing else: the floor for a synchronous bridge call.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gb_ring.h"

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 500000;
	size_t span = 4096 * 2 + 65536;
	uint8_t *mem = mmap(NULL, 2 * span, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	struct gb_ring req, rsp, sreq, srsp;

	gb_ring_init(mem, 65536);
	gb_ring_init(mem + span, 65536);
	gb_ring_attach(&req, mem, span);
	gb_ring_attach(&rsp, mem + span, span);
	gb_ring_attach(&sreq, mem, span);
	gb_ring_attach(&srsp, mem + span, span);
	pid_t pid = fork();

	if (!pid) { /* server */
		for (long i = 0; i < iters; i++) {
			gb_ring_wait_data(&sreq, -1);
			uint32_t sz;
			const struct gb_rec_hdr *h = gb_ring_peek(&sreq, &sz);
			uint32_t op = h->opcode;

			gb_ring_pop(&sreq, sz);
			struct gb_rec_hdr *o = gb_ring_reserve(&srsp, 32, -1);

			o->opcode = op;
			o->size = 32;
			gb_ring_commit(&srsp);
		}
		_exit(0);
	}
	double t0 = now();

	for (long i = 0; i < iters; i++) {
		struct gb_rec_hdr *o = gb_ring_reserve(&req, 32, -1);

		o->opcode = 7;
		o->size = 32;
		gb_ring_commit(&req);
		gb_ring_wait_data(&rsp, -1);
		uint32_t sz;

		(void)gb_ring_peek(&rsp, &sz);
		gb_ring_pop(&rsp, sz);
	}
	double dt = now() - t0;

	waitpid(pid, NULL, 0);
	printf("ring round trip %.0f ns\n", dt / iters * 1e9);
	return 0;
}

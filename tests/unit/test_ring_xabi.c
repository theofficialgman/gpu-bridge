/* SPDX-License-Identifier: MIT */
/*
 * test_ring_xabi.c - cross-ABI ring test: a 32-bit and a 64-bit process share
 * one memfd ring. Built twice (-m32, -m64); the 64-bit build is the driver.
 *
 *   test_ring_xabi64 <peer32-path> <32prod|32cons>
 *
 * The driver creates the memfd, execs the 32-bit build as the peer, and plays
 * the opposite role itself. Exit status 0 means every record arrived intact.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gb_ring.h"

#define RING_BYTES (1u << 15)
#define COUNT 300000u

static uint32_t rng(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static uint32_t msg_size(uint32_t *seed)
{
	return 16 + (rng(seed) % 100) * 8;
}

static void fill_msg(void *buf, uint32_t size, uint32_t seq)
{
	struct gb_cmd_hdr *h = buf;
	h->opcode = 0x200 + (seq & 0xff);
	h->size = size;
	h->seq = seq;
	h->flags = 0;
	uint32_t *w = (uint32_t *)(h + 1);
	for (uint32_t i = 0; i < (size - sizeof(*h)) / 4; i++)
		w[i] = seq * 2246822519u + i;
}

static int check_msg(const void *buf, uint32_t size, uint32_t seq)
{
	const struct gb_cmd_hdr *h = buf;
	if (h->size != size || h->seq != seq || h->opcode != 0x200 + (seq & 0xff))
		return 0;
	const uint32_t *w = (const uint32_t *)(h + 1);
	for (uint32_t i = 0; i < (size - sizeof(*h)) / 4; i++)
		if (w[i] != seq * 2246822519u + i)
			return 0;
	return 1;
}

static int run_producer(struct gb_ring *r)
{
	uint32_t seed = 777;
	for (uint32_t i = 0; i < COUNT; i++) {
		uint32_t sz = msg_size(&seed);
		void *m = gb_ring_reserve(r, sz, -1);
		if (!m)
			return 1;
		fill_msg(m, sz, i);
		uint32_t roll = rng(&seed);
		if ((i & 7) == 7 || (roll & 15) == 0)
			gb_ring_commit(r);
	}
	gb_ring_commit(r);
	gb_ring_close(r);
	return 0;
}

static int run_consumer(struct gb_ring *r)
{
	uint32_t seed = 777, i = 0;
	for (;;) {
		if (gb_ring_wait_data(r, 10ll * 1000000000ll) == -EPIPE)
			break;
		uint32_t sz;
		const void *m;
		while ((m = gb_ring_peek(r, &sz))) {
			uint32_t want = msg_size(&seed);
			(void)rng(&seed);
			if (sz != want || !check_msg(m, sz, i)) {
				fprintf(stderr, "consumer: bad record %u\n", i);
				return 1;
			}
			i++;
			gb_ring_pop(r, sz);
		}
		if (r->broken)
			return 1;
	}
	if (i != COUNT) {
		fprintf(stderr, "consumer: got %u of %u\n", i, COUNT);
		return 1;
	}
	return 0;
}

static int attach_fd(struct gb_ring *r, int fd, int create)
{
	size_t sz = gb_ring_region_size(RING_BYTES);
	if (create && ftruncate(fd, (off_t)sz) < 0)
		return -1;
	void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return -1;
	if (create && gb_ring_init(p, RING_BYTES))
		return -1;
	return gb_ring_attach(r, p, sz);
}

int main(int argc, char **argv)
{
	struct gb_ring r;

	/* Peer mode: test_ring_xabi32 child <fd> <prod|cons> */
	if (argc == 4 && !strcmp(argv[1], "child")) {
		if (attach_fd(&r, atoi(argv[2]), 0)) {
			fprintf(stderr, "child: attach failed\n");
			return 2;
		}
		fprintf(stderr, "  peer: sizeof(void*)=%zu, role=%s\n",
			sizeof(void *), argv[3]);
		return !strcmp(argv[3], "prod") ? run_producer(&r)
						: run_consumer(&r);
	}

	if (argc != 3 || sizeof(void *) != 8) {
		fprintf(stderr, "usage: %s <peer32> <32prod|32cons>  (64-bit)\n",
			argv[0]);
		return 2;
	}
	int peer_produces = !strcmp(argv[2], "32prod");

	int fd = memfd_create("gb-xabi-ring", 0); /* inheritable on exec */
	if (fd < 0 || attach_fd(&r, fd, 1)) {
		perror("setup");
		return 2;
	}

	char fdbuf[16];
	snprintf(fdbuf, sizeof(fdbuf), "%d", fd);
	pid_t pid = fork();
	if (pid == 0) {
		execl(argv[1], argv[1], "child", fdbuf,
		      peer_produces ? "prod" : "cons", (char *)NULL);
		perror("exec peer");
		_exit(127);
	}
	fprintf(stderr, "  driver: sizeof(void*)=%zu, role=%s\n",
		sizeof(void *), peer_produces ? "cons" : "prod");
	int rc = peer_produces ? run_consumer(&r) : run_producer(&r);
	int st = 0;
	waitpid(pid, &st, 0);
	int peer_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	printf("%s: %u records, driver %s, peer %s\n", argv[2], COUNT,
	       rc ? "FAIL" : "ok", peer_ok ? "ok" : "FAIL");
	return rc || !peer_ok;
}

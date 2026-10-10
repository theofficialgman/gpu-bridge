/*
 * test_ring.c - unit/stress tests for gb_ring. Built for both -m64 and -m32.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gb_ring.h"

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,      \
				__LINE__, #cond);                            \
			failures++;                                          \
			return;                                              \
		}                                                            \
	} while (0)

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static uint32_t rng(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

struct pair {
	struct gb_ring prod, cons;
	void *region;
	size_t region_size;
};

static int pair_open(struct pair *p, uint32_t data_size, int use_memfd)
{
	p->region_size = gb_ring_region_size(data_size);
	if (use_memfd) {
		int fd = memfd_create("gb-test-ring", 0);
		if (fd < 0 || ftruncate(fd, (off_t)p->region_size) < 0)
			return -1;
		p->region = mmap(NULL, p->region_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, 0);
		close(fd);
	} else {
		p->region = mmap(NULL, p->region_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	}
	if (p->region == MAP_FAILED)
		return -1;
	if (gb_ring_init(p->region, data_size))
		return -1;
	if (gb_ring_attach(&p->prod, p->region, p->region_size))
		return -1;
	if (gb_ring_attach(&p->cons, p->region, p->region_size))
		return -1;
	return 0;
}

static void pair_close(struct pair *p)
{
	munmap(p->region, p->region_size);
}

/* Message: gb_cmd_hdr followed by payload words derived from `seq`. */
static void fill_msg(void *buf, uint32_t size, uint32_t seq)
{
	struct gb_cmd_hdr *h = buf;
	h->opcode = 0x100 + (seq & 0xff);
	h->size = size;
	h->seq = seq;
	h->flags = 0;
	uint32_t *w = (uint32_t *)(h + 1);
	uint32_t n = (size - sizeof(*h)) / 4;
	for (uint32_t i = 0; i < n; i++)
		w[i] = seq * 2654435761u + i;
}

static int check_msg(const void *buf, uint32_t size, uint32_t want_seq)
{
	const struct gb_cmd_hdr *h = buf;
	if (h->size != size || h->seq != want_seq ||
	    h->opcode != 0x100 + (want_seq & 0xff))
		return 0;
	const uint32_t *w = (const uint32_t *)(h + 1);
	uint32_t n = (size - sizeof(*h)) / 4;
	for (uint32_t i = 0; i < n; i++)
		if (w[i] != want_seq * 2654435761u + i)
			return 0;
	return 1;
}

static void test_layout(void)
{
	printf("layout: sizeof(void*)=%zu ctrl=%zu tail@%zu head@%zu\n",
	       sizeof(void *), sizeof(struct gb_ring_ctrl),
	       offsetof(struct gb_ring_ctrl, tail),
	       offsetof(struct gb_ring_ctrl, head));
	CHECK(offsetof(struct gb_ring_ctrl, tail) == 64);
	CHECK(offsetof(struct gb_ring_ctrl, head) == 128);
}

static void test_basic(void)
{
	struct pair p;
	CHECK(pair_open(&p, 4096, 0) == 0);

	uint32_t sz;
	CHECK(gb_ring_peek(&p.cons, &sz) == NULL && errno == 0);

	void *m = gb_ring_reserve(&p.prod, 64, 0);
	CHECK(m != NULL);
	fill_msg(m, 64, 7);
	/* Unpublished: consumer must not see it. */
	CHECK(gb_ring_peek(&p.cons, &sz) == NULL);
	gb_ring_commit(&p.prod);

	const void *r = gb_ring_peek(&p.cons, &sz);
	CHECK(r != NULL && sz == 64 && check_msg(r, sz, 7));
	gb_ring_pop(&p.cons, sz);
	CHECK(gb_ring_peek(&p.cons, &sz) == NULL);

	CHECK(gb_ring_reserve(&p.prod, 12, 0) == NULL && errno == EINVAL);
	CHECK(gb_ring_reserve(&p.prod, 4096, 0) == NULL && errno == EMSGSIZE);
	pair_close(&p);
}

static void test_wrap_and_pad(void)
{
	struct pair p;
	uint32_t s = 12345, next_w = 0, next_r = 0;
	CHECK(pair_open(&p, 4096, 0) == 0);

	for (int iter = 0; iter < 200000; iter++) {
		uint32_t sz = 16 + (rng(&s) % 120) * 8; /* 16..960 */
		void *m = gb_ring_reserve(&p.prod, sz, 0);
		if (m) {
			fill_msg(m, sz, next_w++);
			gb_ring_commit(&p.prod);
		}
		/* Drain 0..2 messages, so the ring regularly runs near full. */
		for (uint32_t d = rng(&s) % 3; d; d--) {
			uint32_t rsz;
			const void *r = gb_ring_peek(&p.cons, &rsz);
			if (!r)
				break;
			CHECK(check_msg(r, rsz, next_r));
			next_r++;
			gb_ring_pop(&p.cons, rsz);
		}
	}
	uint32_t rsz;
	const void *r;
	while ((r = gb_ring_peek(&p.cons, &rsz))) {
		CHECK(check_msg(r, rsz, next_r));
		next_r++;
		gb_ring_pop(&p.cons, rsz);
	}
	CHECK(next_r == next_w && next_w > 1000);
	printf("  wrap/pad: %u messages\n", next_w);
	pair_close(&p);
}

static void test_full_nonblocking(void)
{
	struct pair p;
	CHECK(pair_open(&p, 4096, 0) == 0);
	int n = 0;
	void *m;
	while ((m = gb_ring_reserve(&p.prod, 512, 0))) {
		fill_msg(m, 512, n++);
		gb_ring_commit(&p.prod);
	}
	CHECK(errno == EAGAIN && n == 8);

	uint32_t sz;
	const void *r = gb_ring_peek(&p.cons, &sz);
	CHECK(r && check_msg(r, sz, 0));
	gb_ring_pop(&p.cons, sz);
	CHECK(gb_ring_reserve(&p.prod, 512, 0) != NULL);
	pair_close(&p);
}

struct stress {
	struct gb_ring *ring;
	uint32_t count;
	uint32_t seed;
	int ok;
};

static void *producer_thread(void *arg)
{
	struct stress *s = arg;
	uint32_t seed = s->seed;
	for (uint32_t i = 0; i < s->count; i++) {
		uint32_t sz = 16 + (rng(&seed) % 100) * 8;
		void *m = gb_ring_reserve(s->ring, sz, -1);
		if (!m) {
			s->ok = 0;
			return NULL;
		}
		fill_msg(m, sz, i);
		uint32_t roll = rng(&seed); /* always rolled: consumer mirrors */
		if ((i & 7) == 7 || (roll & 15) == 0)
			gb_ring_commit(s->ring);
	}
	gb_ring_commit(s->ring);
	gb_ring_close(s->ring);
	s->ok = 1;
	return NULL;
}

static void *consumer_thread(void *arg)
{
	struct stress *s = arg;
	uint32_t seed = s->seed, i = 0;
	s->ok = 1;
	for (;;) {
		int rc = gb_ring_wait_data(s->ring, -1);
		if (rc == -EPIPE)
			break;
		uint32_t sz;
		const void *r;
		while ((r = gb_ring_peek(s->ring, &sz))) {
			uint32_t want = 16 + (rng(&seed) % 100) * 8;
			(void)rng(&seed); /* mirror producer's commit-roll */
			if (sz != want || !check_msg(r, sz, i)) {
				s->ok = 0;
				return NULL;
			}
			i++;
			gb_ring_pop(s->ring, sz);
		}
	}
	s->ok = s->ok && i == s->count;
	return NULL;
}

static void test_threads(void)
{
	struct pair p;
	CHECK(pair_open(&p, 1 << 16, 0) == 0);
	/* Producer and consumer roll the same rng sequence (size, commit). */
	struct stress ps = { &p.prod, 400000, 99, 0 };
	struct stress cs = { &p.cons, 400000, 99, 0 };
	pthread_t pt, ct;
	int64_t t0 = now_ns();
	pthread_create(&ct, NULL, consumer_thread, &cs);
	pthread_create(&pt, NULL, producer_thread, &ps);
	pthread_join(pt, NULL);
	pthread_join(ct, NULL);
	printf("  threads: %.1f ms\n", (now_ns() - t0) / 1e6);
	CHECK(ps.ok);
	CHECK(cs.ok);
	pair_close(&p);
}

static void *delayed_commit(void *arg)
{
	struct gb_ring *prod = arg;
	usleep(50 * 1000);
	void *m = gb_ring_reserve(prod, 32, 0);
	if (m) {
		fill_msg(m, 32, 1);
		gb_ring_commit(prod);
	}
	return NULL;
}

static void test_blocking_and_timeout(void)
{
	struct pair p;
	CHECK(pair_open(&p, 4096, 0) == 0);

	int64_t t0 = now_ns();
	CHECK(gb_ring_wait_data(&p.cons, 30 * 1000000ll) == -ETIMEDOUT);
	int64_t dt = now_ns() - t0;
	CHECK(dt >= 25 * 1000000ll && dt < 500 * 1000000ll);
	CHECK(gb_ring_wait_data(&p.cons, 0) == -ETIMEDOUT);

	pthread_t t;
	pthread_create(&t, NULL, delayed_commit, &p.prod);
	t0 = now_ns();
	CHECK(gb_ring_wait_data(&p.cons, -1) == 0); /* futex sleep, then wake */
	dt = now_ns() - t0;
	CHECK(dt >= 30 * 1000000ll && dt < 1000 * 1000000ll);
	pthread_join(t, NULL);

	uint32_t sz;
	const void *r = gb_ring_peek(&p.cons, &sz);
	CHECK(r && check_msg(r, sz, 1));
	pair_close(&p);
}

static void test_close_drains(void)
{
	struct pair p;
	CHECK(pair_open(&p, 4096, 0) == 0);
	void *m = gb_ring_reserve(&p.prod, 32, 0);
	fill_msg(m, 32, 5);
	gb_ring_commit(&p.prod);
	gb_ring_close(&p.prod);

	CHECK(gb_ring_wait_data(&p.cons, 0) == 0); /* still has data */
	uint32_t sz;
	const void *r = gb_ring_peek(&p.cons, &sz);
	CHECK(r && check_msg(r, sz, 5));
	gb_ring_pop(&p.cons, sz);
	CHECK(gb_ring_wait_data(&p.cons, 0) == -EPIPE);
	CHECK(gb_ring_reserve(&p.prod, 32, 0) == NULL && errno == EPIPE);
	pair_close(&p);
}

static void test_malformed(void)
{
	/* The consumer must survive any bytes a hostile producer writes. */
	static const uint32_t bad_sizes[] = { 0, 4, 12, 7, 0x7ffffff8u,
					      0xfffffff8u, 8192 };
	for (unsigned i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++) {
		struct pair p;
		CHECK(pair_open(&p, 4096, 0) == 0);
		uint32_t *raw = (uint32_t *)((uint8_t *)p.region +
					     GB_RING_DATA_OFFSET);
		raw[0] = 0x100;
		raw[1] = bad_sizes[i];
		memset(raw + 2, 0, 64);
		atomic_store(&p.prod.ctrl->tail, 64);

		uint32_t sz;
		CHECK(gb_ring_peek(&p.cons, &sz) == NULL);
		CHECK(errno == EPROTO && p.cons.broken);
		pair_close(&p);
	}

	/* A hostile control block must be rejected at attach time. */
	struct pair p;
	CHECK(pair_open(&p, 4096, 0) == 0);
	struct gb_ring r;
	((struct gb_ring_ctrl *)p.region)->size = 5000; /* not a power of 2 */
	CHECK(gb_ring_attach(&r, p.region, p.region_size) == -EPROTO);
	((struct gb_ring_ctrl *)p.region)->size = 1u << 20; /* > mapping */
	CHECK(gb_ring_attach(&r, p.region, p.region_size) == -EPROTO);
	pair_close(&p);
}

static void test_cross_process(void)
{
	struct pair p;
	CHECK(pair_open(&p, 1 << 14, 1) == 0);
	const uint32_t count = 200000;

	pid_t pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		struct stress cs = { &p.cons, count, 4242, 0 };
		consumer_thread(&cs);
		_exit(cs.ok ? 0 : 1);
	}
	struct stress ps = { &p.prod, count, 4242, 0 };
	producer_thread(&ps);
	int st = 0;
	waitpid(pid, &st, 0);
	CHECK(ps.ok);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	pair_close(&p);
}

int main(void)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{ "layout", test_layout },
		{ "basic", test_basic },
		{ "wrap_and_pad", test_wrap_and_pad },
		{ "full_nonblocking", test_full_nonblocking },
		{ "threads", test_threads },
		{ "blocking_and_timeout", test_blocking_and_timeout },
		{ "close_drains", test_close_drains },
		{ "malformed", test_malformed },
		{ "cross_process", test_cross_process },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int before = failures;
		printf("[ RUN  ] %s\n", tests[i].name);
		fflush(stdout);
		tests[i].fn();
		printf("[ %s ] %s\n", failures == before ? " OK " : "FAIL",
		       tests[i].name);
	}
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

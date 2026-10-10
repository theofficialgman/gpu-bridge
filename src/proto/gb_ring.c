/*
 * gb_ring.c - SPSC shared-memory byte ring with futex wakeups.
 * See gb_ring.h for the contract. Compiles unchanged for -m32 and -m64.
 */
#define _GNU_SOURCE
#include "gb_ring.h"

#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <sched.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * Wait strategy: a few `pause` spins (catches a peer that answers within
 * ~1us on another core), then sched_yield() spins (when client and server
 * share a core a yield hands the CPU straight to the peer instead of burning
 * the whole time slice), then a futex sleep. Measured: pure pause-spinning
 * made a same-core sync call ~800x slower than a cross-core one.
 */
#define SPIN_PAUSE_ITERS 64
#define SPIN_YIELD_ITERS 64
#define SPIN_ITERATIONS (SPIN_PAUSE_ITERS + SPIN_YIELD_ITERS)
#define MAX_FUTEX_SECS (1L << 28)

/*
 * SYS_futex takes the kernel's *old* timespec: two `long`s. On 32-bit ABIs
 * with 64-bit time_t (_TIME_BITS=64) libc's struct timespec is NOT that, so
 * we use our own struct to be correct for arm32 and i386 alike.
 */
struct gb_ktimespec {
	long tv_sec;
	long tv_nsec;
};

static inline void spin_step(int n)
{
	if (n >= SPIN_PAUSE_ITERS) {
		sched_yield();
		return;
	}
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ __volatile__("yield" ::: "memory");
#endif
}

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* Returns 0 if woken or the value already changed, -ETIMEDOUT on timeout. */
static int futex_wait(_Atomic uint32_t *addr, uint32_t expect, int64_t rel_ns)
{
	struct gb_ktimespec ts;
	const struct gb_ktimespec *tp = NULL;

	if (rel_ns >= 0) {
		int64_t secs = rel_ns / 1000000000ll;
		if (secs > MAX_FUTEX_SECS)
			secs = MAX_FUTEX_SECS;
		ts.tv_sec = (long)secs;
		ts.tv_nsec = (long)(rel_ns % 1000000000ll);
		tp = &ts;
	}
	/* Non-private: the word lives in memory shared between processes. */
	long rc = syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAIT, expect, tp,
			  NULL, 0);
	if (rc < 0 && errno == ETIMEDOUT)
		return -ETIMEDOUT;
	return 0;
}

static void futex_wake(_Atomic uint32_t *addr)
{
	syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/* True (and the ring is closed) if the watched peer hung up. */
static int peer_gone(struct gb_ring *r)
{
	struct pollfd pfd = { .fd = r->watch_fd, .events = 0 };

	if (r->watch_fd < 0)
		return 0;
	if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
		atomic_store_explicit(&r->ctrl->closed, 1,
				      memory_order_release);
		return 1;
	}
	return 0;
}

/* Sleep slice: infinite waits are chopped so peer_gone() gets polled. */
static int64_t slice(struct gb_ring *r, int64_t remaining)
{
	if (r->watch_fd >= 0 &&
	    (remaining < 0 || remaining > GB_RING_WATCH_NS))
		return GB_RING_WATCH_NS;
	return remaining;
}

size_t gb_ring_region_size(uint32_t data_size)
{
	return (size_t)GB_RING_DATA_OFFSET + data_size;
}

static int size_valid(uint32_t s)
{
	return s >= GB_RING_MIN_SIZE && s <= GB_RING_MAX_SIZE &&
	       (s & (s - 1)) == 0;
}

int gb_ring_init(void *region, uint32_t data_size)
{
	struct gb_ring_ctrl *c = region;

	if (!size_valid(data_size))
		return -EINVAL;
	memset(c, 0, sizeof(*c));
	c->size = data_size;
	atomic_thread_fence(memory_order_release);
	c->magic = GB_RING_MAGIC;
	return 0;
}

int gb_ring_attach(struct gb_ring *r, void *region, size_t region_size)
{
	struct gb_ring_ctrl *c = region;

	memset(r, 0, sizeof(*r));
	r->watch_fd = -1;
	if (region_size < GB_RING_DATA_OFFSET)
		return -EINVAL;
	if (c->magic != GB_RING_MAGIC)
		return -EPROTO;
	/* Copy once: the peer controls this memory. */
	uint32_t size = c->size;
	if (!size_valid(size) ||
	    region_size < (size_t)GB_RING_DATA_OFFSET + size)
		return -EPROTO;

	r->ctrl = c;
	r->data = (uint8_t *)region + GB_RING_DATA_OFFSET;
	r->size = size;
	r->mask = size - 1;
	r->wtail = atomic_load_explicit(&c->tail, memory_order_acquire);
	r->rhead = atomic_load_explicit(&c->head, memory_order_acquire);
	r->known_head = r->rhead;
	return 0;
}

/* ---- producer ---------------------------------------------------------- */

uint32_t gb_ring_free(struct gb_ring *r)
{
	return r->size - (r->wtail - r->known_head);
}

void gb_ring_commit(struct gb_ring *r)
{
	struct gb_ring_ctrl *c = r->ctrl;

	if (atomic_load_explicit(&c->tail, memory_order_relaxed) == r->wtail)
		return;
	atomic_store_explicit(&c->tail, r->wtail, memory_order_release);
	atomic_thread_fence(memory_order_seq_cst);
	if (atomic_load_explicit(&c->consumer_sleeping, memory_order_relaxed))
		futex_wake(&c->tail);
}

void *gb_ring_reserve(struct gb_ring *r, uint32_t bytes, int64_t timeout_ns)
{
	struct gb_ring_ctrl *c = r->ctrl;
	int64_t deadline = 0;
	int spin = 0;

	if (bytes == 0 || (bytes & (GB_REC_ALIGN - 1))) {
		errno = EINVAL;
		return NULL;
	}
	if (bytes > r->size / 2) {
		errno = EMSGSIZE;
		return NULL;
	}

	for (;;) {
		if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
			errno = EPIPE;
			return NULL;
		}

		uint32_t idx = r->wtail & r->mask;
		uint32_t contig = r->size - idx;
		uint32_t pad = contig < bytes ? contig : 0;
		uint32_t need = bytes + pad;

		if (r->size - (r->wtail - r->known_head) < need)
			r->known_head = atomic_load_explicit(
				&c->head, memory_order_acquire);

		if (r->size - (r->wtail - r->known_head) >= need) {
			if (pad) {
				struct gb_rec_hdr *p =
					(struct gb_rec_hdr *)(r->data + idx);
				p->opcode = GB_OP_PAD;
				p->size = pad;
				r->wtail += pad;
			}
			void *out = r->data + (r->wtail & r->mask);
			r->wtail += bytes;
			return out;
		}

		/* Full. */
		if (timeout_ns == 0) {
			errno = EAGAIN;
			return NULL;
		}
		/* The consumer can only free space we have published. */
		gb_ring_commit(r);

		if (spin < SPIN_ITERATIONS) {
			spin_step(spin++);
			r->known_head = atomic_load_explicit(
				&c->head, memory_order_acquire);
			continue;
		}

		int64_t remaining = -1;
		if (timeout_ns > 0) {
			if (!deadline)
				deadline = now_ns() + timeout_ns;
			remaining = deadline - now_ns();
			if (remaining <= 0) {
				errno = ETIMEDOUT;
				return NULL;
			}
		}

		atomic_store_explicit(&c->producer_sleeping, 1,
				      memory_order_relaxed);
		atomic_thread_fence(memory_order_seq_cst);
		uint32_t h = atomic_load_explicit(&c->head,
						  memory_order_acquire);
		if (h == r->known_head &&
		    !atomic_load_explicit(&c->closed, memory_order_acquire) &&
		    futex_wait(&c->head, h, slice(r, remaining)) ==
			    -ETIMEDOUT)
			peer_gone(r);
		atomic_store_explicit(&c->producer_sleeping, 0,
				      memory_order_relaxed);
		r->known_head = atomic_load_explicit(&c->head,
						     memory_order_acquire);
	}
}

/* ---- consumer ---------------------------------------------------------- */

int gb_ring_wait_data(struct gb_ring *r, int64_t timeout_ns)
{
	struct gb_ring_ctrl *c = r->ctrl;
	int64_t deadline = 0;
	int spin = 0;

	for (;;) {
		uint32_t t = atomic_load_explicit(&c->tail,
						  memory_order_acquire);
		if (t != r->rhead)
			return 0;
		if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
			/* Drain anything published before the close. */
			t = atomic_load_explicit(&c->tail,
						 memory_order_acquire);
			return t != r->rhead ? 0 : -EPIPE;
		}
		if (timeout_ns == 0)
			return -ETIMEDOUT;
		if (spin < SPIN_ITERATIONS) {
			spin_step(spin++);
			continue;
		}

		int64_t remaining = -1;
		if (timeout_ns > 0) {
			if (!deadline)
				deadline = now_ns() + timeout_ns;
			remaining = deadline - now_ns();
			if (remaining <= 0)
				return -ETIMEDOUT;
		}

		atomic_store_explicit(&c->consumer_sleeping, 1,
				      memory_order_relaxed);
		atomic_thread_fence(memory_order_seq_cst);
		t = atomic_load_explicit(&c->tail, memory_order_acquire);
		if (t == r->rhead &&
		    !atomic_load_explicit(&c->closed, memory_order_acquire) &&
		    futex_wait(&c->tail, t, slice(r, remaining)) ==
			    -ETIMEDOUT)
			peer_gone(r);
		atomic_store_explicit(&c->consumer_sleeping, 0,
				      memory_order_relaxed);
	}
}

static void publish_head(struct gb_ring *r)
{
	struct gb_ring_ctrl *c = r->ctrl;

	atomic_store_explicit(&c->head, r->rhead, memory_order_release);
	atomic_thread_fence(memory_order_seq_cst);
	if (atomic_load_explicit(&c->producer_sleeping, memory_order_relaxed))
		futex_wake(&c->head);
}

const void *gb_ring_peek(struct gb_ring *r, uint32_t *size_out)
{
	struct gb_ring_ctrl *c = r->ctrl;

	errno = 0;
	for (;;) {
		uint32_t t = atomic_load_explicit(&c->tail,
						  memory_order_acquire);
		uint32_t avail = t - r->rhead;
		if (avail == 0)
			return NULL;

		uint32_t idx = r->rhead & r->mask;
		/* Copy the header out of shared memory before validating. */
		struct gb_rec_hdr h;
		memcpy(&h, r->data + idx, sizeof(h));

		if (avail < sizeof(h) || h.size < sizeof(h) ||
		    (h.size & (GB_REC_ALIGN - 1)) || h.size > avail ||
		    h.size > r->size - idx) {
			r->broken = 1;
			errno = EPROTO;
			return NULL;
		}
		if (h.opcode == GB_OP_PAD) {
			r->rhead += h.size;
			publish_head(r);
			continue;
		}
		*size_out = h.size;
		return r->data + idx;
	}
}

void gb_ring_pop(struct gb_ring *r, uint32_t size)
{
	r->rhead += size;
	publish_head(r);
}

void gb_ring_close(struct gb_ring *r)
{
	struct gb_ring_ctrl *c = r->ctrl;

	atomic_store_explicit(&c->closed, 1, memory_order_release);
	atomic_thread_fence(memory_order_seq_cst);
	futex_wake(&c->tail);
	futex_wake(&c->head);
}

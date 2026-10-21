/*
 * gb_ring.h - single-producer / single-consumer byte ring for shared memory.
 *
 * The region (typically a memfd mapped by both processes) holds a control block
 * followed by the data area. All control fields are 32-bit so the layout and
 * lock-freedom are identical on 32- and 64-bit ABIs.
 *
 * Records never straddle the end of the data area: if one would, the producer
 * writes a GB_OP_PAD record covering the tail and starts at offset 0.
 *
 * Producer API:  reserve() ... reserve() ... commit()   (commit publishes all)
 * Consumer API:  wait_data() / peek() ... pop()
 *
 * Counters are free-running uint32_t; index = counter & mask. The data size is
 * a power of two <= 2^30, so (tail - head) is always the fill level.
 */
#ifndef GB_RING_H
#define GB_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "gb_proto.h"

#define GB_RING_MAGIC 0x31524247u /* "GBR1" little-endian */
#define GB_RING_DATA_OFFSET 256u
#define GB_RING_MIN_SIZE 4096u
#define GB_RING_MAX_SIZE (1u << 30)

/*
 * A one-cache-line mailbox for tiny synchronous calls (glGetError): the
 * request cell lives in the request ring's control block (client writes,
 * server polls), the reply cell in the response ring's (server writes, client
 * polls). One line crosses in each direction instead of a record and a
 * counter. The request is ordered behind everything already in the ring:
 * tail_at is the published tail at the time of posting, and the server serves
 * the mailbox once it has consumed up to there.
 */
struct gb_mbox {
	_Atomic uint32_t seq; /* written last by the poster */
	uint32_t op, arg, tail_at;
	uint32_t pad[12];
};

struct gb_ring_ctrl {
	uint32_t magic;
	uint32_t size; /* data bytes, power of two */
	uint32_t reserved0[14];

	_Alignas(64) _Atomic uint32_t tail; /* producer-written */
	_Atomic uint32_t consumer_sleeping;
	_Atomic uint32_t closed;
	uint32_t reserved1[13];

	_Alignas(64) _Atomic uint32_t head; /* consumer-written */
	_Atomic uint32_t producer_sleeping;
	/* bytes a sleeping producer needs: the consumer wakes it only once at
	 * least max(need, size/4) are free, not for every record it frees */
	_Atomic uint32_t producer_need;
	uint32_t reserved2[13];

	_Alignas(64) struct gb_mbox mbox;
};

_Static_assert(offsetof(struct gb_ring_ctrl, tail) == 64, "ctrl.tail offset");
_Static_assert(offsetof(struct gb_ring_ctrl, head) == 128, "ctrl.head offset");
_Static_assert(sizeof(struct gb_ring_ctrl) <= GB_RING_DATA_OFFSET, "ctrl size");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "32-bit atomics must be lock-free");

/* Per-process handle. Each side attaches its own; fields are process-local. */
struct gb_ring {
	struct gb_ring_ctrl *ctrl;
	uint8_t *data;
	uint32_t size;
	uint32_t mask;
	uint32_t wtail;      /* producer: next write counter (maybe unpublished) */
	uint32_t known_head; /* producer: last observed consumer head */
	uint32_t rhead;      /* consumer: next read counter */
	int broken;          /* consumer saw a malformed record */
	int watch_fd;        /* if >= 0: poll for POLLHUP while blocking */
	uint32_t mbox_seq;   /* client: last mailbox request posted */
	uint32_t mbox_done;  /* server: last mailbox request served */
	int mbox_watch;      /* server: wait_data/recv also return for a mailbox request */
};

/* Bytes needed for a region holding `data_size` bytes of ring data. */
size_t gb_ring_region_size(uint32_t data_size);

/* Initialise a zeroed region (creator side only). Returns 0 or -errno. */
int gb_ring_init(void *region, uint32_t data_size);

/* Attach to an initialised region of `region_size` bytes. Returns 0 or
 * -errno. The control block is untrusted input: size/magic are validated. */
int gb_ring_attach(struct gb_ring *r, void *region, size_t region_size);

/* ---- producer ---------------------------------------------------------- */

/* Reserve `bytes` (multiple of GB_REC_ALIGN, <= size/2) contiguous bytes.
 * timeout_ns: 0 = non-blocking, <0 = forever, >0 = relative timeout.
 * Returns pointer into the ring or NULL with errno = EAGAIN (non-blocking,
 * full), ETIMEDOUT, EPIPE (closed), EMSGSIZE, EINVAL.
 * Nothing is visible to the consumer until gb_ring_commit(). */
void *gb_ring_reserve(struct gb_ring *r, uint32_t bytes, int64_t timeout_ns);

/* Publish everything reserved so far and wake a sleeping consumer. */
void gb_ring_commit(struct gb_ring *r);

/* Producer-side free space (bytes) as last observed. */
uint32_t gb_ring_free(struct gb_ring *r);

/* ---- consumer ---------------------------------------------------------- */

/* Block until at least one record is available. Returns 0, -ETIMEDOUT, or
 * -EPIPE (closed and drained). timeout_ns as above. */
int gb_ring_wait_data(struct gb_ring *r, int64_t timeout_ns);

/* Non-blocking. Returns the next record (skipping PADs) and its size, or NULL
 * if empty (errno = 0) or malformed (errno = EPROTO, r->broken set). */
const void *gb_ring_peek(struct gb_ring *r, uint32_t *size_out);

/* Consume the record last returned by peek(); wakes a sleeping producer. */
void gb_ring_pop(struct gb_ring *r, uint32_t size);

/* ---- mailbox (see struct gb_mbox) ---------------------------------------- */

#define GB_MBOX_GETERROR 1u

/* Client: post a request on `req` (publishes anything pending first) and
 * return its sequence number. */
uint32_t gb_mbox_post(struct gb_ring *req, uint32_t op, uint32_t arg);
/* Client: wait for the reply to `seq` on `rsp`. 0 with *val set; 1 if data
 * arrived on the response ring first (drain it, then call again); -EPIPE if
 * the peer is gone. */
int gb_mbox_wait(struct gb_ring *rsp, uint32_t seq, uint32_t *val);
/* Server: is a request waiting? Fills the fields. */
int gb_mbox_pending(struct gb_ring *req, uint32_t *seq, uint32_t *op,
		    uint32_t *arg, uint32_t *tail_at);
/* Server: answer request `seq` (reply cell in `rsp`'s control block). */
void gb_mbox_reply(struct gb_ring *rsp, struct gb_ring *req, uint32_t seq,
		   uint32_t val);

/* ---- shutdown ---------------------------------------------------------- */

/* Blocking waits with an infinite timeout wake every GB_RING_WATCH_NS and
 * poll `watch_fd` (the control socket). If the peer died, the ring is marked
 * closed so the wait returns -EPIPE instead of hanging forever. */
#define GB_RING_WATCH_NS (100ll * 1000000ll)

/* Mark the ring closed (either side) and wake any sleeper. */
void gb_ring_close(struct gb_ring *r);

#endif /* GB_RING_H */

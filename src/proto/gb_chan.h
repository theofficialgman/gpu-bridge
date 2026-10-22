/* SPDX-License-Identifier: MIT */
/*
 * gb_chan.h - a bidirectional channel (request ring + response ring in one
 * memfd) and the message layer on top of it, including transparent
 * chunking of messages larger than a quarter of the ring.
 *
 * One channel per client thread; one server worker thread per channel.
 */
#ifndef GB_CHAN_H
#define GB_CHAN_H

#include <stddef.h>
#include <stdint.h>

#include "gb_ring.h"

#define GB_CHAN_RING_BYTES (1u << 20)
/* The request ring is larger: a message of up to a quarter of a ring is one
 * record the server reads in place, anything bigger is chunked and copied
 * out of the ring (client arrays of a few hundred KB are common). */
#define GB_CHAN_REQ_RING_BYTES (1u << 20)
#define GB_OP_CHUNK 0xFFFFFFFEu     /* continuation of a BIG message */
#define GB_CMD_F_BIG (1u << 3)      /* hdr is a BIG announcement */
#define GB_CMD_F_REPLY (1u << 4)    /* server -> client response */
#define GB_CMD_F_ERROR (1u << 5)    /* reply: request was rejected */
#define GB_CMD_F_UPCALL (1u << 6)   /* server -> client request (or its
					 * reply, client -> server, with
					 * GB_CMD_F_REPLY); opcode is GB_UP_* */
#define GB_MAX_MSG_BYTES (512u << 20)

struct gb_chan {
	int fd;           /* memfd backing both rings */
	void *region;
	size_t region_size;
	struct gb_ring req; /* client -> server */
	struct gb_ring rsp; /* server -> client */
	uint32_t seq;       /* client-side sequence counter */
};

/* Client: create memfd, init + attach both rings. 0 or -errno. */
int gb_chan_create(struct gb_chan *c, uint32_t req_bytes, uint32_t rsp_bytes);
/* Server: attach to a memfd received over the control socket. */
int gb_chan_attach(struct gb_chan *c, int fd, size_t region_size);
void gb_chan_destroy(struct gb_chan *c);
void gb_chan_close(struct gb_chan *c); /* mark both rings closed */

/* ---- outgoing messages --------------------------------------------------- */

struct gb_out {
	uint8_t *p;        /* where the caller writes the whole message */
	uint32_t size;
	struct gb_ring *ring;
	int heap;          /* p is malloc'd (message too big for one record) */
	int staged;        /* p is in the client's batch staging area: nothing to commit */
};

/* Reserve a message of `size` bytes (multiple of 8, >= 16). The caller fills
 * p[0..size) including the gb_cmd_hdr. Returns 0 or -errno (-EPIPE, ...). */
int gb_out_begin(struct gb_out *o, struct gb_ring *ring, uint32_t size);
/* Publish. For heap messages this streams BIG + CHUNK records. */
int gb_out_end(struct gb_out *o);

/* ---- incoming messages --------------------------------------------------- */

struct gb_in {
	const uint8_t *p;
	uint32_t size;
	struct gb_ring *ring;
	int heap;          /* p is malloc'd; otherwise it points into the ring */
};

/* Wait for the next complete message. 0, -ETIMEDOUT, -EPIPE or -EPROTO; 1
 * (only on a ring with mbox_watch) when the ring is drained and a mailbox
 * request is waiting instead. */
int gb_in_recv(struct gb_in *in, struct gb_ring *ring, int64_t timeout_ns);
void gb_in_release(struct gb_in *in);

#endif /* GB_CHAN_H */

/* SPDX-License-Identifier: MIT */
/*
 * gb_chan.c - channel + message layer. See gb_chan.h.
 */
#define _GNU_SOURCE
#include "gb_chan.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t ring_span(uint32_t bytes)
{
	return GB_ALIGN_UP(gb_ring_region_size(bytes), 4096u);
}

static uint32_t max_inline(const struct gb_ring *r)
{
	return r->size / 2;
}

int gb_chan_create(struct gb_chan *c, uint32_t req_bytes, uint32_t rsp_bytes)
{
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	size_t req_span = ring_span(req_bytes), rsp_span = ring_span(rsp_bytes);
	c->region_size = req_span + rsp_span;

	c->fd = memfd_create("gpu-bridge-chan", MFD_CLOEXEC);
	if (c->fd < 0)
		return -errno;
	if (ftruncate(c->fd, (off_t)c->region_size) < 0)
		goto fail;
	c->region = mmap(NULL, c->region_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, c->fd, 0);
	if (c->region == MAP_FAILED) {
		c->region = NULL;
		goto fail;
	}
	if (gb_ring_init(c->region, req_bytes) ||
	    gb_ring_init((uint8_t *)c->region + req_span, rsp_bytes) ||
	    gb_ring_attach(&c->req, c->region, req_span) ||
	    gb_ring_attach(&c->rsp, (uint8_t *)c->region + req_span, rsp_span)) {
		errno = EPROTO;
		goto fail;
	}
	return 0;
fail: {
	int e = errno;
	gb_chan_destroy(c);
	return -e;
}
}

int gb_chan_attach(struct gb_chan *c, int fd, size_t region_size)
{
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	c->region_size = region_size;
	if (region_size < 2 * 4096u || region_size > (size_t)1 << 31)
		return -EINVAL;

	c->region = mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);
	if (c->region == MAP_FAILED) {
		c->region = NULL;
		return -errno;
	}
	/* The request ring starts the region; its size comes from its (peer-
	 * written, untrusted) control block, then the response ring follows. */
	const struct gb_ring_ctrl *c0 = c->region;
	uint32_t req_bytes = c0->size;
	if (req_bytes < GB_RING_MIN_SIZE || req_bytes > GB_RING_MAX_SIZE ||
	    ring_span(req_bytes) >= region_size)
		return -EPROTO;
	size_t req_span = ring_span(req_bytes);
	int rc = gb_ring_attach(&c->req, c->region, req_span);
	if (rc)
		return rc;
	return gb_ring_attach(&c->rsp, (uint8_t *)c->region + req_span,
			      region_size - req_span);
}

void gb_chan_close(struct gb_chan *c)
{
	if (c->req.ctrl)
		gb_ring_close(&c->req);
	if (c->rsp.ctrl)
		gb_ring_close(&c->rsp);
}

void gb_chan_destroy(struct gb_chan *c)
{
	if (c->region)
		munmap(c->region, c->region_size);
	if (c->fd >= 0)
		close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

/* ---- outgoing ------------------------------------------------------------ */

int gb_out_begin(struct gb_out *o, struct gb_ring *ring, uint32_t size)
{
	o->ring = ring;
	o->size = size;
	o->heap = 0;
	if (size < sizeof(struct gb_cmd_hdr) || (size & (GB_REC_ALIGN - 1)))
		return -EINVAL;
	if (size > GB_MAX_MSG_BYTES)
		return -EMSGSIZE;

	if (size <= max_inline(ring)) {
		o->p = gb_ring_reserve(ring, size, -1);
		return o->p ? 0 : -errno;
	}
	o->p = malloc(size);
	o->heap = 1;
	return o->p ? 0 : -ENOMEM;
}

int gb_out_end(struct gb_out *o)
{
	struct gb_ring *r = o->ring;

	if (!o->heap) {
		gb_ring_commit(r);
		return 0;
	}

	/* BIG announcement: a gb_cmd_hdr copy with flags |= BIG and the total. */
	struct gb_cmd_hdr orig;
	memcpy(&orig, o->p, sizeof(orig));
	struct {
		struct gb_cmd_hdr h;
		uint32_t total;
		uint32_t reserved;
	} *ann = gb_ring_reserve(r, sizeof(*ann), -1);
	int rc = 0;

	if (!ann) {
		rc = -errno;
		goto out;
	}
	ann->h = orig;
	ann->h.size = sizeof(*ann);
	ann->h.flags |= GB_CMD_F_BIG;
	ann->total = o->size;
	ann->reserved = 0;
	gb_ring_commit(r);

	uint32_t chunk_max = max_inline(r) - sizeof(struct gb_rec_hdr);
	for (uint32_t off = 0; off < o->size;) {
		uint32_t n = o->size - off;
		if (n > chunk_max)
			n = chunk_max;
		uint32_t rec = sizeof(struct gb_rec_hdr) + n;
		struct gb_rec_hdr *h = gb_ring_reserve(r, rec, -1);
		if (!h) {
			rc = -errno;
			goto out;
		}
		h->opcode = GB_OP_CHUNK;
		h->size = rec;
		memcpy(h + 1, o->p + off, n);
		gb_ring_commit(r);
		off += n;
	}
out:
	free(o->p);
	o->p = NULL;
	return rc;
}

/* ---- incoming ------------------------------------------------------------ */

static int wait_record(struct gb_ring *ring, int64_t timeout_ns,
		       const void **rec, uint32_t *size)
{
	for (;;) {
		*rec = gb_ring_peek(ring, size);
		if (*rec)
			return 0;
		if (errno == EPROTO || ring->broken)
			return -EPROTO;
		if (ring->mbox_watch) {
			uint32_t q, op, arg, ta;

			if (gb_mbox_pending(ring, &q, &op, &arg, &ta)) {
				/* look at the ring again: what was published
				 * before the request must run first */
				*rec = gb_ring_peek(ring, size);
				return *rec ? 0 : 1;
			}
		}
		int rc = gb_ring_wait_data(ring, timeout_ns);
		if (rc)
			return rc;
	}
}

int gb_in_recv(struct gb_in *in, struct gb_ring *ring, int64_t timeout_ns)
{
	const void *rec;
	uint32_t rsize;
	int rc = wait_record(ring, timeout_ns, &rec, &rsize);

	memset(in, 0, sizeof(*in));
	in->ring = ring;
	if (rc)
		return rc;
	if (rsize < sizeof(struct gb_cmd_hdr))
		return -EPROTO;

	struct gb_cmd_hdr h;
	memcpy(&h, rec, sizeof(h));
	if (!(h.flags & GB_CMD_F_BIG)) {
		if (h.size != rsize)
			return -EPROTO;
		in->p = rec;
		in->size = rsize;
		return 0;
	}

	/* BIG: announcement carries the total; chunks follow. */
	uint32_t total;
	if (rsize < sizeof(h) + 4)
		return -EPROTO;
	memcpy(&total, (const uint8_t *)rec + sizeof(h), 4);
	gb_ring_pop(ring, rsize);
	if (total < sizeof(h) || total > GB_MAX_MSG_BYTES ||
	    (total & (GB_REC_ALIGN - 1)))
		return -EPROTO;

	uint8_t *buf = malloc(total);
	if (!buf)
		return -ENOMEM;
	uint32_t got = 0;
	while (got < total) {
		struct gb_rec_hdr *c;
		uint32_t csize;
		rc = wait_record(ring, -1, (const void **)&c, &csize);
		if (rc || c->opcode != GB_OP_CHUNK ||
		    csize <= sizeof(struct gb_rec_hdr) ||
		    csize - sizeof(struct gb_rec_hdr) > total - got) {
			free(buf);
			return rc ? rc : -EPROTO;
		}
		uint32_t n = csize - sizeof(struct gb_rec_hdr);
		memcpy(buf + got, c + 1, n);
		got += n;
		gb_ring_pop(ring, csize);
	}
	in->p = buf;
	in->size = total;
	in->heap = 1;
	return 0;
}

void gb_in_release(struct gb_in *in)
{
	if (!in->p)
		return;
	if (in->heap)
		free((void *)in->p);
	else
		gb_ring_pop(in->ring, in->size);
	in->p = NULL;
}

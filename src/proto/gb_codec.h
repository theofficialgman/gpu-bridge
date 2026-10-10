/*
 * gb_codec.h - bounds-checked reader/writer used by the generated
 * encoders (client) and decoders (server).
 *
 * Message layout (identical for requests and replies):
 *   gb_cmd_hdr (16) | fixed fields | 8-aligned variable blobs
 * 64-bit fields are 8-aligned relative to the message start; 32-bit fields
 * are 4-aligned. Offsets are relative to `base`, so the layout never depends
 * on where the buffer lives or on the compiling ABI.
 */
#ifndef GB_CODEC_H
#define GB_CODEC_H

#include <stdint.h>
#include <string.h>

#include "gb_proto.h"

/* Sentinel byte-length meaning "this pointer argument was NULL". */
#define GB_NULL_LEN 0xFFFFFFFFu
/* Upper bound for any single array argument (sanity / DoS guard). */
#define GB_MAX_ARRAY_BYTES (256u << 20)

/* ---- writer ------------------------------------------------------------ */

struct gb_w {
	uint8_t *base;
	uint32_t off;
};

static inline void gb_w_init(struct gb_w *w, void *base)
{
	w->base = base;
	w->off = sizeof(struct gb_cmd_hdr);
}

static inline void gb_w_u32(struct gb_w *w, uint32_t v)
{
	memcpy(w->base + w->off, &v, 4);
	w->off += 4;
}

static inline void gb_w_u64(struct gb_w *w, uint64_t v)
{
	w->off = GB_ALIGN_UP(w->off, 8u);
	memcpy(w->base + w->off, &v, 8);
	w->off += 8;
}

static inline void gb_w_f32(struct gb_w *w, float v)
{
	memcpy(w->base + w->off, &v, 4);
	w->off += 4;
}

static inline void gb_w_f64(struct gb_w *w, double v)
{
	uint64_t b;
	memcpy(&b, &v, 8);
	gb_w_u64(w, b);
}

/* Reserve `bytes` of 8-aligned blob space; returns a pointer into the msg. */
static inline void *gb_w_blob(struct gb_w *w, uint32_t bytes)
{
	w->off = GB_ALIGN_UP(w->off, 8u);
	void *p = w->base + w->off;
	w->off += bytes;
	return p;
}

/* ---- reader ------------------------------------------------------------ */

struct gb_r {
	const uint8_t *base;
	uint32_t size;
	uint32_t off;
	int err; /* sticky: set on any out-of-bounds read */
};

static inline void gb_r_init(struct gb_r *r, const void *base, uint32_t size)
{
	r->base = base;
	r->size = size;
	r->off = sizeof(struct gb_cmd_hdr);
	r->err = size < sizeof(struct gb_cmd_hdr);
}

static inline uint32_t gb_r_u32(struct gb_r *r)
{
	uint32_t v = 0;
	if (r->err || r->size - r->off < 4 || r->off > r->size) {
		r->err = 1;
		return 0;
	}
	memcpy(&v, r->base + r->off, 4);
	r->off += 4;
	return v;
}

static inline uint64_t gb_r_u64(struct gb_r *r)
{
	uint64_t v = 0;
	uint32_t o = GB_ALIGN_UP(r->off, 8u);
	if (r->err || o > r->size || r->size - o < 8) {
		r->err = 1;
		return 0;
	}
	memcpy(&v, r->base + o, 8);
	r->off = o + 8;
	return v;
}

static inline float gb_r_f32(struct gb_r *r)
{
	uint32_t b = gb_r_u32(r);
	float v;
	memcpy(&v, &b, 4);
	return v;
}

static inline double gb_r_f64(struct gb_r *r)
{
	uint64_t b = gb_r_u64(r);
	double v;
	memcpy(&v, &b, 8);
	return v;
}

/* Returns a pointer to `bytes` of 8-aligned blob data, or NULL (and sets
 * err) if it would run past the end of the message. */
static inline const void *gb_r_blob(struct gb_r *r, uint32_t bytes)
{
	uint32_t o = GB_ALIGN_UP(r->off, 8u);
	if (r->err || o > r->size || r->size - o < bytes) {
		r->err = 1;
		return NULL;
	}
	r->off = o + bytes;
	return r->base + o;
}

#endif /* GB_CODEC_H */

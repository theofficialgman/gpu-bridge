/*
 * gb_gl_sizes.h - size rules for COMPSIZE() arguments and EGL attribute
 * lists. Compiled into BOTH the client and the server: the client uses these
 * to size what it sends, the server recomputes with the same functions (using
 * native state) and rejects any mismatch.
 *
 * Rule (ARCHITECTURE.md 5.3): an unknown combination returns -1, never a
 * guess. Callers treat -1 as "do not hand the native driver a pointer".
 */
#ifndef GB_GL_SIZES_H
#define GB_GL_SIZES_H

#include <stdint.h>

#define GB_EGL_NONE 0x3038

/* ---- pname -> element count (glGet*v family and friends) ---------------- */

/* geti(ctx, pname) fetches an integer GL state value (client: cached SYNC
 * call, server: native glGetIntegerv). Only used for the few pnames whose
 * result length is itself state. */
typedef int (*gb_geti_fn)(void *ctx, uint32_t pname);

static inline int gb_pname_count_state(uint32_t pname, gb_geti_fn geti, void *ctx)
{
	switch (pname) {
	case 0x0BA2: /* GL_VIEWPORT */
	case 0x0C10: /* GL_SCISSOR_BOX */
	case 0x0C22: /* GL_COLOR_CLEAR_VALUE */
	case 0x0C23: /* GL_COLOR_WRITEMASK */
	case 0x8005: /* GL_BLEND_COLOR */
		return 4;
	case 0x846E: /* GL_ALIASED_LINE_WIDTH_RANGE */
	case 0x846D: /* GL_ALIASED_POINT_SIZE_RANGE */
	case 0x0B70: /* GL_DEPTH_RANGE */
	case 0x0D3A: /* GL_MAX_VIEWPORT_DIMS */
		return 2;
	case 0x86A3: /* GL_COMPRESSED_TEXTURE_FORMATS */
		return geti(ctx, 0x86A2); /* GL_NUM_COMPRESSED_TEXTURE_FORMATS */
	case 0x87FF: /* GL_PROGRAM_BINARY_FORMATS */
		return geti(ctx, 0x87FE); /* GL_NUM_PROGRAM_BINARY_FORMATS */
	case 0x8DF8: /* GL_SHADER_BINARY_FORMATS */
		return geti(ctx, 0x8DF9); /* GL_NUM_SHADER_BINARY_FORMATS */
	default:
		return 1;
	}
}

/* Other pname-sized queries (texture/shader/program/buffer parameters). */
static inline int gb_pname_count_param(uint32_t pname)
{
	switch (pname) {
	case 0x1004: /* GL_TEXTURE_BORDER_COLOR */
		return 4;
	case 0x8267: /* GL_COMPUTE_WORK_GROUP_SIZE (glGetProgramiv, ES 3.1) */
		return 3;
	default:
		return 1;
	}
}

/* ---- pixel rectangles ----------------------------------------------------- */

static inline int gb_format_components(uint32_t f)
{
	switch (f) {
	case 0x1901: /* STENCIL_INDEX */
	case 0x1902: /* DEPTH_COMPONENT */
	case 0x1903: /* RED */
	case 0x1906: /* ALPHA */
	case 0x1909: /* LUMINANCE */
	case 0x8D94: /* RED_INTEGER */
		return 1;
	case 0x190A: /* LUMINANCE_ALPHA */
	case 0x8227: /* RG */
	case 0x8228: /* RG_INTEGER */
	case 0x84F9: /* DEPTH_STENCIL (only with packed types) */
		return 2;
	case 0x1907: /* RGB */
	case 0x8D98: /* RGB_INTEGER */
		return 3;
	case 0x1908: /* RGBA */
	case 0x8D99: /* RGBA_INTEGER */
	case 0x80E1: /* BGRA_EXT */
		return 4;
	default:
		return -1;
	}
}

/* Bytes per pixel for packed types, 0 if `t` is not packed, -1 unknown. */
static inline int gb_packed_type_bytes(uint32_t t)
{
	switch (t) {
	case 0x8033: /* UNSIGNED_SHORT_4_4_4_4 */
	case 0x8034: /* UNSIGNED_SHORT_5_5_5_1 */
	case 0x8363: /* UNSIGNED_SHORT_5_6_5 */
	case 0x8365: /* UNSIGNED_SHORT_4_4_4_4_REV_EXT */
	case 0x8366: /* UNSIGNED_SHORT_1_5_5_5_REV_EXT */
		return 2;
	case 0x8368: /* UNSIGNED_INT_2_10_10_10_REV */
	case 0x8C3B: /* UNSIGNED_INT_10F_11F_11F_REV */
	case 0x8C3E: /* UNSIGNED_INT_5_9_9_9_REV */
	case 0x84FA: /* UNSIGNED_INT_24_8 */
		return 4;
	case 0x8DAD: /* FLOAT_32_UNSIGNED_INT_24_8_REV */
		return 8;
	default:
		return 0;
	}
}

static inline int gb_type_bytes(uint32_t t)
{
	switch (t) {
	case 0x1400: /* BYTE */
	case 0x1401: /* UNSIGNED_BYTE */
		return 1;
	case 0x1402: /* SHORT */
	case 0x1403: /* UNSIGNED_SHORT */
	case 0x140B: /* HALF_FLOAT */
	case 0x8D61: /* HALF_FLOAT_OES */
		return 2;
	case 0x1404: /* INT */
	case 0x1405: /* UNSIGNED_INT */
	case 0x1406: /* FLOAT */
		return 4;
	default:
		return -1;
	}
}

struct gb_pixstore {
	int alignment;   /* 1,2,4,8 */
	int row_length;  /* 0 = use width */
	int skip_pixels;
	int skip_rows;
};

/* Bytes the driver will touch for a w*h rectangle, or -1 if unknown. */
static inline int64_t gb_pixel_bytes(uint32_t format, uint32_t type, int64_t w,
				     int64_t h, const struct gb_pixstore *ps)
{
	if (w < 0 || h < 0)
		return -1;
	if (w == 0 || h == 0)
		return 0;
	int bpp = gb_packed_type_bytes(type);
	if (bpp == 0) {
		int c = gb_format_components(format), tb = gb_type_bytes(type);
		if (c < 0 || tb < 0)
			return -1;
		bpp = c * tb;
	}
	int a = ps->alignment;
	if (a != 1 && a != 2 && a != 4 && a != 8)
		return -1;
	int64_t rl = ps->row_length > 0 ? ps->row_length : w;
	if (rl < w || w > (1 << 20) || h > (1 << 20) || rl > (1 << 20))
		return -1;
	int64_t row = rl * bpp;
	int64_t stride = ((row + a - 1) / a) * a;
	int64_t off = (int64_t)ps->skip_rows * stride +
		      (int64_t)ps->skip_pixels * bpp;
	return off + stride * (h - 1) + w * bpp;
}

/* Bytes of one vertex-attribute element (all components), or -1. */
static inline int gb_attrib_elem_bytes(uint32_t type, int size)
{
	switch (type) {
	case 0x1400: case 0x1401: return size;       /* BYTE, UNSIGNED_BYTE */
	case 0x1402: case 0x1403: return 2 * size;   /* SHORT, UNSIGNED_SHORT */
	case 0x140B: case 0x8D61: return 2 * size;   /* HALF_FLOAT(_OES) */
	case 0x1404: case 0x1405: return 4 * size;   /* INT, UNSIGNED_INT */
	case 0x1406: case 0x140C: return 4 * size;   /* FLOAT, FIXED */
	case 0x8368: case 0x8D9F: return 4;          /* 2_10_10_10_REV */
	default: return -1;
	}
}

/* ---- EGL attribute lists (EGL_NONE terminated key/value pairs) ---------- */

#define GB_ATTRIB_MAX 2048

/* Number of elements including the terminating EGL_NONE, or -1. */
static inline int64_t gb_attrib_count_EGLint(const int32_t *p)
{
	for (int64_t i = 0; i < GB_ATTRIB_MAX; i += 2)
		if (p[i] == GB_EGL_NONE)
			return i + 1;
	return -1;
}

static inline int64_t gb_attrib_count_EGLAttrib(const intptr_t *p)
{
	for (int64_t i = 0; i < GB_ATTRIB_MAX; i += 2)
		if (p[i] == (intptr_t)GB_EGL_NONE)
			return i + 1;
	return -1;
}

/* Server-side validation of what arrived: must be well-formed pairs ending in
 * EGL_NONE exactly at the last element. `elem64` selects 8-byte elements. */
static inline int gb_attrib_valid_EGLint(const void *b, int64_t n)
{
	const int32_t *p = b;
	int64_t i = 0;
	while (i < n) {
		if (p[i] == GB_EGL_NONE)
			return i == n - 1;
		i += 2;
	}
	return 0;
}

static inline int gb_attrib_valid_EGLAttrib(const void *b, int64_t n)
{
	const int64_t *p = b;
	int64_t i = 0;
	while (i < n) {
		if (p[i] == GB_EGL_NONE)
			return i == n - 1;
		i += 2;
	}
	return 0;
}

#endif /* GB_GL_SIZES_H */

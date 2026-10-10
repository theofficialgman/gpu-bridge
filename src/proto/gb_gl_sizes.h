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

#include "gb_carr.h"
#include "gb_pname_classes.h"

#define GB_EGL_NONE 0x3038

/* ---- pname -> element count (glGet*v family and friends) ---------------- */

/* geti(ctx, pname) fetches an integer GL state value (client: cached SYNC
 * call, server: native glGetIntegerv). Only used for the few pnames whose
 * result length is itself state. */
typedef int (*gb_geti_fn)(void *ctx, uint32_t pname);

/* Element count for the pname-sized commands of class `cls`. A too-small
 * answer is made harmless server-side (512 bytes of slack) and a too-large
 * one just wastes a few bytes: unknown pnames return 1. */
static inline int gb_pname_count_class(int cls, uint32_t pname,
				       gb_geti_fn geti, void *ctx)
{
	switch (cls) {
	case GB_PNC_getv:
		switch (pname) {
		case 0x0BA6: /* MODELVIEW_MATRIX */
		case 0x0BA7: /* PROJECTION_MATRIX */
		case 0x0BA8: /* TEXTURE_MATRIX */
		case 0x80B1: /* COLOR_MATRIX */
		case 0x84E3: case 0x84E4: case 0x84E5: case 0x84E6: /* TRANSPOSE_* */
			return 16;
		case 0x0BA2: /* VIEWPORT */
		case 0x0C10: /* SCISSOR_BOX */
		case 0x0C22: /* COLOR_CLEAR_VALUE */
		case 0x0C23: /* COLOR_WRITEMASK */
		case 0x8005: /* BLEND_COLOR */
		case 0x0B00: /* CURRENT_COLOR */
		case 0x0B03: /* CURRENT_TEXTURE_COORDS */
		case 0x0B07: /* CURRENT_RASTER_POSITION */
		case 0x0B04: /* CURRENT_RASTER_COLOR */
		case 0x0B06: /* CURRENT_RASTER_TEXTURE_COORDS */
		case 0x845F: /* CURRENT_RASTER_SECONDARY_COLOR */
		case 0x8459: /* CURRENT_SECONDARY_COLOR */
		case 0x0B80: /* ACCUM_CLEAR_VALUE */
		case 0x0B66: /* FOG_COLOR */
		case 0x0B53: /* LIGHT_MODEL_AMBIENT */
		case 0x0DD2: /* MAP2_GRID_DOMAIN */
			return 4;
		case 0x0B02: /* CURRENT_NORMAL */
		case 0x8129: /* POINT_DISTANCE_ATTENUATION */
		case 0x91BE: /* MAX_COMPUTE_WORK_GROUP_COUNT */
		case 0x91BF: /* MAX_COMPUTE_WORK_GROUP_SIZE */
			return 3;
		case 0x0B70: /* DEPTH_RANGE */
		case 0x0B22: /* LINE_WIDTH_RANGE / SMOOTH_LINE_WIDTH_RANGE */
		case 0x0B12: /* POINT_SIZE_RANGE / SMOOTH_POINT_SIZE_RANGE */
		case 0x846E: /* ALIASED_LINE_WIDTH_RANGE */
		case 0x846D: /* ALIASED_POINT_SIZE_RANGE */
		case 0x0D3A: /* MAX_VIEWPORT_DIMS */
		case 0x0B40: /* POLYGON_MODE */
		case 0x0DD0: /* MAP1_GRID_DOMAIN */
		case 0x0DD3: /* MAP2_GRID_SEGMENTS */
		case 0x825D: /* VIEWPORT_BOUNDS_RANGE */
			return 2;
		case 0x86A3: /* COMPRESSED_TEXTURE_FORMATS */
			return geti(ctx, 0x86A2);
		case 0x87FF: /* PROGRAM_BINARY_FORMATS */
			return geti(ctx, 0x87FE);
		case 0x8DF8: /* SHADER_BINARY_FORMATS */
			return geti(ctx, 0x8DF9);
		default:
			return 1;
		}
	case GB_PNC_geti_v:
		switch (pname) {
		case 0x0BA2: case 0x0C10: case 0x0C23: /* viewport/scissor/mask */
			return 4;
		case 0x0B70: /* DEPTH_RANGE */
			return 2;
		case 0x91BE: case 0x91BF: /* compute work group count/size */
			return 3;
		default:
			return 1;
		}
	case GB_PNC_light:
		switch (pname) {
		case 0x1200: case 0x1201: case 0x1202: case 0x1203:
			return 4; /* AMBIENT DIFFUSE SPECULAR POSITION */
		case 0x1204:
			return 3; /* SPOT_DIRECTION */
		default:
			return 1;
		}
	case GB_PNC_material:
		switch (pname) {
		case 0x1200: case 0x1201: case 0x1202: case 0x1600: case 0x1602:
			return 4; /* AMBIENT DIFFUSE SPECULAR EMISSION AMB_AND_DIFF */
		case 0x1603:
			return 3; /* COLOR_INDEXES */
		default:
			return 1;
		}
	case GB_PNC_texenv:
		return pname == 0x2201 ? 4 : 1; /* TEXTURE_ENV_COLOR */
	case GB_PNC_texgen:
		return (pname == 0x2501 || pname == 0x2502) ? 4 : 1;
	case GB_PNC_lightmodel:
		return pname == 0x0B53 ? 4 : 1;
	case GB_PNC_fog:
		return pname == 0x0B66 ? 4 : 1;
	case GB_PNC_pointparam:
		return pname == 0x8129 ? 3 : 1;
	case GB_PNC_texparam:
		return (pname == 0x1004 || pname == 0x8E46) ? 4 : 1;
	case GB_PNC_clearbuffer:
		return pname == 0x1800 ? 4 : 1; /* COLOR vs DEPTH/STENCIL */
	default:
		return pname == 0x8267 ? 3 : 1; /* COMPUTE_WORK_GROUP_SIZE */
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
	int alignment;     /* 1,2,4,8 */
	int row_length;    /* 0 = use width */
	int skip_pixels;
	int skip_rows;
	int image_height;  /* 0 = use height (3D) */
	int skip_images;
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

/* Bytes touched by a w x h x d volume (d = 1 for 2D). -1 if unknown. */
static inline int64_t gb_pixel_bytes3(uint32_t format, uint32_t type,
				      int64_t w, int64_t h, int64_t d,
				      const struct gb_pixstore *ps)
{
	if (d <= 1 && ps->skip_images == 0)
		return gb_pixel_bytes(format, type, w, h, ps);
	if (w < 0 || h < 0 || d < 0)
		return -1;
	if (w == 0 || h == 0 || d == 0)
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
	int64_t ih = ps->image_height > 0 ? ps->image_height : h;
	if (rl < w || ih < h || w > (1 << 16) || h > (1 << 16) || d > (1 << 16))
		return -1;
	int64_t stride = (((rl * bpp) + a - 1) / a) * a;
	int64_t image = stride * ih;
	int64_t off = ((int64_t)ps->skip_images * image) +
		      (int64_t)ps->skip_rows * stride +
		      (int64_t)ps->skip_pixels * bpp;
	return off + image * (d - 1) + stride * (h - 1) + w * bpp;
}

/* glBitmap / glPolygonStipple: 1 bit per pixel, rows padded to alignment. */
static inline int64_t gb_bitmap_bytes(int64_t w, int64_t h,
				      const struct gb_pixstore *ps)
{
	if (w < 0 || h < 0 || w > (1 << 20) || h > (1 << 20))
		return -1;
	if (w == 0 || h == 0)
		return 0;
	int a = ps->alignment;
	if (a != 1 && a != 2 && a != 4 && a != 8)
		return -1;
	int64_t rl = ps->row_length > 0 ? ps->row_length : w;
	int64_t row = (rl + 7) / 8;
	int64_t stride = ((row + a - 1) / a) * a;
	int64_t off = (int64_t)ps->skip_rows * stride +
		      (int64_t)(ps->skip_pixels / 8);
	return off + stride * (h - 1) + (w + 7) / 8;
}

/* glCallLists payload size. */
static inline int64_t gb_callists_bytes(int64_t n, uint32_t type)
{
	int64_t sz;
	switch (type) {
	case 0x1400: case 0x1401: sz = 1; break; /* BYTE, UNSIGNED_BYTE */
	case 0x1402: case 0x1403: sz = 2; break; /* SHORT, UNSIGNED_SHORT */
	case 0x1404: case 0x1405: case 0x1406: sz = 4; break; /* INT UINT FLOAT */
	case 0x1407: sz = 2; break;              /* 2_BYTES */
	case 0x1408: sz = 3; break;              /* 3_BYTES */
	case 0x1409: sz = 4; break;              /* 4_BYTES */
	default: return -1;
	}
	return n < 0 ? -1 : n * sz;
}

/* glMap1x / glMap2x: element count touched in `points`, or -1. */
static inline int gb_map_components(uint32_t target)
{
	switch (target) {
	case 0x0D91: case 0x0DB1: return 1; /* INDEX */
	case 0x0D90: case 0x0DB0: return 4; /* COLOR_4 */
	case 0x0D92: case 0x0DB2: return 3; /* NORMAL */
	case 0x0D93: case 0x0DB3: return 1; /* TEXTURE_COORD_1 */
	case 0x0D94: case 0x0DB4: return 2; /* TEXTURE_COORD_2 */
	case 0x0D95: case 0x0DB5: return 3; /* TEXTURE_COORD_3 */
	case 0x0D96: case 0x0DB6: return 4; /* TEXTURE_COORD_4 */
	case 0x0D97: case 0x0DB7: return 3; /* VERTEX_3 */
	case 0x0D98: case 0x0DB8: return 4; /* VERTEX_4 */
	default: return -1;
	}
}

static inline int64_t gb_map1_count(uint32_t target, int64_t stride,
				    int64_t order)
{
	int k = gb_map_components(target);
	if (k < 0 || stride < k || order < 1 || order > 4096)
		return -1;
	return (order - 1) * stride + k;
}

static inline int64_t gb_map2_count(uint32_t target, int64_t ustride,
				    int64_t uorder, int64_t vstride,
				    int64_t vorder)
{
	int k = gb_map_components(target);
	if (k < 0 || ustride < k || vstride < k || uorder < 1 || vorder < 1 ||
	    uorder > 4096 || vorder > 4096)
		return -1;
	return (vorder - 1) * vstride + (uorder - 1) * ustride + k;
}

#define GB_BYTES_callists(op, n, t) gb_callists_bytes((n), (uint32_t)(t))
#define GB_BYTES_map1(op, t, st, o) gb_map1_count((uint32_t)(t), (st), (o))
#define GB_BYTES_map2(op, t, us, uo, vs, vo) \
	gb_map2_count((uint32_t)(t), (us), (uo), (vs), (vo))

/* Bytes of one vertex-attribute element (all components), or -1. */
static inline int gb_attrib_elem_bytes(uint32_t type, int size)
{
	switch (type) {
	case 0x1400: case 0x1401: return size;       /* BYTE, UNSIGNED_BYTE */
	case 0x1402: case 0x1403: return 2 * size;   /* SHORT, UNSIGNED_SHORT */
	case 0x140B: case 0x8D61: return 2 * size;   /* HALF_FLOAT(_OES) */
	case 0x1404: case 0x1405: return 4 * size;   /* INT, UNSIGNED_INT */
	case 0x1406: case 0x140C: return 4 * size;   /* FLOAT, FIXED */
	case 0x140A: return 8 * size;                /* DOUBLE */
	case 0x8368: case 0x8D9F: return 4;          /* 2_10_10_10_REV */
	case 0x8C3B: return 4;                       /* 10F_11F_11F_REV */
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

/* GLX attribute lists are ints, terminated by None (0). glXChooseVisual's
 * legacy form has bare booleans (GLX_USE_GL, GLX_RGBA, GLX_DOUBLEBUFFER,
 * GLX_STEREO) and key/value pairs for the rest; FBConfig/context attribute
 * lists are always key/value pairs. Returns elements incl. terminator or -1. */
static inline int gb_glx_is_bare_bool(int k)
{
	return k == 1 || k == 4 || k == 5 || k == 6;
}

static inline int64_t gb_glx_attrib_count(const int *p, int visual)
{
	int64_t i = 0;
	while (i < GB_ATTRIB_MAX) {
		if (p[i] == 0)
			return i + 1;
		i += (visual && gb_glx_is_bare_bool(p[i])) ? 1 : 2;
	}
	return -1;
}

static inline int gb_glx_attrib_valid(const void *b, int64_t n, int visual)
{
	const int *p = b;
	int64_t i = 0;
	while (i < n) {
		if (p[i] == 0)
			return i == n - 1;
		i += (visual && gb_glx_is_bare_bool(p[i])) ? 1 : 2;
	}
	return 0;
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

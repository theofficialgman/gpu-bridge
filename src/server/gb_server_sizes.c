/*
 * gb_server_sizes.c - size rules that need native GL state.
 *
 * The client sizes what it *sends* from its shadow state; the server sizes
 * the same thing from the real driver state and rejects any mismatch. Out
 * arrays whose length only the server can know are sized here.
 *
 * Queries must not raise GL errors the application would later see, so
 * state that only exists in some API flavors is queried only there.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_server_impl.h"

enum { FL_ES2 = 1, FL_ES3, FL_DESKTOP };

static int geti(uint32_t pname)
{
	return gb_sv_geti(NULL, pname);
}

/* Which GL flavor is current on this thread (cached per context pair). */
static int flavor(void)
{
	static __thread void *e_key, *g_key;
	static __thread int fl;
	GB_NATIVE(eglGetCurrentContext, EGLContext, (void));
	GB_NATIVE(glXGetCurrentContext, GLXContext, (void));
	GB_NATIVE(glGetString, const GLubyte *, (GLenum));
	void *e = fn_eglGetCurrentContext ? (void *)fn_eglGetCurrentContext()
					  : NULL;
	void *g = fn_glXGetCurrentContext ? (void *)fn_glXGetCurrentContext()
					  : NULL;

	if (fl && e == e_key && g == g_key)
		return fl;
	e_key = e;
	g_key = g;
	/* An EGL context may be desktop GL (EGL_OPENGL_API); the version string
	 * is what tells the flavors apart ("OpenGL ES 3.2 ..." vs "4.6.0 ..."). */
	const char *v = (e || g) && fn_glGetString ?
		(const char *)fn_glGetString(GL_VERSION) : NULL;

	if (v && !strncmp(v, "OpenGL ES", 9))
		fl = (!strncmp(v, "OpenGL ES 2", 11) ||
		      !strncmp(v, "OpenGL ES-CM", 12)) ? FL_ES2 : FL_ES3;
	else
		fl = FL_DESKTOP;
	return fl;
}

/* Is the current context OpenGL ES (by its version string, not by which
 * window-system API created it: GLX can make ES contexts too)? */
int gb_sv_ctx_is_es(void)
{
	return flavor() != FL_DESKTOP;
}

/* Desktop GL version at least major.minor (queried states exist only there) */
static int desktop_at_least(int major, int minor)
{
	GB_NATIVE(glGetString, const GLubyte *, (GLenum));
	const char *v = fn_glGetString ? (const char *)fn_glGetString(GL_VERSION)
				       : NULL;
	int ma, mi = 0;

	if (!v || !strncmp(v, "OpenGL ES", 9))
		return 0;
	ma = atoi(v);
	while (*v && *v != '.')
		v++;
	if (*v == '.')
		mi = atoi(v + 1);
	return ma > major || (ma == major && mi >= minor);
}

static void pixstore(int mode, struct gb_pixstore *ps)
{
	int pack = mode == GB_PM_PACK, fl = flavor();

	memset(ps, 0, sizeof(*ps));
	ps->alignment = geti(pack ? 0x0D05 : 0x0CF5);
	if (fl == FL_ES2)
		return;
	ps->row_length = geti(pack ? 0x0D02 : 0x0CF2);
	ps->skip_pixels = geti(pack ? 0x0D04 : 0x0CF4);
	ps->skip_rows = geti(pack ? 0x0D03 : 0x0CF3);
	if (!pack || fl == FL_DESKTOP) {
		ps->image_height = geti(pack ? 0x806C : 0x806E);
		ps->skip_images = geti(pack ? 0x806B : 0x806D);
	}
	if (!pack && fl == FL_DESKTOP && desktop_at_least(4, 2)) {
		/* ARB_compressed_texture_pixel_storage (core in 4.2) */
		ps->cb_width = geti(0x9127);
		ps->cb_height = geti(0x9128);
		ps->cb_depth = geti(0x9129);
		ps->cb_size = geti(0x912A);
	}
}

int64_t gb_sv_compressed_bytes(int64_t size, int64_t w, int64_t h, int64_t d)
{
	struct gb_pixstore ps;

	pixstore(GB_PM_UNPACK, &ps);
	return gb_compressed_bytes(size, w, h, d, &ps);
}

int64_t gb_sv_pixel_bytes(int mode, uint32_t format, uint32_t type, int64_t w,
			  int64_t h, int64_t d)
{
	struct gb_pixstore ps;

	pixstore(mode, &ps);
	return gb_pixel_bytes3(format, type, w, h, d, &ps);
}

int64_t gb_sv_bitmap_bytes(int mode, int64_t w, int64_t h)
{
	struct gb_pixstore ps;

	pixstore(mode, &ps);
	return gb_bitmap_bytes(w, h, &ps);
}

/* ---- server-sized out arrays ------------------------------------------------ */

static int uniform_components(uint32_t type)
{
	switch (type) {
	case 0x8B51: case 0x8B54: case 0x8B58: case 0x8DC7: case 0x8FFD:
		return 3; /* VEC3 variants */
	case 0x8B52: case 0x8B55: case 0x8B59: case 0x8DC8: case 0x8FFE:
	case 0x8B5A: case 0x8F46:
		return 4; /* VEC4, MAT2 */
	case 0x8B50: case 0x8B53: case 0x8B57: case 0x8DC6: case 0x8FFC:
		return 2; /* VEC2 */
	case 0x8B5B: case 0x8F47:
		return 9; /* MAT3 */
	case 0x8B5C: case 0x8F48:
		return 16; /* MAT4 */
	case 0x8B65: case 0x8B67: case 0x8F49: case 0x8F4B:
		return 6; /* MAT2x3, MAT3x2 */
	case 0x8B66: case 0x8B69: case 0x8F4A: case 0x8F4D:
		return 8; /* MAT2x4, MAT4x2 */
	case 0x8B68: case 0x8B6A: case 0x8F4C: case 0x8F4E:
		return 12; /* MAT3x4, MAT4x3 */
	default:
		return 1; /* scalars, bool, samplers, images */
	}
}

int64_t gb_sv_sz_uniform(struct gb_sth *t, int wsz, uint32_t program,
			 int32_t location)
{
	GB_NATIVE(glGetProgramiv, void, (GLuint, GLenum, GLint *));
	GB_NATIVE(glGetActiveUniform, void,
		  (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *,
		   GLchar *));
	GB_NATIVE(glGetUniformLocation, GLint, (GLuint, const GLchar *));
	GLint nu = 0, maxlen = 0;

	if (!fn_glGetProgramiv || !fn_glGetActiveUniform ||
	    !fn_glGetUniformLocation || location < 0)
		return -1;
	fn_glGetProgramiv(program, 0x8B86 /* ACTIVE_UNIFORMS */, &nu);
	fn_glGetProgramiv(program, 0x8B87 /* ACTIVE_UNIFORM_MAX_LENGTH */,
			  &maxlen);
	if (nu <= 0 || maxlen <= 0 || maxlen > 4096)
		return -1;
	char *name = gb_tmp(t, (size_t)maxlen + 16);
	char *elem = gb_tmp(t, (size_t)maxlen + 32);
	if (!name || !elem)
		return -1;

	for (GLint i = 0; i < nu; i++) {
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		fn_glGetActiveUniform(program, (GLuint)i, maxlen, &len, &size,
				      &type, name);
		if (len <= 0)
			continue;
		int comps = uniform_components(type);
		int is_arr = len > 3 && !strcmp(name + len - 3, "[0]");
		if (!is_arr) {
			if (fn_glGetUniformLocation(program, name) == location)
				return (int64_t)comps * wsz;
			continue;
		}
		name[len - 3] = 0;
		for (GLint k = 0; k < size; k++) {
			snprintf(elem, (size_t)maxlen + 32, "%s[%d]", name, k);
			if (fn_glGetUniformLocation(program, elem) == location)
				return (int64_t)comps * wsz;
		}
	}
	return -1;
}

int64_t gb_sv_sz_pixels_tex(struct gb_sth *t, uint32_t target, int32_t level,
			    uint32_t format, uint32_t type)
{
	GB_NATIVE(glGetTexLevelParameteriv, void, (GLenum, GLint, GLenum, GLint *));
	GLint w = 0, h = 1, d = 1;

	if (!fn_glGetTexLevelParameteriv || flavor() != FL_DESKTOP)
		return -1;
	fn_glGetTexLevelParameteriv(target, level, 0x1000 /* WIDTH */, &w);
	fn_glGetTexLevelParameteriv(target, level, 0x1001 /* HEIGHT */, &h);
	fn_glGetTexLevelParameteriv(target, level, 0x8071 /* DEPTH */, &d);
	/* skip-images / image-height apply to 3D-like targets only */
	int vol = target == 0x806F || target == 0x8C1A || target == 0x9009;

	t->px_dim[0] = w;
	t->px_dim[1] = h;
	t->px_dim[2] = vol ? d : GB_DEPTH_2D;
	return gb_sv_pixel_bytes(GB_PM_PACK, format, type, w, h,
				 vol ? d : GB_DEPTH_2D);
}

int64_t gb_sv_sz_pixels_dsa(struct gb_sth *t, int multi, uint32_t tex_or_unit,
			    uint32_t target, int32_t level, uint32_t format,
			    uint32_t type)
{
	GB_NATIVE(glGetTextureLevelParameterivEXT, void,
		  (GLuint, GLenum, GLint, GLenum, GLint *));
	GB_NATIVE(glGetMultiTexLevelParameterivEXT, void,
		  (GLenum, GLenum, GLint, GLenum, GLint *));
	GLint w = 0, h = 1, d = 1;

	if (flavor() != FL_DESKTOP)
		return -1;
	if (multi && fn_glGetMultiTexLevelParameterivEXT) {
		fn_glGetMultiTexLevelParameterivEXT(tex_or_unit, target, level,
						    0x1000, &w);
		fn_glGetMultiTexLevelParameterivEXT(tex_or_unit, target, level,
						    0x1001, &h);
		fn_glGetMultiTexLevelParameterivEXT(tex_or_unit, target, level,
						    0x8071, &d);
	} else if (!multi && fn_glGetTextureLevelParameterivEXT) {
		fn_glGetTextureLevelParameterivEXT(tex_or_unit, target, level,
						   0x1000, &w);
		fn_glGetTextureLevelParameterivEXT(tex_or_unit, target, level,
						   0x1001, &h);
		fn_glGetTextureLevelParameterivEXT(tex_or_unit, target, level,
						   0x8071, &d);
	} else {
		return -1;
	}
	int vol = target == 0x806F || target == 0x8C1A || target == 0x9009;

	t->px_dim[0] = w;
	t->px_dim[1] = h;
	t->px_dim[2] = vol ? d : GB_DEPTH_2D;
	return gb_sv_pixel_bytes(GB_PM_PACK, format, type, w, h,
				 vol ? d : GB_DEPTH_2D);
}

int64_t gb_sv_sz_ctex_dsa(struct gb_sth *t, int multi, uint32_t tex_or_unit,
			  uint32_t target, int32_t level)
{
	GB_NATIVE(glGetTextureLevelParameterivEXT, void,
		  (GLuint, GLenum, GLint, GLenum, GLint *));
	GB_NATIVE(glGetMultiTexLevelParameterivEXT, void,
		  (GLenum, GLenum, GLint, GLenum, GLint *));
	GLint sz = 0;

	if (multi && fn_glGetMultiTexLevelParameterivEXT)
		fn_glGetMultiTexLevelParameterivEXT(tex_or_unit, target, level,
						    0x86A0, &sz);
	else if (!multi && fn_glGetTextureLevelParameterivEXT)
		fn_glGetTextureLevelParameterivEXT(tex_or_unit, target, level,
						   0x86A0, &sz);
	else
		return -1;
	return sz;
}

int64_t gb_sv_sz_progstring_named(struct gb_sth *t, uint32_t program,
				  uint32_t target)
{
	GB_NATIVE(glGetNamedProgramivEXT, void,
		  (GLuint, GLenum, GLenum, GLint *));
	GLint n = 0;

	if (!fn_glGetNamedProgramivEXT)
		return -1;
	fn_glGetNamedProgramivEXT(program, target, 0x8627 /* PROGRAM_LENGTH */,
				  &n);
	return n < 0 ? -1 : n;
}

int64_t gb_sv_sz_imaging(struct gb_sth *t, int which, uint32_t target,
			 uint32_t format, uint32_t type)
{
	GB_NATIVE(glGetColorTableParameteriv, void, (GLenum, GLenum, GLint *));
	GB_NATIVE(glGetConvolutionParameteriv, void, (GLenum, GLenum, GLint *));
	GB_NATIVE(glGetHistogramParameteriv, void, (GLenum, GLenum, GLint *));
	GLint w = 0, h = 1;

	if (flavor() != FL_DESKTOP)
		return -1;
	switch (which) {
	case 0: /* color table */
		if (!fn_glGetColorTableParameteriv)
			return -1;
		fn_glGetColorTableParameteriv(target, 0x80D9, &w);
		break;
	case 1: /* convolution filter (2D has a height) */
	case 2: /* separable row */
	case 3: /* separable column: `height` pixels */
		if (!fn_glGetConvolutionParameteriv)
			return -1;
		fn_glGetConvolutionParameteriv(target, 0x8018, &w);
		if (which == 1 && target != 0x8010 /* CONVOLUTION_1D */)
			fn_glGetConvolutionParameteriv(target, 0x8019, &h);
		if (which == 3) {
			fn_glGetConvolutionParameteriv(target, 0x8019, &w);
			h = 1;
		}
		break;
	case 4: /* histogram */
		if (!fn_glGetHistogramParameteriv)
			return -1;
		fn_glGetHistogramParameteriv(target, 0x8026, &w);
		break;
	default: /* minmax: two pixels (minimum, maximum) */
		w = 2;
		break;
	}
	if (w < 0 || h < 0)
		return -1;
	return gb_sv_pixel_bytes(GB_PM_PACK, format, type, w, h, GB_DEPTH_2D);
}

int64_t gb_sv_sz_progstring_nv(struct gb_sth *t, uint32_t id, uint32_t pname)
{
	GB_NATIVE(glGetProgramivNV, void, (GLuint, GLenum, GLint *));
	GLint n = 0;

	if (!fn_glGetProgramivNV)
		return -1;
	fn_glGetProgramivNV(id, 0x8E2E /* PROGRAM_LENGTH_NV */, &n);
	return n < 0 ? -1 : n;
}

int64_t gb_sv_sz_ctex(struct gb_sth *t, uint32_t target, int32_t level)
{
	GB_NATIVE(glGetTexLevelParameteriv, void, (GLenum, GLint, GLenum, GLint *));
	GLint sz = 0;

	if (!fn_glGetTexLevelParameteriv)
		return -1;
	fn_glGetTexLevelParameteriv(target, level,
				    0x86A0 /* COMPRESSED_IMAGE_SIZE */, &sz);
	return sz;
}

int64_t gb_sv_sz_pixelmap(struct gb_sth *t, int wsz, uint32_t map)
{
	if (map < 0x0C70 || map > 0x0C79)
		return -1;
	return (int64_t)geti(map - 0x0C70 + 0x0CB0) * wsz;
}

/* glGetMap{d,f,i}v: GL_ORDER / GL_DOMAIN have fixed counts; GL_COEFF is
 * components * order (map1) or components * uorder * vorder (map2). */
int64_t gb_sv_sz_map(struct gb_sth *t, int wsz, uint32_t target,
		     uint32_t query)
{
	static const int8_t comps[9] = { 4, 1, 3, 1, 2, 3, 4, 3, 4 };
	int two;
	int c;

	if (target >= 0x0D90 && target <= 0x0D98) {
		two = 0;
		c = comps[target - 0x0D90];
	} else if (target >= 0x0DB0 && target <= 0x0DB8) {
		two = 1;
		c = comps[target - 0x0DB0];
	} else {
		return -1;
	}
	switch (query) {
	case 0x0A01: /* ORDER */
		return (int64_t)(two ? 2 : 1) * wsz;
	case 0x0A02: /* DOMAIN */
		return (int64_t)(two ? 4 : 2) * wsz;
	case 0x0A00: { /* COEFF */
		GB_NATIVE(glGetMapiv, void, (GLenum, GLenum, GLint *));
		GLint ord[2] = { 0, 0 };

		if (!fn_glGetMapiv)
			return -1;
		fn_glGetMapiv(target, 0x0A01, ord);
		if (ord[0] < 0 || ord[1] < 0)
			return -1;
		return (int64_t)c * ord[0] * (two ? ord[1] : 1) * wsz;
	}
	default:
		return -1;
	}
}

int64_t gb_sv_sz_uniformblock(struct gb_sth *t, int wsz, uint32_t program,
			      uint32_t block, uint32_t pname)
{
	GB_NATIVE(glGetActiveUniformBlockiv, void,
		  (GLuint, GLuint, GLenum, GLint *));
	GLint n = 0;

	if (pname != 0x8A43 /* ACTIVE_UNIFORM_INDICES */)
		return wsz;
	if (!fn_glGetActiveUniformBlockiv)
		return -1;
	fn_glGetActiveUniformBlockiv(program, block, 0x8A42, &n);
	return n < 0 ? -1 : (int64_t)n * wsz;
}

int64_t gb_sv_sz_subr_uniform(struct gb_sth *t, int wsz, uint32_t program,
			      uint32_t shadertype, uint32_t index,
			      uint32_t pname)
{
	GB_NATIVE(glGetActiveSubroutineUniformiv, void,
		  (GLuint, GLenum, GLuint, GLenum, GLint *));
	GLint n = 0;

	if (pname != 0x8E4B /* COMPATIBLE_SUBROUTINES */)
		return wsz;
	if (!fn_glGetActiveSubroutineUniformiv)
		return -1;
	fn_glGetActiveSubroutineUniformiv(program, shadertype, index,
					  0x8E4A /* NUM_COMPATIBLE_SUBROUTINES */,
					  &n);
	return n < 0 ? -1 : (int64_t)n * wsz;
}

int64_t gb_sv_sz_atomic_buf(struct gb_sth *t, int wsz, uint32_t program,
			    uint32_t buffer, uint32_t pname)
{
	GB_NATIVE(glGetActiveAtomicCounterBufferiv, void,
		  (GLuint, GLuint, GLenum, GLint *));
	GLint n = 0;

	if (pname != 0x92C6 /* ACTIVE_ATOMIC_COUNTER_INDICES */)
		return wsz;
	if (!fn_glGetActiveAtomicCounterBufferiv)
		return -1;
	fn_glGetActiveAtomicCounterBufferiv(program, buffer,
					    0x92C5 /* ACTIVE_ATOMIC_COUNTERS */,
					    &n);
	return n < 0 ? -1 : (int64_t)n * wsz;
}

int64_t gb_sv_sz_progstring(struct gb_sth *t, uint32_t target, uint32_t pname)
{
	GB_NATIVE(glGetProgramivARB, void, (GLenum, GLenum, GLint *));
	GLint n = 0;

	if (!fn_glGetProgramivARB)
		return -1;
	fn_glGetProgramivARB(target, 0x8627 /* PROGRAM_LENGTH_ARB */, &n);
	return n < 0 ? -1 : n;
}

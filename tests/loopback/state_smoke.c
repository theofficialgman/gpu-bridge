/*
 * state_smoke.c - getter element counts, error behaviour and buffer mapping
 * through the bridge. Every expectation here must also hold for the system
 * libGL (the same file is built against it as the reference), so each check
 * is something a conforming driver does.
 *
 * Most of these were found by running a conformance suite through the
 * 32-bit client: a getter that returns fewer elements than the driver
 * writes leaves the application's tail uninitialised, a call that never
 * reaches the driver loses its error, a mapping pointer that is not aligned
 * breaks code that assumes GL_MIN_MAP_BUFFER_ALIGNMENT.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("  FAIL line %d: %s  ", __LINE__, #cond);    \
			printf("" __VA_ARGS__);                            \
			printf("\n");                                      \
			g_fail++;                                          \
		}                                                          \
	} while (0)

static GLenum err(void)
{
	GLenum e = glGetError(), n = e;

	while (n != GL_NO_ERROR) /* drain */
		n = glGetError();
	return e;
}

#define EXPECT_ERR(want, what)                                             \
	do {                                                               \
		GLenum e_ = err();                                         \
		CHECK(e_ == (want), "%s: error 0x%x, wanted 0x%x", what,   \
		      e_, (unsigned)(want));                               \
	} while (0)

static int have_ext(const char *name)
{
	const char *e = (const char *)glGetString(GL_EXTENSIONS);

	return e && strstr(e, name);
}

static void test_getter_counts(void)
{
	GLuint tex, vao;
	GLuint u[16];
	GLint i[16];
	GLfloat f[16];
	GLdouble d[16];

	/* texture swizzle: four values */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	GLint sw[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
	glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
	memset(u, 0xAB, sizeof u);
	glGetTexParameterIuiv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, u);
	CHECK(u[0] == GL_RED && u[1] == GL_GREEN && u[2] == GL_BLUE &&
	      u[3] == GL_ALPHA, "GetTexParameterIuiv swizzle %x %x %x %x",
	      u[0], u[1], u[2], u[3]);
	memset(u, 0xAB, sizeof u);
	glTexParameterIuiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
			   (GLuint[4]){ 1, 2, 3, 4 });
	glGetTexParameterIuiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, u);
	CHECK(u[0] == 1 && u[1] == 2 && u[2] == 3 && u[3] == 4,
	      "border colour %u %u %u %u", u[0], u[1], u[2], u[3]);
	if (have_ext("GL_ARB_direct_state_access")) {
		memset(u, 0xAB, sizeof u);
		glGetTextureParameterIuiv(tex, GL_TEXTURE_SWIZZLE_RGBA, u);
		CHECK(u[0] == GL_RED && u[1] == GL_GREEN && u[2] == GL_BLUE &&
		      u[3] == GL_ALPHA, "DSA swizzle %x %x %x %x", u[0], u[1],
		      u[2], u[3]);
	}
	err();

	/* current generic attribute: four values for every getter flavour */
	glVertexAttrib4f(3, 1.f, 2.f, 3.f, 4.f);
	memset(f, 0xAB, sizeof f);
	glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, f);
	CHECK(f[0] == 1 && f[1] == 2 && f[2] == 3 && f[3] == 4,
	      "GetVertexAttribfv current %g %g %g %g", f[0], f[1], f[2], f[3]);
	glVertexAttribI4i(4, -1, 2, -3, 4);
	memset(i, 0xAB, sizeof i);
	glGetVertexAttribIiv(4, GL_CURRENT_VERTEX_ATTRIB, i);
	CHECK(i[0] == -1 && i[1] == 2 && i[2] == -3 && i[3] == 4,
	      "GetVertexAttribIiv current %d %d %d %d", i[0], i[1], i[2], i[3]);
	memset(u, 0xAB, sizeof u);
	glGetVertexAttribIuiv(4, GL_CURRENT_VERTEX_ATTRIB, u);
	CHECK(u[0] == 0xffffffffu && u[1] == 2 && u[3] == 4,
	      "GetVertexAttribIuiv current");
	if (have_ext("GL_ARB_vertex_attrib_64bit")) {
		glVertexAttribL4d(5, 1., 2., 3., 4.);
		for (int k = 0; k < 16; k++)
			d[k] = -7777.0;
		glGetVertexAttribLdv(5, GL_CURRENT_VERTEX_ATTRIB, d);
		CHECK(d[0] == 1 && d[1] == 2 && d[2] == 3 && d[3] == 4,
		      "GetVertexAttribLdv current %g %g %g %g", d[0], d[1],
		      d[2], d[3]);
	}
	err();

	/* tessellation defaults: 4 outer, 2 inner */
	if (have_ext("GL_ARB_tessellation_shader")) {
		glPatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL,
				   (GLfloat[4]){ 5.f, 6.f, 7.f, 8.f });
		glPatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL,
				   (GLfloat[2]){ 9.f, 10.f });
		memset(f, 0, sizeof f);
		glGetFloatv(GL_PATCH_DEFAULT_OUTER_LEVEL, f);
		CHECK(f[0] == 5 && f[1] == 6 && f[2] == 7 && f[3] == 8,
		      "PatchParameterfv outer %g %g %g %g", f[0], f[1], f[2],
		      f[3]);
		memset(f, 0, sizeof f);
		glGetFloatv(GL_PATCH_DEFAULT_INNER_LEVEL, f);
		CHECK(f[0] == 9 && f[1] == 10, "PatchParameterfv inner %g %g",
		      f[0], f[1]);
		err();
	}

	/* evaluator getters */
	glMap1f(GL_MAP1_VERTEX_3, 0.f, 1.f, 3, 4,
		(GLfloat[12]){ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 });
	memset(i, 0xAB, sizeof i);
	glGetMapiv(GL_MAP1_VERTEX_3, GL_ORDER, i);
	CHECK(i[0] == 4, "GetMapiv order %d", i[0]);
	for (int k = 0; k < 16; k++)
		f[k] = -1.f;
	glGetMapfv(GL_MAP1_VERTEX_3, GL_COEFF, f);
	CHECK(f[0] == 0 && f[11] == 11 && f[12] == -1.f,
	      "GetMapfv coeff %g %g %g", f[0], f[11], f[12]);
	for (int k = 0; k < 16; k++)
		d[k] = -1.;
	glGetMapdv(GL_MAP1_VERTEX_3, GL_DOMAIN, d);
	CHECK(d[0] == 0. && d[1] == 1. && d[2] == -1., "GetMapdv domain");
	err();

	glDeleteTextures(1, &tex);
	(void)vao;
}

/* an invalid index must leave exactly one error and write nothing */
static void test_attrib_errors(void)
{
	GLint max = 0;
	GLfloat f[4] = { -1, -1, -1, -1 };
	GLint iv[4] = { -2, -2, -2, -2 };
	GLvoid *ptr = (GLvoid *)0x1;

	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max);
	err();
	glGetVertexAttribfv(max, GL_CURRENT_VERTEX_ATTRIB, f);
	CHECK(glGetError() == GL_INVALID_VALUE, "GetVertexAttribfv error");
	CHECK(glGetError() == GL_NO_ERROR, "GetVertexAttribfv: second error");
	CHECK(f[0] == -1 && f[3] == -1, "GetVertexAttribfv wrote %g", f[0]);
	glGetVertexAttribiv(max, GL_VERTEX_ATTRIB_ARRAY_ENABLED, iv);
	CHECK(glGetError() == GL_INVALID_VALUE, "GetVertexAttribiv error");
	CHECK(glGetError() == GL_NO_ERROR, "GetVertexAttribiv: second error");
	glGetVertexAttribPointerv(max, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
	CHECK(glGetError() == GL_INVALID_VALUE, "GetVertexAttribPointerv error");
	CHECK(glGetError() == GL_NO_ERROR, "GetVertexAttribPointerv: second");
	glEnableVertexAttribArray(max);
	CHECK(glGetError() == GL_INVALID_VALUE, "Enable error");
	CHECK(glGetError() == GL_NO_ERROR, "Enable: second error");
	glVertexAttrib4f(max, 1, 2, 3, 4);
	CHECK(glGetError() == GL_INVALID_VALUE, "VertexAttrib4f error");
	CHECK(glGetError() == GL_NO_ERROR, "VertexAttrib4f: second error");
}

/* glGetTextureSubImage on a cube map: one face per z slice */
static void test_get_texture_sub_image(void)
{
	GLuint tex;
	unsigned char face[6][4 * 4 * 4], got[4 * 4 * 4];

	if (!have_ext("GL_ARB_get_texture_sub_image"))
		return;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 4, 4);
	for (int f = 0; f < 6; f++) {
		for (unsigned i = 0; i < sizeof face[f]; i++)
			face[f][i] = (unsigned char)(f * 40 + i);
		glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 4, 4,
				GL_RGBA, GL_UNSIGNED_BYTE, face[f]);
	}
	err();
	for (int f = 0; f < 6; f++) {
		memset(got, 0xEE, sizeof got);
		glGetTextureSubImage(tex, 0, 0, 0, f, 4, 4, 1, GL_RGBA,
				     GL_UNSIGNED_BYTE, sizeof got, got);
		CHECK(!memcmp(got, face[f], sizeof got),
		      "cube face %d: first bytes %02x %02x (want %02x %02x)", f,
		      got[0], got[1], face[f][0], face[f][1]);
	}
	/* a 2D slice of all six at once */
	{
		unsigned char all[6 * 4 * 4 * 4];

		memset(all, 0xEE, sizeof all);
		glGetTextureSubImage(tex, 0, 0, 0, 0, 4, 4, 6, GL_RGBA,
				     GL_UNSIGNED_BYTE, sizeof all, all);
		CHECK(!memcmp(all + 3 * 64, face[3], 64), "all faces: face 3");
	}
	EXPECT_ERR(GL_NO_ERROR, "GetTextureSubImage");
	glDeleteTextures(1, &tex);
}

/* a pixel-pack transfer with row length / skip leaves the other bytes alone */
static void test_pack_gaps(void)
{
	unsigned char buf[8 * 4 * 4];
	int bad = 0;

	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
	memset(buf, 0xEE, sizeof buf);
	glPixelStorei(GL_PACK_ROW_LENGTH, 8);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 4);
	glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	/* first four pixels of each row (rows are 8 pixels wide) untouched? */
	for (int row = 0; row < 4; row++)
		for (int i = 0; i < 16; i++)
			if (buf[row * 32 + i] != 0xEE)
				bad++;
	CHECK(bad == 0, "%d bytes outside the transfer were overwritten", bad);
	/* the transfer itself landed in the second half of each row */
	CHECK(buf[16] != 0xEE, "transfer not written");
	err();
}

/* ARB_buffer_storage persistent mappings: written data reaches the GPU at the
 * next command that uses it, GPU-written data shows up after a fence */
/* glGetTexImage honours GL_PACK_ROW_LENGTH: every pixel lands at its strided
 * place and the bytes between rows are untouched (CTS packed_pixels) */
static void test_pack_teximage(void)
{
	enum { W = 7, H = 3 };
	static const int rls[] = { 0, 10, 15, 7 };
	unsigned char src[W * H * 4], buf[16 * H * 4 + 16];
	GLuint tex;

	for (int i = 0; i < W * H * 4; i++)
		src[i] = (unsigned char)(i * 7 + 3);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, src);
	for (unsigned k = 0; k < sizeof rls / sizeof rls[0]; k++) {
		int rl = rls[k], stride = (rl ? rl : W) * 4, bad = 0, gap = 0;

		memset(buf, 0xEE, sizeof buf);
		glPixelStorei(GL_PACK_ROW_LENGTH, rl);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W * 4; x++)
				if (buf[y * stride + x] != src[y * W * 4 + x])
					bad++;
		for (int y = 0; y < H - 1; y++)
			for (int x = W * 4; x < stride; x++)
				if (buf[y * stride + x] != 0xEE)
					gap++;
		CHECK(!bad && !gap, "row length %d: %d wrong bytes, %d gap bytes touched",
		      rl, bad, gap);
	}
	EXPECT_ERR(GL_NO_ERROR, "GetTexImage pack");
	glDeleteTextures(1, &tex);
}

/* every storage-flag / access combination of KHR-GL46.buffer_storage
 * get_buffer_parameter: map, query the access flags, unmap */
static void test_storage_map_matrix(void)
{
	static const GLbitfield mapbits[] = { 0, GL_MAP_PERSISTENT_BIT,
		GL_MAP_COHERENT_BIT | GL_MAP_PERSISTENT_BIT };
	unsigned char data[32] = { 0 };
	int combos = 0, bad = 0;

	if (!have_ext("GL_ARB_buffer_storage"))
		return;
	for (int dyn = 0; dyn < 2; dyn++)
	for (int cl = 0; cl < 2; cl++)
	for (int fi = 0; fi < 3; fi++)
	for (int ai = 0; ai <= fi; ai++) {
		GLbitfield base = (dyn ? GL_DYNAMIC_STORAGE_BIT : 0) |
				  (cl ? GL_CLIENT_STORAGE_BIT : 0) | mapbits[fi];
		GLbitfield fl[5] = { base | GL_MAP_READ_BIT,
				     base | GL_MAP_WRITE_BIT,
				     base | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT,
				     base | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT,
				     base | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT };
		GLbitfield ac[5] = { mapbits[ai] | GL_MAP_READ_BIT,
				     mapbits[ai] | GL_MAP_WRITE_BIT,
				     mapbits[ai] | GL_MAP_READ_BIT,
				     mapbits[ai] | GL_MAP_WRITE_BIT,
				     mapbits[ai] | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT };
		for (int k = 0; k < 5; k++) {
			GLuint b;
			void *p;
			GLint q = -1, qf = -1;

			glGenBuffers(1, &b);
			glBindBuffer(GL_ARRAY_BUFFER, b);
			glBufferStorage(GL_ARRAY_BUFFER, 32, data, fl[k]);
			glGetBufferParameteriv(GL_ARRAY_BUFFER,
					       GL_BUFFER_STORAGE_FLAGS, &qf);
			p = glMapBufferRange(GL_ARRAY_BUFFER, 0, 32, ac[k]);
			GLenum e = err();
			combos++;
			if (!p || e != GL_NO_ERROR || qf != (GLint)fl[k]) {
				bad++;
				if (bad < 6)
					printf("  storage 0x%x (q 0x%x) access 0x%x -> %p err 0x%x\n",
					       fl[k], qf, ac[k], p, e);
			} else {
				glGetBufferParameteriv(GL_ARRAY_BUFFER,
						       GL_BUFFER_ACCESS_FLAGS, &q);
				CHECK(q == (GLint)ac[k], "access flags 0x%x, wanted 0x%x",
				      q, ac[k]);
				glUnmapBuffer(GL_ARRAY_BUFFER);
			}
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glDeleteBuffers(1, &b);
		}
	}
	CHECK(!bad, "%d of %d map combinations failed", bad, combos);
}

/* a persistent mapping keeps working after the application rebinds the
 * target it was mapped through (CTS buffer_storage map_persistent_*) */
static void test_persistent_rebind(void)
{
	GLuint b[2];
	unsigned char *p, out[32], pat[32];

	if (!have_ext("GL_ARB_buffer_storage"))
		return;
	for (int i = 0; i < 32; i++)
		pat[i] = (unsigned char)(i * 5 + 1);
	glGenBuffers(2, b);
	glBindBuffer(GL_ARRAY_BUFFER, b[0]);
	glBufferStorage(GL_ARRAY_BUFFER, 32, NULL,
			GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT |
			GL_MAP_WRITE_BIT);
	p = glMapBufferRange(GL_ARRAY_BUFFER, 0, 32,
			     GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT |
			     GL_MAP_WRITE_BIT);
	CHECK(p != NULL, "persistent map");
	glBindBuffer(GL_ARRAY_BUFFER, 0); /* the target no longer names it */
	glBindBuffer(GL_COPY_WRITE_BUFFER, b[1]);
	glBufferData(GL_COPY_WRITE_BUFFER, 32, NULL, GL_STATIC_DRAW);
	if (p)
		memcpy(p, pat, 32);
	glBindBuffer(GL_COPY_READ_BUFFER, b[0]);
	glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 32);
	memset(out, 0, sizeof out);
	glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, 32, out);
	CHECK(!memcmp(out, pat, 32), "write through the mapping reached the buffer");
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, b[0]);
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, b);
	EXPECT_ERR(GL_NO_ERROR, "persistent rebind");
}

/* mapping bits must be covered by immutable storage flags; a non-persistent
 * mapping blocks only the sub-data calls that cross it */
static void test_map_rules(void)
{
	GLuint b;
	unsigned char d[64] = { 0 };

	if (!have_ext("GL_ARB_buffer_storage"))
		return;
	glGenBuffers(1, &b);
	glBindBuffer(GL_ARRAY_BUFFER, b);
	glBufferStorage(GL_ARRAY_BUFFER, 64, d, GL_DYNAMIC_STORAGE_BIT);
	CHECK(glMapBufferRange(GL_ARRAY_BUFFER, 0, 16, GL_MAP_READ_BIT) == NULL,
	      "map READ of storage without MAP_READ");
	EXPECT_ERR(GL_INVALID_OPERATION, "map beyond storage flags");
	glDeleteBuffers(1, &b);

	glGenBuffers(1, &b);
	glBindBuffer(GL_ARRAY_BUFFER, b);
	glBufferData(GL_ARRAY_BUFFER, 64, d, GL_STATIC_DRAW);
	CHECK(glMapBufferRange(GL_ARRAY_BUFFER, 16, 16, GL_MAP_WRITE_BIT) != NULL,
	      "plain map");
	glBufferSubData(GL_ARRAY_BUFFER, 0, 16, d);
	EXPECT_ERR(GL_NO_ERROR, "sub-data before the mapped range");
	glBufferSubData(GL_ARRAY_BUFFER, 32, 16, d);
	EXPECT_ERR(GL_NO_ERROR, "sub-data after the mapped range");
	glBufferSubData(GL_ARRAY_BUFFER, 8, 16, d);
	EXPECT_ERR(GL_INVALID_OPERATION, "sub-data crossing the mapped range");
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(1, &b);
}

/* compressed upload out of a larger cuboid: the driver reads past imageSize
 * (CTS pixelstoragemodes.compressedteximage2d) */
static void test_compressed_block_unpack(void)
{
	enum { BS = 8 };               /* DXT1: 4x4 texels in 8 bytes */
	unsigned char src[16 * BS];    /* 16x16 texels = 4x4 blocks */
	unsigned char back[4 * BS];    /* 8x8 texels = 2x2 blocks */
	GLuint tex;
	GLint w = 0;
	int bad = 0;

	if (!have_ext("GL_EXT_texture_compression_s3tc") ||
	    (!have_ext("GL_ARB_compressed_texture_pixel_storage") &&
	     atof((const char *)glGetString(GL_VERSION)) < 4.2)) {
		printf("  (compressed block unpack: not supported, skipped)\n");
		return;
	}
	for (int i = 0; i < 16 * BS; i++)
		src[i] = (unsigned char)(i + 1);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_COMPRESSED_BLOCK_WIDTH, 4);
	glPixelStorei(GL_UNPACK_COMPRESSED_BLOCK_HEIGHT, 4);
	glPixelStorei(GL_UNPACK_COMPRESSED_BLOCK_SIZE, BS);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 16);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
	/* the 8x8 sub-cuboid starts at block (1,1) of a 4x4-block image */
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
			       8, 8, 0, 4 * BS, src);
	glPixelStorei(GL_UNPACK_COMPRESSED_BLOCK_WIDTH, 0);
	glPixelStorei(GL_UNPACK_COMPRESSED_BLOCK_HEIGHT, 0);
	glPixelStorei(GL_UNPACK_COMPRESSED_BLOCK_SIZE, 0);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	EXPECT_ERR(GL_NO_ERROR, "compressed upload with block state");
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
	CHECK(w == 8, "width %d", w);
	memset(back, 0, sizeof back);
	glGetCompressedTexImage(GL_TEXTURE_2D, 0, back);
	for (int by = 0; by < 2; by++)
		for (int bx = 0; bx < 2; bx++)
			if (memcmp(back + (by * 2 + bx) * BS,
				   src + ((by + 1) * 4 + bx + 1) * BS, BS))
				bad++;
	CHECK(!bad, "%d of 4 blocks differ from the source sub-cuboid", bad);
	glDeleteTextures(1, &tex);
}

static void test_persistent_map(void)
{
	GLuint b[2];
	const GLbitfield sflags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
				  GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
	unsigned char src[256], chk[256];

	if (!have_ext("GL_ARB_buffer_storage"))
		return;
	for (int i = 0; i < 256; i++)
		src[i] = (unsigned char)(i * 7 + 1);
	glGenBuffers(2, b);
	glBindBuffer(GL_COPY_READ_BUFFER, b[0]);
	glBufferStorage(GL_COPY_READ_BUFFER, 8192, NULL, sflags);
	glBindBuffer(GL_COPY_WRITE_BUFFER, b[1]);
	glBufferStorage(GL_COPY_WRITE_BUFFER, 8192, NULL, GL_DYNAMIC_STORAGE_BIT);
	err();
	/* application -> GPU */
	unsigned char *p = glMapBufferRange(GL_COPY_READ_BUFFER, 0, 8192,
					    sflags);
	CHECK(p != NULL, "persistent map");
	if (p) {
		memcpy(p + 4096, src, sizeof src);
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
				    4096, 0, sizeof src);
		memset(chk, 0, sizeof chk);
		glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof chk, chk);
		CHECK(!memcmp(chk, src, sizeof src), "write reached the buffer");
		/* GPU -> application */
		unsigned char more[256];
		for (int i = 0; i < 256; i++)
			more[i] = (unsigned char)(255 - i);
		glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof more, more);
		glCopyBufferSubData(GL_COPY_WRITE_BUFFER, GL_COPY_READ_BUFFER, 0,
				    0, sizeof more);
		GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
		glDeleteSync(f);
		CHECK(!memcmp(p, more, sizeof more), "GPU write visible: %02x %02x",
		      p[0], p[1]);
		/* the earlier application write is still there */
		CHECK(!memcmp(p + 4096, src, sizeof src), "earlier write kept");
		CHECK(glUnmapBuffer(GL_COPY_READ_BUFFER) == GL_TRUE, "unmap");
	}
	/* not created persistent: refused */
	glBindBuffer(GL_COPY_WRITE_BUFFER, b[1]);
	CHECK(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, 64,
			       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT) == NULL,
	      "non-persistent storage mapped persistently");
	EXPECT_ERR(GL_INVALID_OPERATION, "persistent map of plain storage");
	glDeleteBuffers(2, b);
}

static int g_dbg_n, g_dbg_pop;
static void GLAPIENTRY count_cb(GLenum source, GLenum type, GLuint id,
				GLenum severity, GLsizei length,
				const GLchar *msg, const void *user)
{
	(void)source; (void)id; (void)severity; (void)length; (void)msg;
	(void)user;
	g_dbg_n++;
	if (type == GL_DEBUG_TYPE_POP_GROUP)
		g_dbg_pop++;
}

/* debug groups: each push/pop is reported exactly once */
static void test_debug_groups(void)
{
	if (!have_ext("GL_KHR_debug"))
		return;
	g_dbg_n = g_dbg_pop = 0;
	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL,
			      GL_TRUE);
	glDebugMessageCallback(count_cb, NULL);
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 1, -1, "group");
	glPopDebugGroup();
	glFinish();
	CHECK(g_dbg_pop == 1, "pop-group messages: %d (all %d)", g_dbg_pop,
	      g_dbg_n);
	g_dbg_n = g_dbg_pop = 0;
	glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 7,
			     GL_DEBUG_SEVERITY_LOW, -1, "one");
	glFinish();
	CHECK(g_dbg_n == 1, "inserted message delivered %d times", g_dbg_n);
	glDebugMessageCallback(NULL, NULL);
	glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDisable(GL_DEBUG_OUTPUT);
	err();
}

/* asynchronous debug output reaches the callback although the application
 * makes no further GL call (CTS debug.async) */
static void test_debug_async(void)
{
	if (!have_ext("GL_KHR_debug"))
		return;
	g_dbg_n = g_dbg_pop = 0;
	glEnable(GL_DEBUG_OUTPUT);
	glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL,
			      GL_TRUE);
	glDebugMessageCallback(count_cb, NULL);
	glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 9,
			     GL_DEBUG_SEVERITY_LOW, -1, "async");
	for (int i = 0; i < 100 && g_dbg_n == 0; i++)
		usleep(10000); /* no GL calls while waiting */
	CHECK(g_dbg_n == 1, "async message delivered %d times", g_dbg_n);
	glDebugMessageCallback(NULL, NULL);
	glDisable(GL_DEBUG_OUTPUT);
	err();
}

static void test_string_getters(void)
{
	const char *vs =
		"#version 120\nuniform vec4 a_long_uniform_name;\n"
		"void main(){ gl_Position = a_long_uniform_name; }\n";
	GLuint sh = glCreateShader(GL_VERTEX_SHADER), p = glCreateProgram();

	glShaderSource(sh, 1, &vs, NULL);
	glCompileShader(sh);
	glAttachShader(p, sh);
	glLinkProgram(p);

	/* a buffer exactly as long as the name: a bigger bufSize must not
	 * make the library write past the terminator */
	GLint maxlen = 0;
	glGetProgramiv(p, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxlen);
	CHECK(maxlen > 0, "active uniform max length %d", maxlen);
	size_t need = strlen("a_long_uniform_name") + 1;
	char *buf = malloc(need + 16);
	memset(buf, 'Z', need + 16);
	GLsizei len = -1;
	GLint size = 0;
	GLenum type = 0;
	glGetActiveUniform(p, 0, 256, &len, &size, &type, buf);
	CHECK(len == (GLsizei)strlen("a_long_uniform_name") ||
	      len == (GLsizei)strlen("a_long_uniform_name"), "length %d", len);
	CHECK(buf[need - 1] == 0, "terminator");
	CHECK(buf[need] == 'Z', "wrote past the terminator (bufSize 256)");
	free(buf);

	/* bufSize 0: size and type are still reported */
	size = -1;
	type = 0;
	len = -1;
	glGetActiveUniform(p, 0, 0, &len, &size, &type, buf);
	CHECK(size == 1 && type == GL_FLOAT_VEC4, "bufSize 0: size %d type %x",
	      size, type);

	/* info log: same rule */
	char log[64];
	memset(log, 'Y', sizeof log);
	glGetProgramInfoLog(p, 64, &len, log);
	CHECK(log[len] == 0 && (len + 1 >= 64 || log[len + 1] == 'Y'),
	      "info log wrote past terminator");

	/* uniform getters: unknown location is an error, not garbage */
	GLfloat v[4] = { 1, 1, 1, 1 };
	glUseProgram(p);
	glGetUniformfv(p, 9999, v);
	EXPECT_ERR(GL_INVALID_OPERATION, "glGetUniformfv bogus location");
	/* more elements than the array holds is an error too */
	glUniform4fv(0, 0x7fffffff, v);
	EXPECT_ERR(GL_INVALID_OPERATION, "glUniform4fv count INT_MAX");
	glUseProgram(0);
	glDeleteProgram(p);
	glDeleteShader(sh);
}

static void test_uniform_queries(void)
{
	const char *vs =
		"#version 150\nuniform float a;\nuniform vec2 b;\n"
		"uniform mat2 c;\nuniform U { vec4 u0; float u1; } blk;\n"
		"void main(){ gl_Position = vec4(a + b.x + c[0][0] + blk.u0.x"
		" + blk.u1); }\n";
	GLuint sh = glCreateShader(GL_VERTEX_SHADER), p = glCreateProgram();
	GLuint idx[8];
	const char *names[3] = { "a", "does_not_exist", "c" };
	char name[64];
	GLsizei len;
	GLint v[8];

	glShaderSource(sh, 1, &vs, NULL);
	glCompileShader(sh);
	glAttachShader(p, sh);
	glLinkProgram(p);
	{
		GLint ok = 0;
		char lg[512] = "";

		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
			printf("  link failed: %s\n", lg);
			glGetProgramInfoLog(p, sizeof lg, NULL, lg);
			printf("  %s\n", lg);
		}
	}

	memset(idx, 0xaa, sizeof idx);
	glGetUniformIndices(p, 0, names, idx);
	CHECK(idx[0] == 0xaaaaaaaau, "count 0 wrote %x", idx[0]);
	memset(idx, 0xaa, sizeof idx);
	glGetUniformIndices(p, 3, names, idx);
	CHECK(idx[1] == GL_INVALID_INDEX, "unknown name -> %x", idx[1]);
	CHECK(idx[0] != GL_INVALID_INDEX && idx[2] != GL_INVALID_INDEX,
	      "known names -> %x %x", idx[0], idx[2]);
	err();

	/* names and indices of the block uniforms */
	GLuint blk = glGetUniformBlockIndex(p, "U");
	CHECK(blk != GL_INVALID_INDEX, "block index");
	memset(name, 'Q', sizeof name);
	len = -1;
	glGetActiveUniformBlockName(p, blk, sizeof name, &len, name);
	CHECK(len == 1 && !strcmp(name, "U"), "block name '%s' len %d", name,
	      len);
	memset(name, 'Q', sizeof name);
	len = -77;
	glGetActiveUniformBlockName(p, 99, sizeof name, &len, name);
	EXPECT_ERR(GL_INVALID_VALUE, "block name bad index");
	CHECK(len == -77 && name[0] == 'Q',
	      "outputs of a failed call were modified (len %d)", len);
	v[0] = -7;
	glGetActiveUniformBlockiv(p, 99, GL_UNIFORM_BLOCK_BINDING, v);
	EXPECT_ERR(GL_INVALID_VALUE, "blockiv bad index");
	glGetActiveUniformBlockiv(p, blk, 0x1234, v);
	EXPECT_ERR(GL_INVALID_ENUM, "blockiv bad pname");
	memset(v, 0xaa, sizeof v);
	glGetActiveUniformBlockiv(p, blk, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, v);
	CHECK(v[0] == 2, "block active uniforms %d", v[0]);
	memset(v, 0xaa, sizeof v);
	glGetActiveUniformBlockiv(p, blk, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES,
				  v);
	CHECK(v[0] != (GLint)0xaaaaaaaa && v[1] != (GLint)0xaaaaaaaa &&
	      v[2] == (GLint)0xaaaaaaaa, "block uniform indices %d %d %d", v[0],
	      v[1], v[2]);
	GLuint ui[2] = { 0, 99 };
	glGetActiveUniformsiv(p, 2, ui, GL_UNIFORM_TYPE, v);
	EXPECT_ERR(GL_INVALID_VALUE, "uniformsiv bad index");
	glGetActiveUniformsiv(p, 1, ui, 0x1234, v);
	EXPECT_ERR(GL_INVALID_ENUM, "uniformsiv bad pname");
	memset(name, 'Q', sizeof name);
	glGetActiveUniformName(p, idx[0], sizeof name, &len, name);
	CHECK(len == 1 && !strcmp(name, "a"), "uniform name '%s' %d", name, len);
	glDeleteProgram(p);
	glDeleteShader(sh);
	err();
}

static void test_buffers(void)
{
	GLuint b[2];
	GLint align = 0;

	glGetIntegerv(GL_MIN_MAP_BUFFER_ALIGNMENT, &align);
	CHECK(align >= 64, "GL_MIN_MAP_BUFFER_ALIGNMENT %d", align);
	glGenBuffers(2, b);
	glBindBuffer(GL_ARRAY_BUFFER, b[0]);
	glBufferData(GL_ARRAY_BUFFER, 8192, NULL, GL_DYNAMIC_DRAW);
	for (int off = 0; off < 130; off += 7) {
		void *p = glMapBufferRange(GL_ARRAY_BUFFER, off, 64,
					   GL_MAP_WRITE_BIT);
		CHECK(p && ((uintptr_t)p - off) % (uintptr_t)align == 0,
		      "mapping at %d: %p not aligned to %d", off, p, align);
		glUnmapBuffer(GL_ARRAY_BUFFER);
	}
	err();

	/* operations on a mapped buffer are errors */
	glBindBuffer(GL_COPY_READ_BUFFER, b[0]);
	glBindBuffer(GL_COPY_WRITE_BUFFER, b[1]);
	glBufferData(GL_COPY_WRITE_BUFFER, 8192, NULL, GL_DYNAMIC_DRAW);
	void *p = glMapBufferRange(GL_COPY_READ_BUFFER, 0, 64, GL_MAP_READ_BIT);
	CHECK(p != NULL, "map");
	glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
			    16);
	EXPECT_ERR(GL_INVALID_OPERATION, "glCopyBufferSubData from mapped");
	glUnmapBuffer(GL_COPY_READ_BUFFER);
	glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
			    16);
	EXPECT_ERR(GL_NO_ERROR, "glCopyBufferSubData after unmap");

	/* range errors: offset+length past the end is INVALID_VALUE */
	p = glMapBufferRange(GL_ARRAY_BUFFER, 8000, 1000, GL_MAP_WRITE_BIT);
	CHECK(p == NULL, "mapping past the end");
	EXPECT_ERR(GL_INVALID_VALUE, "glMapBufferRange past the end");
	p = glMapBufferRange(GL_ARRAY_BUFFER, 0, 16, 0);
	CHECK(p == NULL, "no access bit");
	EXPECT_ERR(GL_INVALID_OPERATION, "glMapBufferRange without access");

	glDeleteBuffers(2, b);
}

static void test_pointer_errors(void)
{
	/* a client-memory pointer is still validated by the driver */
	static const float junk[16];

	glDisable(GL_VERTEX_ARRAY);
	glVertexAttribPointer(0, 5, GL_FLOAT, GL_FALSE, 0, junk);
	EXPECT_ERR(GL_INVALID_VALUE, "glVertexAttribPointer size 5");
	glVertexAttribPointer(0, 3, GL_UNSIGNED_INT_2_10_10_10_REV, GL_FALSE, 0,
			      junk);
	EXPECT_ERR(GL_INVALID_OPERATION, "packed type with size 3");
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, -4, junk);
	EXPECT_ERR(GL_INVALID_VALUE, "negative stride");
	if (have_ext("GL_ARB_vertex_array_bgra")) {
		glVertexAttribPointer(0, GL_BGRA, GL_FLOAT, GL_FALSE, 0, junk);
		EXPECT_ERR(GL_INVALID_OPERATION, "BGRA with float");
		GLuint b;
		GLint sz = 0;
		glGenBuffers(1, &b);
		glBindBuffer(GL_ARRAY_BUFFER, b);
		glBufferData(GL_ARRAY_BUFFER, 64, NULL, GL_STATIC_DRAW);
		glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, 0,
				      NULL);
		glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
		CHECK(sz == GL_BGRA, "array size %d (BGRA expected)", sz);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteBuffers(1, &b);
	}
	err();
}

static void test_sync_errors(void)
{
	if (!have_ext("GL_ARB_sync"))
		return;
	GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	CHECK(s != NULL, "FenceSync");
	GLenum r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
	CHECK(r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED,
	      "ClientWaitSync %x", r);
	EXPECT_ERR(GL_NO_ERROR, "ClientWaitSync");
	glClientWaitSync(s, 0x80000000u, 0); /* invalid flag bit */
	EXPECT_ERR(GL_INVALID_VALUE, "ClientWaitSync flags");
	glDeleteSync(s);
	EXPECT_ERR(GL_NO_ERROR, "DeleteSync");
	glDeleteSync((GLsync)(uintptr_t)0x1234);
	EXPECT_ERR(GL_INVALID_VALUE, "DeleteSync with a bogus name");
	glClientWaitSync((GLsync)(uintptr_t)0x1234, 0, 0);
	EXPECT_ERR(GL_INVALID_VALUE, "ClientWaitSync with a bogus name");
}

static void test_labels(void)
{
	if (!have_ext("GL_KHR_debug"))
		return;
	GLuint b;
	char lab[32];
	GLsizei len = -1;

	glGenBuffers(1, &b);
	glBindBuffer(GL_ARRAY_BUFFER, b);
	glObjectLabel(GL_BUFFER, b, -1, "hello");
	glGetObjectLabel(GL_BUFFER, b, sizeof lab, &len, lab);
	CHECK(len == 5 && !strcmp(lab, "hello"), "label '%s' (%d)", lab, len);
	glObjectLabel(GL_BUFFER, b, 0, NULL);
	len = -1;
	lab[0] = 'x';
	glGetObjectLabel(GL_BUFFER, b, sizeof lab, &len, lab);
	CHECK(len == 0, "NULL label removes the label (length %d)", len);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(1, &b);
	err();
}

int main(void)
{
	Display *dpy = XOpenDisplay(NULL);

	if (!dpy) {
		printf("no display\n");
		return 77;
	}
	int vattr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	XVisualInfo *vi = glXChooseVisual(dpy, 0, vattr);

	if (!vi) {
		printf("no visual\n");
		return 1;
	}
	XSetWindowAttributes swa;
	swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual,
				       AllocNone);
	swa.border_pixel = 0;
	swa.event_mask = ExposureMask | StructureNotifyMask;
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 64, 64, 0,
				   vi->depth, InputOutput, vi->visual,
				   CWColormap | CWBorderPixel | CWEventMask,
				   &swa);
	XMapWindow(dpy, win);
	XSync(dpy, False);

	GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);

	if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
		printf("no context\n");
		return 1;
	}
	printf("GL_VERSION %s\n", glGetString(GL_VERSION));
	err();
	if (!have_ext("GL_ARB_uniform_buffer_object") &&
	    atof((const char *)glGetString(GL_VERSION)) < 3.1)
		printf("no UBOs\n");

	test_getter_counts();
	test_get_texture_sub_image();
	test_pack_gaps();
	test_pack_teximage();
	test_storage_map_matrix();
	test_persistent_rebind();
	test_map_rules();
	test_compressed_block_unpack();
	test_persistent_map();
	test_debug_groups();
	test_debug_async();
	test_attrib_errors();
	test_string_getters();
	test_uniform_queries();
	test_buffers();
	test_pointer_errors();
	test_sync_errors();
	test_labels();

	glXMakeCurrent(dpy, None, NULL);
	glXDestroyContext(dpy, ctx);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
	return g_fail ? 1 : 0;
}

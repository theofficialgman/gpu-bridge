/*
 * glx_smoke.c - GLX + desktop OpenGL end-to-end test. Built three ways like
 * the EGL tests: against the system libGL (reference), against the bridge
 * for the native ABI, and against the bridge built -m32.
 *
 * Covers legacy fixed-function paths (immediate mode, display lists, matrix
 * queries, client-memory vertex arrays/indices, interleaved arrays, textures
 * and pixel buffer objects) and a 3.3 core-profile context (VAO, VBO, GLSL,
 * instancing, uniforms, FBO). Everything is verified by reading pixels back.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
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

#define W 64
#define H 64

static int near_px(const unsigned char *p, int r, int g, int b)
{
	return abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3;
}

static void center(unsigned char px[4])
{
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static void clear(float r, float g, float b)
{
	glClearColor(r, g, b, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void ortho_identity(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* ------------------------------------------------------------------------ */

static void test_legacy(void)
{
	unsigned char px[4];

	printf("legacy (compat) context\n");
	glViewport(0, 0, W, H);
	ortho_identity();

	/* immediate mode */
	clear(0, 0, 0);
	glBegin(GL_TRIANGLES);
	glColor3f(1.f, 0.f, 0.f);
	glVertex2f(-1.f, -1.f);
	glVertex2f(3.f, -1.f);
	glVertex2f(-1.f, 3.f);
	glEnd();
	center(px);
	CHECK(near_px(px, 255, 0, 0), "immediate %d %d %d", px[0], px[1], px[2]);

	/* matrix query: 16 floats */
	glLoadIdentity();
	glTranslatef(1.f, 2.f, 3.f);
	GLfloat m[16] = { 0 };
	glGetFloatv(GL_MODELVIEW_MATRIX, m);
	CHECK(m[0] == 1.f && m[5] == 1.f && m[12] == 1.f && m[13] == 2.f &&
	      m[14] == 3.f && m[15] == 1.f, "modelview %g %g %g", m[12], m[13],
	      m[14]);
	glLoadIdentity();

	GLdouble md[16] = { 0 };
	glGetDoublev(GL_MODELVIEW_MATRIX, md);
	CHECK(md[0] == 1.0 && md[15] == 1.0, "doublev matrix");

	/* display list + glCallLists (size depends on n and type) */
	GLuint base = glGenLists(2);
	CHECK(base != 0, "glGenLists");
	glNewList(base, GL_COMPILE);
	glColor3f(0.f, 0.f, 1.f);
	glBegin(GL_QUADS);
	glVertex2f(-1.f, -1.f);
	glVertex2f(1.f, -1.f);
	glVertex2f(1.f, 1.f);
	glVertex2f(-1.f, 1.f);
	glEnd();
	glEndList();
	clear(0, 0, 0);
	glCallList(base);
	center(px);
	CHECK(near_px(px, 0, 0, 255), "call list %d %d %d", px[0], px[1], px[2]);
	clear(0, 0, 0);
	GLubyte lists[2] = { (GLubyte)base, (GLubyte)base };
	glListBase(0);
	glCallLists(2, GL_UNSIGNED_BYTE, lists);
	center(px);
	CHECK(near_px(px, 0, 0, 255), "call lists");
	CHECK(glIsList(base) == GL_TRUE, "glIsList");

	/* light/material getters (state-dependent element counts) */
	GLfloat pos[4] = { 1, 2, 3, 1 }, got[4] = { 0 };
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glGetLightfv(GL_LIGHT0, GL_POSITION, got);
	CHECK(got[3] == 1.f, "glGetLightfv size 4");
	GLfloat dir[3] = { 0, 0, -1 }, gd[3] = { 9, 9, 9 };
	glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
	glGetLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, gd);
	CHECK(gd[0] == 0.f && gd[1] == 0.f && gd[2] == -1.f, "spot dir size 3");

	/* client-memory arrays */
	static const GLfloat tri[] = { -1, -1, 3, -1, -1, 3 };
	static const GLfloat col[] = { 0, 1, 0, 0, 1, 0, 0, 1, 0 };
	clear(0, 0, 0);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, tri);
	glColorPointer(3, GL_FLOAT, 0, col);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	center(px);
	CHECK(near_px(px, 0, 255, 0), "client arrays %d %d %d", px[0], px[1],
	      px[2]);

	/* client indices, with an offset into the vertex data */
	static const GLfloat quad[] = { 9, 9, -1, -1, 1, -1, 1, 1, -1, 1 };
	static const GLushort idx[] = { 1, 2, 3, 1, 3, 4 };
	glDisableClientState(GL_COLOR_ARRAY);
	glColor3f(1.f, 1.f, 0.f);
	clear(0, 0, 0);
	glVertexPointer(2, GL_FLOAT, 0, quad);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
	center(px);
	CHECK(near_px(px, 255, 255, 0), "client indices %d %d %d", px[0],
	      px[1], px[2]);

	/* glDrawRangeElements + glMultiDrawArrays */
	clear(0, 0, 0);
	glColor3f(0.f, 1.f, 1.f);
	glDrawRangeElements(GL_TRIANGLES, 1, 4, 6, GL_UNSIGNED_SHORT, idx);
	center(px);
	CHECK(near_px(px, 0, 255, 255), "range elements");
	clear(0, 0, 0);
	static const GLint firsts[2] = { 0, 0 };
	static const GLsizei counts[2] = { 3, 3 };
	glVertexPointer(2, GL_FLOAT, 0, tri);
	glColor3f(1.f, 0.f, 1.f);
	glMultiDrawArrays(GL_TRIANGLES, firsts, counts, 2);
	center(px);
	CHECK(near_px(px, 255, 0, 255), "multidraw");

	/* glInterleavedArrays: C3F_V3F */
	static const GLfloat inter[] = {
		0, 1, 0, -1, -1, 0,
		0, 1, 0, 3, -1, 0,
		0, 1, 0, -1, 3, 0,
	};
	clear(0, 0, 0);
	glInterleavedArrays(GL_C3F_V3F, 0, inter);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	center(px);
	CHECK(near_px(px, 0, 255, 0), "interleaved %d %d %d", px[0], px[1],
	      px[2]);

	/* glArrayElement emulation inside Begin/End */
	clear(0, 0, 0);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, tri);
	glColor3f(1.f, 0.5f, 0.f);
	glBegin(GL_TRIANGLES);
	glArrayElement(0);
	glArrayElement(1);
	glArrayElement(2);
	glEnd();
	center(px);
	CHECK(near_px(px, 255, 128, 0), "glArrayElement %d %d %d", px[0],
	      px[1], px[2]);
	glDisableClientState(GL_VERTEX_ARRAY);

	/* glGetPointerv returns the client pointer */
	void *gp = NULL;
	glGetPointerv(GL_VERTEX_ARRAY_POINTER, &gp);
	CHECK(gp == (void *)tri, "glGetPointerv");

	/* error reporting */
	while (glGetError() != GL_NO_ERROR)
		;
	glBindTexture(0xDEAD, 0);
	CHECK(glGetError() == GL_INVALID_ENUM, "GL_INVALID_ENUM");
	CHECK(glGetError() == GL_NO_ERROR, "error cleared");
}

static void test_textures_pbo(void)
{
	unsigned char px[4];
	unsigned char texels[4 * 4 * 4];

	printf("textures and pixel buffer objects\n");
	for (int i = 0; i < 16; i++) {
		texels[i * 4 + 0] = 10;
		texels[i * 4 + 1] = 200;
		texels[i * 4 + 2] = 100;
		texels[i * 4 + 3] = 255;
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	clear(0, 0, 0);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
	center(px);
	CHECK(near_px(px, 10, 200, 100), "texture %d %d %d", px[0], px[1],
	      px[2]);

	/* glGetTexImage: length known only to the server */
	unsigned char back[4 * 4 * 4];
	memset(back, 0, sizeof(back));
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
	CHECK(!memcmp(back, texels, sizeof(back)), "glGetTexImage round trip");

	/* texture parameter queries */
	GLint filt = 0;
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filt);
	CHECK(filt == GL_NEAREST, "glGetTexParameteriv");
	GLint tw = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
	CHECK(tw == 4, "glGetTexLevelParameteriv");

	/* PBO unpack: upload from a buffer object (pointer arg is an offset) */
	GLuint pbo;
	unsigned char white[4 * 4 * 4];
	memset(white, 0x80, sizeof(white));
	glGenBuffers(1, &pbo);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(white), white,
		     GL_STATIC_DRAW);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA,
			GL_UNSIGNED_BYTE, (const void *)0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	clear(0, 0, 0);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
	center(px);
	CHECK(near_px(px, 128, 128, 128), "PBO unpack %d %d %d", px[0], px[1],
	      px[2]);

	/* PBO pack: glReadPixels into a buffer, read it back */
	GLuint rb;
	glGenBuffers(1, &rb);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, rb);
	glBufferData(GL_PIXEL_PACK_BUFFER, 4, NULL, GL_STREAM_READ);
	glReadBuffer(GL_BACK);
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
		     (void *)0);
	unsigned char fromb[4] = { 0 };
	glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, 4, fromb);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	CHECK(near_px(fromb, 128, 128, 128), "PBO pack %d %d %d", fromb[0],
	      fromb[1], fromb[2]);
	glDisable(GL_TEXTURE_2D);
}

static GLuint make_prog(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	const char *src[2] = { vs, fs };
	GLenum ty[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(ty[i]);
		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		GLint ok = 0;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[512];
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			printf("  shader error: %s\n", log);
			g_fail++;
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "off");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	CHECK(ok, "link");
	return p;
}

static void test_core(void)
{
	unsigned char px[4];

	printf("core profile 3.3\n");
	const char *ver = (const char *)glGetString(GL_VERSION);
	CHECK(ver != NULL, "GL_VERSION");

	/* extension enumeration must be coherent in core profiles */
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	CHECK(n > 0, "GL_NUM_EXTENSIONS %d", n);
	int bad = 0;
	for (GLint i = 0; i < n; i++)
		if (!glGetStringi(GL_EXTENSIONS, i))
			bad++;
	CHECK(bad == 0, "%d NULL glGetStringi entries of %d", bad, n);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	const char *vs = "#version 330 core\n"
			 "layout(location=0) in vec2 pos;\n"
			 "layout(location=1) in vec2 off;\n"
			 "void main(){ gl_Position = vec4(pos*0.4 + off, 0, 1); }";
	const char *fs = "#version 330 core\n"
			 "uniform vec4 color; out vec4 o;\n"
			 "void main(){ o = color; }";
	GLuint prog = make_prog(vs, fs);
	glUseProgram(prog);
	GLint loc = glGetUniformLocation(prog, "color");
	CHECK(loc >= 0, "uniform location");
	glUniform4f(loc, 0.f, 1.f, 0.f, 1.f);
	GLfloat uv[4] = { 0 };
	glGetUniformfv(prog, loc, uv);
	CHECK(uv[1] == 1.f && uv[3] == 1.f, "glGetUniformfv %g %g", uv[1],
	      uv[3]);

	static const GLfloat quad[] = { -1, -1, 1, -1, 1, 1, -1, 1 };
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glVertexAttrib2f(1, 0.f, 0.f);

	clear(0, 0, 0);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	center(px);
	CHECK(near_px(px, 0, 255, 0), "core draw %d %d %d", px[0], px[1],
	      px[2]);

	/* instancing with a per-instance attribute */
	static const GLfloat offs[] = { -0.5f, 0.f, 0.5f, 0.f };
	GLuint ovbo;
	glGenBuffers(1, &ovbo);
	glBindBuffer(GL_ARRAY_BUFFER, ovbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(offs), offs, GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glVertexAttribDivisor(1, 1);
	glUniform4f(loc, 1.f, 0.f, 0.f, 1.f);
	clear(0, 0, 0);
	glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, 2);
	/* instances centered at x = -0.5 and +0.5, each 0.8 wide */
	unsigned char l[4], r[4];
	glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
	glReadPixels(3 * W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
	CHECK(near_px(l, 255, 0, 0) && near_px(r, 255, 0, 0),
	      "instanced %d,%d,%d | %d,%d,%d", l[0], l[1], l[2], r[0], r[1],
	      r[2]);
	glVertexAttribDivisor(1, 0);
	glDisableVertexAttribArray(1);

	/* client-memory arrays are illegal in a core profile: the bridge must
	 * reject them exactly like the driver does */
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	static const GLfloat big[] = { -1, -1, 3, -1, -1, 3 };
	while (glGetError() != GL_NO_ERROR)
		;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, big);
	CHECK(glGetError() == GL_INVALID_OPERATION,
	      "core: client array pointer must raise INVALID_OPERATION");
	while (glGetError() != GL_NO_ERROR)
		;
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

	/* FBO render-to-texture and readback */
	GLuint fbo, tex;
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	      GL_FRAMEBUFFER_COMPLETE, "FBO complete");
	glViewport(0, 0, 16, 16);
	glClearColor(0.25f, 0.5f, 0.75f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	unsigned char f[4];
	glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, f);
	CHECK(near_px(f, 64, 128, 191), "FBO %d %d %d", f[0], f[1], f[2]);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, W, H);

	/* ---- mapped buffers (client-side shadow copy) ---- */
	{
		GLuint mb;
		unsigned char init[64], back[64];
		for (int i = 0; i < 64; i++)
			init[i] = (unsigned char)(i * 3 + 1);
		glGenBuffers(1, &mb);
		glBindBuffer(GL_COPY_WRITE_BUFFER, mb);
		glBufferData(GL_COPY_WRITE_BUFFER, sizeof(init), init,
			     GL_DYNAMIC_DRAW);

		/* read mapping sees the server's data */
		unsigned char *p = glMapBuffer(GL_COPY_WRITE_BUFFER,
					       GL_READ_ONLY);
		CHECK(p != NULL, "glMapBuffer READ_ONLY");
		if (p) {
			CHECK(!memcmp(p, init, 64), "mapped read contents");
			GLint mapped = 0, access = 0;
			glGetBufferParameteriv(GL_COPY_WRITE_BUFFER,
					       GL_BUFFER_MAPPED, &mapped);
			glGetBufferParameteriv(GL_COPY_WRITE_BUFFER,
					       GL_BUFFER_ACCESS, &access);
			CHECK(mapped == 1 && access == GL_READ_ONLY,
			      "BUFFER_MAPPED %d ACCESS 0x%x", mapped, access);
			void *ptr = NULL;
			glGetBufferPointerv(GL_COPY_WRITE_BUFFER,
					    GL_BUFFER_MAP_POINTER, &ptr);
			CHECK(ptr == p, "GL_BUFFER_MAP_POINTER");
			/* mapping twice is an error */
			CHECK(glMapBuffer(GL_COPY_WRITE_BUFFER,
					  GL_READ_ONLY) == NULL &&
			      glGetError() == GL_INVALID_OPERATION,
			      "double map");
			CHECK(glUnmapBuffer(GL_COPY_WRITE_BUFFER) == GL_TRUE,
			      "unmap");
		}
		GLint mapped = 1;
		glGetBufferParameteriv(GL_COPY_WRITE_BUFFER, GL_BUFFER_MAPPED,
				       &mapped);
		CHECK(mapped == 0, "unmapped state");

		/* WRITE_ONLY keeps the bytes the app does not touch */
		p = glMapBuffer(GL_COPY_WRITE_BUFFER, GL_WRITE_ONLY);
		CHECK(p != NULL, "glMapBuffer WRITE_ONLY");
		if (p) {
			p[0] = 200;
			p[63] = 201;
			glUnmapBuffer(GL_COPY_WRITE_BUFFER);
		}
		glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, 64, back);
		CHECK(back[0] == 200 && back[63] == 201 && back[10] == init[10] &&
		      back[40] == init[40], "partial write keeps the rest "
		      "(%d %d %d %d)", back[0], back[63], back[10], back[40]);

		/* range mapping with explicit flush: only flushed bytes land */
		p = glMapBufferRange(GL_COPY_WRITE_BUFFER, 16, 32,
				     GL_MAP_WRITE_BIT |
				     GL_MAP_FLUSH_EXPLICIT_BIT);
		CHECK(p != NULL, "glMapBufferRange explicit flush");
		if (p) {
			memset(p, 0x77, 32);
			glFlushMappedBufferRange(GL_COPY_WRITE_BUFFER, 8, 8);
			glUnmapBuffer(GL_COPY_WRITE_BUFFER);
		}
		glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, 64, back);
		/* only the flushed bytes (map offset 8..15 = buffer 24..31)
		 * are defined to land; the rest is undefined by the spec */
		CHECK(back[24] == 0x77 && back[31] == 0x77,
		      "flushed bytes landed: %02x %02x", back[24], back[31]);

		/* invalidate-range write replaces the whole mapped range */
		p = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, 64,
				     GL_MAP_WRITE_BIT |
				     GL_MAP_INVALIDATE_BUFFER_BIT);
		if (p) {
			memset(p, 0x11, 64);
			glUnmapBuffer(GL_COPY_WRITE_BUFFER);
		}
		glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, 64, back);
		CHECK(back[0] == 0x11 && back[63] == 0x11, "invalidate write");

		/* mapped data feeds rendering: update a vertex buffer by map */
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		GLfloat *vp = glMapBufferRange(GL_ARRAY_BUFFER, 0,
					       sizeof(quad),
					       GL_MAP_WRITE_BIT |
					       GL_MAP_INVALIDATE_RANGE_BIT);
		CHECK(vp != NULL, "map vertex buffer");
		if (vp) {
			memcpy(vp, quad, sizeof(quad));
			glUnmapBuffer(GL_ARRAY_BUFFER);
		}
		glUniform4f(loc, 1.f, 0.f, 0.f, 1.f);
		clear(0, 0, 0);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		center(px);
		CHECK(near_px(px, 255, 0, 0), "draw from mapped-updated VBO "
		      "%d %d %d", px[0], px[1], px[2]);

		/* persistent mappings are refused, not silently stale */
		while (glGetError() != GL_NO_ERROR)
			;
		p = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, 64,
				     GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
		CHECK(p == NULL && glGetError() == GL_INVALID_OPERATION,
		      "persistent mapping refused");
		glDeleteBuffers(1, &mb);
	}

	/* glGetIntegeri_v: indexed query (3.1+) */
	GLint ub = -1;
	glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &ub);
	CHECK(ub == 0, "glGetIntegeri_v %d", ub);
	CHECK(glGetError() == GL_NO_ERROR, "no stray errors");
}

/* ------------------------------------------------------------------------ */

int main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		printf("SKIP: no X display\n");
		return 77;
	}
	printf("client pointer size %zu, display %s\n", sizeof(void *),
	       DisplayString(dpy));

	int maj = 0, min = 0, eb, evb;
	CHECK(glXQueryExtension(dpy, &eb, &evb), "glXQueryExtension");
	CHECK(glXQueryVersion(dpy, &maj, &min) && (maj > 1 || min >= 3),
	      "GLX version %d.%d", maj, min);
	const char *gx = glXQueryExtensionsString(dpy, 0);
	CHECK(gx && strstr(gx, "GLX_ARB_create_context"), "extensions string");
	CHECK(glXGetClientString(dpy, GLX_VENDOR) != NULL, "client string");

	static int vattr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16,
			       None };
	XVisualInfo *vi = glXChooseVisual(dpy, 0, vattr);
	CHECK(vi != NULL, "glXChooseVisual");
	if (!vi)
		return 1;

	XSetWindowAttributes swa = { 0 };
	swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual,
				       AllocNone);
	swa.event_mask = StructureNotifyMask;
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, W, H, 0,
				   vi->depth, InputOutput, vi->visual,
				   CWColormap | CWEventMask, &swa);
	XMapWindow(dpy, win);
	XSync(dpy, False);

	GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
	CHECK(ctx != NULL, "glXCreateContext");
	CHECK(glXMakeCurrent(dpy, win, ctx), "glXMakeCurrent");
	CHECK(glXGetCurrentContext() == ctx, "glXGetCurrentContext");
	CHECK(glXGetCurrentDrawable() == win, "glXGetCurrentDrawable");
	CHECK(glXGetCurrentDisplay() == dpy, "glXGetCurrentDisplay");

	printf("GL_RENDERER: %s\nGL_VERSION: %s\n",
	       (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION));

	int v = 0;
	CHECK(glXGetConfig(dpy, vi, GLX_DOUBLEBUFFER, &v) == 0 && v == 1,
	      "glXGetConfig");

	test_legacy();
	test_textures_pbo();

	/* present the window and read it back through the X server */
	clear(1.f, 0.f, 1.f);
	glXSwapBuffers(dpy, win);
	XSync(dpy, False);

	/* core-profile context on the same window */
	typedef GLXContext (*create_attribs_fn)(Display *, GLXFBConfig,
						GLXContext, Bool, const int *);
	create_attribs_fn create_attribs = (create_attribs_fn)
		glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
	CHECK(create_attribs != NULL, "glXCreateContextAttribsARB");
	if (create_attribs) {
		static const int fb_attr[] = {
			GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE,
			GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
			GLX_DOUBLEBUFFER, True, GLX_DEPTH_SIZE, 16, None
		};
		int nfb = 0;
		GLXFBConfig *fbs = glXChooseFBConfig(dpy, 0, fb_attr, &nfb);
		CHECK(fbs && nfb > 0, "glXChooseFBConfig n=%d", nfb);
		XVisualInfo *fvi = fbs ? glXGetVisualFromFBConfig(dpy, fbs[0])
				       : NULL;
		CHECK(fvi != NULL, "glXGetVisualFromFBConfig");
		/* the window's visual must match the FBConfig's */
		GLXFBConfig pick = NULL;
		for (int i = 0; i < nfb && !pick; i++) {
			XVisualInfo *t = glXGetVisualFromFBConfig(dpy, fbs[i]);
			if (t && t->visualid == vi->visualid)
				pick = fbs[i];
			if (t)
				XFree(t);
		}
		CHECK(pick != NULL, "FBConfig for the window's visual");
		if (pick) {
			static const int ca[] = {
				GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
				GLX_CONTEXT_MINOR_VERSION_ARB, 3,
				GLX_CONTEXT_PROFILE_MASK_ARB,
				GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None
			};
			GLXContext core = create_attribs(dpy, pick, NULL, True,
							 ca);
			CHECK(core != NULL, "core context");
			if (core && glXMakeCurrent(dpy, win, core)) {
				test_core();
				glXMakeCurrent(dpy, None, NULL);
				glXDestroyContext(dpy, core);
			}
		}
		if (fvi)
			XFree(fvi);
		if (fbs)
			XFree(fbs);
	}

	glXMakeCurrent(dpy, None, NULL);
	glXDestroyContext(dpy, ctx);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);

	printf("%s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}

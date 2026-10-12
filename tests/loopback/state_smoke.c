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
	glGetActiveUniformBlockName(p, 99, sizeof name, &len, name);
	EXPECT_ERR(GL_INVALID_VALUE, "block name bad index");
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

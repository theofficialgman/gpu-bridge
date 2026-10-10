/*
 * gles2_smoke.c - end-to-end EGL/GLES2 test. The same source is linked three
 * ways: against the system driver (reference), against the bridge client for
 * the native ABI, and against the bridge client built -m32.
 *
 * Renders offscreen (pbuffer) and verifies pixels, so it needs no window.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int near_px(const unsigned char *p, int r, int g, int b, int a)
{
	return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 &&
	       abs(p[2] - b) <= 2 && abs(p[3] - a) <= 2;
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		GLsizei n = 0;
		glGetShaderInfoLog(s, sizeof(log), &n, log);
		printf("  shader compile failed: %.*s\n", (int)n, log);
		g_fail++;
	}
	return s;
}

static GLuint link_program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "uv");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	CHECK(ok, "program link");
	return p;
}

static void read_center(int w, int h, unsigned char out[4])
{
	glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

int main(void)
{
	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;

	CHECK(dpy != EGL_NO_DISPLAY, "eglGetDisplay");
	CHECK(eglInitialize(dpy, &major, &minor) == EGL_TRUE, "eglInitialize");
	printf("EGL %d.%d  vendor: %s\n", major, minor,
	       eglQueryString(dpy, EGL_VENDOR));
	CHECK(major >= 1, "EGL version");
	const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
	CHECK(ext != NULL, "EGL_EXTENSIONS");

	CHECK(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE, "eglBindAPI");
	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_NONE
	};
	EGLConfig cfg;
	EGLint ncfg = 0;
	CHECK(eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) && ncfg == 1,
	      "eglChooseConfig ncfg=%d", ncfg);

	enum { W = 64, H = 64 };
	const EGLint pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	CHECK(surf != EGL_NO_SURFACE, "pbuffer");
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
					   EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");
	CHECK(eglMakeCurrent(dpy, surf, surf, ctx) == EGL_TRUE, "makeCurrent");
	CHECK(eglGetCurrentContext() == ctx, "eglGetCurrentContext");

	const char *ver = (const char *)glGetString(GL_VERSION);
	printf("GL_VERSION: %s\nGL_RENDERER: %s\n", ver ? ver : "(null)",
	       (const char *)glGetString(GL_RENDERER));
	CHECK(ver && strstr(ver, "OpenGL ES"), "GL_VERSION");

	/* --- 1. clear + readback ------------------------------------------ */
	unsigned char px[4];
	glClearColor(1.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	read_center(W, H, px);
	CHECK(near_px(px, 255, 0, 0, 255), "clear red: %d %d %d %d", px[0],
	      px[1], px[2], px[3]);

	/* --- 2. glGetIntegerv sized by pname ------------------------------- */
	GLint vp[4] = { -1, -1, -1, -1 };
	glViewport(0, 0, W, H);
	glGetIntegerv(GL_VIEWPORT, vp);
	CHECK(vp[0] == 0 && vp[1] == 0 && vp[2] == W && vp[3] == H,
	      "viewport %d %d %d %d", vp[0], vp[1], vp[2], vp[3]);

	/* --- 3. shaders + client-memory vertex array + uniform ------------- */
	const char *vs = "attribute vec2 pos; attribute vec2 uv;"
			 "varying vec2 v_uv;"
			 "void main(){ gl_Position = vec4(pos, 0.0, 1.0);"
			 " v_uv = uv; }";
	const char *fs_color = "precision mediump float; uniform vec4 color;"
			       "void main(){ gl_FragColor = color; }";
	GLuint prog = link_program(vs, fs_color);
	glUseProgram(prog);
	GLint loc = glGetUniformLocation(prog, "color");
	CHECK(loc >= 0, "uniform location");
	glUniform4f(loc, 0.f, 1.f, 0.f, 1.f);

	static const GLfloat tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri); /* client */
	glDrawArrays(GL_TRIANGLES, 0, 3);
	read_center(W, H, px);
	CHECK(near_px(px, 0, 255, 0, 255), "client-array triangle: %d %d %d %d",
	      px[0], px[1], px[2], px[3]);

	/* --- 4. client-memory indices (glDrawElements) --------------------- */
	static const GLfloat quad[] = { -1.f, -1.f, 1.f, -1.f,
					1.f, 1.f, -1.f, 1.f };
	static const GLushort idx[] = { 0, 1, 2, 0, 2, 3 };
	glUniform4f(loc, 0.f, 0.f, 1.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
	read_center(W, H, px);
	CHECK(near_px(px, 0, 0, 255, 255), "client-index quad: %d %d %d %d",
	      px[0], px[1], px[2], px[3]);

	/* --- 5. interleaved client array with stride ------------------------ */
	static const GLfloat inter[] = {
		/* pos        uv (unused) */
		-1.f, -1.f,   0.f, 0.f,
		 3.f, -1.f,   1.f, 0.f,
		-1.f,  3.f,   0.f, 1.f,
	};
	glUniform4f(loc, 1.f, 1.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      inter);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	read_center(W, H, px);
	CHECK(near_px(px, 255, 255, 0, 255), "strided array: %d %d %d %d",
	      px[0], px[1], px[2], px[3]);

	/* --- 6. VBO + IBO (buffer-offset paths) ----------------------------- */
	GLuint bufs[2];
	glGenBuffers(2, bufs);
	CHECK(bufs[0] && bufs[1] && bufs[0] != bufs[1], "glGenBuffers");
	glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
	glUniform4f(loc, 1.f, 0.f, 1.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
	read_center(W, H, px);
	CHECK(near_px(px, 255, 0, 255, 255), "VBO/IBO quad: %d %d %d %d",
	      px[0], px[1], px[2], px[3]);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	/* --- 7. texture upload from client memory -------------------------- */
	const char *fs_tex = "precision mediump float; uniform sampler2D tex;"
			     "varying vec2 v_uv;"
			     "void main(){ gl_FragColor = texture2D(tex, v_uv); }";
	GLuint tprog = link_program(vs, fs_tex);
	glUseProgram(tprog);
	unsigned char texels[4 * 4 * 4];
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
	static const GLfloat uvs[] = { 0.f, 0.f, 2.f, 0.f, 0.f, 2.f };
	glUniform1i(glGetUniformLocation(tprog, "tex"), 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uvs);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	read_center(W, H, px);
	CHECK(near_px(px, 10, 200, 100, 255), "texture: %d %d %d %d", px[0],
	      px[1], px[2], px[3]);
	glDisableVertexAttribArray(1);

	/* --- 8. error reporting -------------------------------------------- */
	while (glGetError() != GL_NO_ERROR)
		;
	glBindTexture(0xDEAD, 0); /* invalid target */
	GLenum e = glGetError();
	CHECK(e == GL_INVALID_ENUM, "glGetError = 0x%x", e);
	CHECK(glGetError() == GL_NO_ERROR, "error cleared");

	/* --- 9. large readback: chunked reply path -------------------------- */
	const EGLint big_attr[] = { EGL_WIDTH, 1024, EGL_HEIGHT, 1024,
				    EGL_NONE };
	EGLSurface big = eglCreatePbufferSurface(dpy, cfg, big_attr);
	CHECK(big != EGL_NO_SURFACE, "big pbuffer");
	CHECK(eglMakeCurrent(dpy, big, big, ctx) == EGL_TRUE, "makeCurrent big");
	glViewport(0, 0, 1024, 1024);
	glClearColor(0.2f, 0.4f, 0.6f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	size_t nbytes = 1024u * 1024u * 4u;
	unsigned char *img = malloc(nbytes);
	memset(img, 0x55, nbytes);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, 1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, img);
	int bad = 0;
	for (size_t i = 0; i < nbytes; i += 4)
		if (!near_px(img + i, 51, 102, 153, 255))
			bad++;
	CHECK(bad == 0, "%d of %u pixels wrong in 4MB readback", bad,
	      1024u * 1024u);
	free(img);

	/* --- 10. extension filtering ---------------------------------------- */
	const char *gext = (const char *)glGetString(GL_EXTENSIONS);
	CHECK(gext != NULL, "GL_EXTENSIONS");
	printf("GL_EXTENSIONS (filtered): %zu bytes\n",
	       gext ? strlen(gext) : 0);
	CHECK(eglGetProcAddress("glDrawArrays") != NULL, "proc glDrawArrays");
#ifdef GB_BRIDGE
	/* The bridge must not hand out entry points it does not implement
	 * (a driver may legally return non-NULL for anything). */
	CHECK(eglGetProcAddress("glNotARealFunction") == NULL, "proc unknown");
	CHECK(eglGetProcAddress("glGetUniformfv") == NULL,
	      "unsupported entry point must be hidden");
#endif

	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, big);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);

	printf("%s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}

/* SPDX-License-Identifier: MIT */
/*
 * pname_audit.c - empirical check of the pname -> element-count tables.
 *
 * For every enum 0..0xFFFF this calls glGetDoublev / glGetDoublei_v on the
 * *native* driver with a sentinel-filled buffer, counts how many elements the
 * driver wrote, and compares with gb_pname_count_class() (the table the
 * bridge sizes reply buffers with). A table value larger than what the driver
 * writes makes the client over-write an application buffer; a smaller one
 * truncates. Run it against each native driver (dev Radeon, Tegra NVIDIA).
 *
 *   gcc pname_audit.c -I../../src/proto -I../../build/gen -lEGL -lGL -o audit
 *   EGL_PLATFORM=surfaceless ./audit
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gb_pname_classes.h"
#include "gb_gl_sizes.h"

#define NMAX 4096
#define SENT (-1.2345e300)

static sigjmp_buf jb;
static void on_segv(int sig)
{
	(void)sig;
	siglongjmp(jb, 1);
}

static int geti_fn(void *c, uint32_t p)
{
	GLint v = 0;
	(void)c;
	glGetIntegerv(p, &v);
	while (glGetError())
		;
	return v;
}

static int written(const double *b)
{
	int n = 0;
	for (int i = 0; i < NMAX; i++)
		if (b[i] != SENT)
			n = i + 1;
	return n;
}

int main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint a, b, n;
	EGLConfig c;
	EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
			EGL_OPENGL_BIT, EGL_NONE };
	EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };

	eglInitialize(d, &a, &b);
	eglBindAPI(EGL_OPENGL_API);
	eglChooseConfig(d, ca, &c, 1, &n);
	EGLSurface s = eglCreatePbufferSurface(d, c, pa);
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, NULL);
	eglMakeCurrent(d, s, s, x);
	printf("# renderer: %s\n", glGetString(GL_RENDERER));

	int bad = 0, checked = 0, crashed = 0;
	signal(SIGSEGV, on_segv); /* some drivers crash on odd enums: skip them */
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (uint32_t p = 0; p < 0x10000; p++) {
		static double buf[NMAX];

		if (sigsetjmp(jb, 1)) {
			crashed++;
			continue;
		}

		while (glGetError())
			;
		for (int i = 0; i < NMAX; i++)
			buf[i] = SENT;
		glGetDoublev(p, buf);
		if (glGetError())
			goto indexed; /* not a valid non-indexed query */
		int w = written(buf);
		int t = gb_pname_count_class(GB_PNC_getv, p, geti_fn, NULL);
		checked++;
		if (w)
			printf("M getv 0x%04X %d\n", p, w);
		if (w && w != t) {
			printf("getv   pname 0x%04X: driver wrote %d, table says %d%s\n",
			       p, w, t, t < w ? "  <-- TRUNCATES" : "  (over-sized)");
			bad++;
		}
indexed:
		while (glGetError())
			;
		for (int i = 0; i < NMAX; i++)
			buf[i] = SENT;
		glGetDoublei_v(p, 0, buf);
		if (glGetError())
			continue;
		w = written(buf);
		t = gb_pname_count_class(GB_PNC_geti_v, p, geti_fn, NULL);
		checked++;
		if (w)
			printf("M geti_v 0x%04X %d\n", p, w);
		if (w && w != t) {
			printf("geti_v pname 0x%04X: driver wrote %d, table says %d%s\n",
			       p, w, t, t < w ? "  <-- TRUNCATES" : "  (over-sized)");
			bad++;
		}
	}
	/* the small pname classes: queried with a representative object */
	static const struct {
		const char *name;
		int cls;
		void (*get)(GLenum, GLenum, GLfloat *);
		GLenum target;
	} cl[] = {
		{ "light", GB_PNC_light, glGetLightfv, GL_LIGHT0 },
		{ "material", GB_PNC_material, glGetMaterialfv, GL_FRONT },
		{ "texenv", GB_PNC_texenv, glGetTexEnvfv, GL_TEXTURE_ENV },
		{ "texgen", GB_PNC_texgen, glGetTexGenfv, GL_S },
		{ "texparam", GB_PNC_texparam, glGetTexParameterfv, GL_TEXTURE_2D },
	};
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	for (unsigned k = 0; k < sizeof(cl) / sizeof(cl[0]); k++) {
		for (uint32_t p = 0; p < 0x10000; p++) {
			static GLfloat fb[NMAX];

			if (sigsetjmp(jb, 1))
				continue;
			while (glGetError())
				;
			for (int i = 0; i < NMAX; i++)
				fb[i] = -1.2345e30f;
			cl[k].get(cl[k].target, p, fb);
			if (glGetError())
				continue;
			int w = 0;
			for (int i = 0; i < NMAX; i++)
				if (fb[i] != -1.2345e30f)
					w = i + 1;
			int t = gb_pname_count_class(cl[k].cls, p, geti_fn, NULL);
			checked++;
			if (w && w != t) {
				printf("%-7s pname 0x%04X: driver wrote %d, table says %d%s\n",
				       cl[k].name, p, w, t,
				       t < w ? "  <-- TRUNCATES" : "  (over-sized)");
				bad++;
			}
		}
	}
	printf("# checked %d valid queries, %d mismatches, %d enums crashed the driver\n", checked, bad, crashed);
	return bad != 0;
}

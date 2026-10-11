/*
 * ext_compare.c - print every advertised extension / version of each API the
 * bridge serves, one per line, so a native run and a bridged run can be
 * diffed:
 *
 *   gcc ext_compare.c -o ext_compare -lEGL -lGL -lGLESv2 -lGLESv1_CM
 *   EGL_PLATFORM=surfaceless ./ext_compare > native.txt
 *   LD_LIBRARY_PATH=build/shim64 GB_SERVER=build/gbd EGL_PLATFORM=surfaceless \
 *       ./ext_compare > bridged.txt ; diff native.txt bridged.txt
 *
 * Contexts: desktop compat, desktop core 3.3 and 4.6, GLES 1.1/2.0/3.0/3.1/3.2
 * (those the driver gives us), plus the EGL client and display strings.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void words(const char *tag, const char *s)
{
	if (!s)
		return;
	char *c = strdup(s), *save = NULL;
	for (char *w = strtok_r(c, " ", &save); w; w = strtok_r(NULL, " ", &save))
		printf("%s %s\n", tag, w);
	free(c);
}

static void one(EGLDisplay d, const char *tag, EGLenum api, EGLint renderable,
		const EGLint *ctx_attribs)
{
	EGLConfig c;
	EGLint n = 0;
	EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
			renderable, EGL_NONE };

	if (!eglBindAPI(api) || !eglChooseConfig(d, ca, &c, 1, &n) || !n) {
		printf("%s unavailable\n", tag);
		return;
	}
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, ctx_attribs);
	if (x == EGL_NO_CONTEXT) {
		printf("%s no-context\n", tag);
		return;
	}
	EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	EGLSurface s = eglCreatePbufferSurface(d, c, pa);
	if (!eglMakeCurrent(d, s, s, x)) {
		printf("%s make-current-failed\n", tag);
		return;
	}
	printf("%s version %s\n", tag, glGetString(GL_VERSION));
	const char *e = (const char *)glGetString(GL_EXTENSIONS);
	if (e) {
		words(tag, e);
	} else { /* core profile: glGetStringi */
		GLint cnt = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &cnt);
		for (GLint i = 0; i < cnt; i++)
			printf("%s %s\n", tag, glGetStringi(GL_EXTENSIONS, i));
	}
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(d, s);
	eglDestroyContext(d, x);
}

int main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint a, b;

	words("EGL-client", eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS));
	if (!eglInitialize(d, &a, &b))
		return 1;
	printf("EGL version %s\n", eglQueryString(d, EGL_VERSION));
	words("EGL-display", eglQueryString(d, EGL_EXTENSIONS));

	one(d, "GL-compat", EGL_OPENGL_API, EGL_OPENGL_BIT, NULL);
	{
		EGLint core33[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
				    EGL_CONTEXT_MINOR_VERSION, 3,
				    EGL_CONTEXT_OPENGL_PROFILE_MASK,
				    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
		one(d, "GL-core33", EGL_OPENGL_API, EGL_OPENGL_BIT, core33);
		EGLint core46[] = { EGL_CONTEXT_MAJOR_VERSION, 4,
				    EGL_CONTEXT_MINOR_VERSION, 6,
				    EGL_CONTEXT_OPENGL_PROFILE_MASK,
				    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
		one(d, "GL-core46", EGL_OPENGL_API, EGL_OPENGL_BIT, core46);
	}
	{
		EGLint e1[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
		one(d, "ES1", EGL_OPENGL_ES_API, EGL_OPENGL_ES_BIT, e1);
		EGLint e2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		one(d, "ES2", EGL_OPENGL_ES_API, EGL_OPENGL_ES2_BIT, e2);
		for (int minor = 0; minor <= 2; minor++) {
			char tag[16];
			EGLint e3[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
					EGL_CONTEXT_MINOR_VERSION, minor,
					EGL_NONE };
			snprintf(tag, sizeof(tag), "ES3.%d", minor);
			one(d, tag, EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, e3);
		}
	}
	return 0;
}

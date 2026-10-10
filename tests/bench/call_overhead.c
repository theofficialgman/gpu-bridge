/*
 * call_overhead.c - micro-benchmark of API-call cost, linked three ways like
 * gles2_smoke.c (native driver / bridge 64-bit / bridge 32-bit).
 *
 * Reports best-of-3 per case. These are *per-call* costs on trivial GPU
 * work: they show the transport overhead, not application frame rates.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define BEST_OF 3
#define RUN(name, n, unit, body)                                          \
	do {                                                              \
		double best = 1e30;                                       \
		for (int r = 0; r < BEST_OF; r++) {                       \
			double t0 = now();                                \
			for (long i = 0; i < (n); i++) { body; }          \
			glFinish();                                       \
			double dt = now() - t0;                           \
			if (dt < best) best = dt;                         \
		}                                                         \
		printf("%-34s %9.1f k%s/s  (%6.0f ns each)\n", name,      \
		       (n) / best / 1e3, unit, best / (n) * 1e9);         \
	} while (0)

int main(void)
{
	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint M, m;
	eglInitialize(dpy, &M, &m);
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
				     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
				     EGL_RED_SIZE, 8, EGL_NONE };
	EGLConfig cfg;
	EGLint n;
	eglChooseConfig(dpy, ca, &cfg, 1, &n);
	static const EGLint pb[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
	EGLSurface s = eglCreatePbufferSurface(dpy, cfg, pb);
	static const EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext c = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cx);
	eglMakeCurrent(dpy, s, s, c);

	const char *vsrc = "attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}";
	const char *fsrc = "precision mediump float; uniform vec4 c;"
			   "void main(){gl_FragColor=c;}";
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vsrc, NULL);
	glCompileShader(vs);
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fsrc, NULL);
	glCompileShader(fs);
	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glUseProgram(p);
	GLint loc = glGetUniformLocation(p, "c");
	glViewport(0, 0, 256, 256);

	static const GLfloat tri[] = { -1, -1, 3, -1, -1, 3 };
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

	printf("sizeof(void*)=%zu  renderer: %s\n", sizeof(void *),
	       (const char *)glGetString(GL_RENDERER));

	RUN("async: glUniform4f", 2000000L, "calls", glUniform4f(loc, 1, 0, 0, 1));
	RUN("sync : glGetError round-trip", 100000L, "calls", (void)glGetError());
	RUN("draw : glDrawArrays (VBO, 3v)", 200000L, "draws",
	    glDrawArrays(GL_TRIANGLES, 0, 3));

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	RUN("draw : glDrawArrays (client array)", 100000L, "draws",
	    glDrawArrays(GL_TRIANGLES, 0, 3));

	static unsigned char img[256 * 256 * 4];
	{
		double best = 1e30;
		for (int r = 0; r < BEST_OF; r++) {
			double t0 = now();
			for (int i = 0; i < 400; i++)
				glReadPixels(0, 0, 256, 256, GL_RGBA,
					     GL_UNSIGNED_BYTE, img);
			double dt = now() - t0;
			if (dt < best) best = dt;
		}
		printf("%-34s %9.1f MB/s    (%6.0f us each)\n",
		       "sync : glReadPixels 256x256", 400.0 * sizeof(img) /
		       best / 1e6, best / 400 * 1e6);
	}
	return 0;
}

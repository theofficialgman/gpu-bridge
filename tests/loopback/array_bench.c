/* SPDX-License-Identifier: MIT */
/*
 * array_bench.c - cost per draw call with vertex data in client memory (what
 * Irrlicht/minetest, glmark2 "build" and many fixed-function games do). Prints
 * us per draw; run against the system libGL (reference) and through the bridge.
 * Not a correctness test.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct vtx { /* an Irrlicht S3DVertex: interleaved, 36 bytes */
	float pos[3], nrm[3];
	unsigned char col[4];
	float uv[2];
};

static int only; /* argv[2]: run just this pattern (1-4), 0 = all */
static int pat;

#define RUN(name, ...)                                                       \
	if (++pat, !only || only == pat) do {                                \
		double t0 = now();                                           \
		long iters = 0;                                              \
		while (now() - t0 < secs) {                                  \
			for (int k = 0; k < 1000; k++) { __VA_ARGS__; iters++; }    \
			glFinish();                                          \
		}                                                            \
		double dt = now() - t0;                                      \
		printf("%-46s %7.2f us/draw  %8.0f draws/s\n", name,         \
		       dt / iters * 1e6, iters / dt);                        \
		fflush(stdout);                                              \
	} while (0)

static struct vtx *mesh(int n)
{
	struct vtx *v = calloc(n, sizeof *v);

	for (int i = 0; i < n; i++) {
		v[i].pos[0] = (i % 50) * 0.01f - 0.25f;
		v[i].pos[1] = (i / 50) * 0.01f - 0.25f;
		v[i].nrm[2] = 1;
		v[i].col[0] = v[i].col[3] = 255;
	}
	return v;
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 3.0;
	only = argc > 2 ? atoi(argv[2]) : 0;
	Display *dpy = XOpenDisplay(NULL);

	if (!dpy)
		return 77;
	int vattr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	XVisualInfo *vi = glXChooseVisual(dpy, 0, vattr);
	XSetWindowAttributes swa;

	swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual,
				       AllocNone);
	swa.border_pixel = 0;
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 64, 64, 0,
				   vi->depth, InputOutput, vi->visual,
				   CWColormap | CWBorderPixel, &swa);
	XMapWindow(dpy, win);
	XSync(dpy, False);
	GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);

	glXMakeCurrent(dpy, win, ctx);
	printf("renderer: %s\n", glGetString(GL_RENDERER));
	glViewport(0, 0, 64, 64);
	glEnable(GL_CULL_FACE);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_NORMAL_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	enum { BIG = 600, SMALL = 30 };
	struct vtx *vb = mesh(BIG), *vs = mesh(SMALL);
	unsigned short ib[BIG * 3], is[SMALL * 3];

	for (int i = 0; i < BIG * 3; i++)
		ib[i] = (unsigned short)(i / 3 + (i % 3));
	for (int i = 0; i < SMALL * 3; i++)
		is[i] = (unsigned short)((i / 3 + (i % 3)) % SMALL);

#define ILV(v) do {                                                           \
	glVertexPointer(3, GL_FLOAT, sizeof(struct vtx), (v)->pos);           \
	glNormalPointer(GL_FLOAT, sizeof(struct vtx), (v)->nrm);              \
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), (v)->col);    \
	glTexCoordPointer(2, GL_FLOAT, sizeof(struct vtx), (v)->uv);          \
} while (0)
	ILV(vb);
	RUN("interleaved 4 attr, 600 vtx, 1800 idx",
	    glDrawElements(GL_TRIANGLES, BIG * 3, GL_UNSIGNED_SHORT, ib));
	ILV(vs);
	RUN("interleaved 4 attr, 30 vtx, 90 idx",
	    glDrawElements(GL_TRIANGLES, SMALL * 3, GL_UNSIGNED_SHORT, is));
	ILV(vs);
	RUN("interleaved, same mesh, vertex+color only",
	    glDisableClientState(GL_NORMAL_ARRAY);
	    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	    glDrawElements(GL_TRIANGLES, SMALL * 3, GL_UNSIGNED_SHORT, is);
	    glEnableClientState(GL_NORMAL_ARRAY);
	    glEnableClientState(GL_TEXTURE_COORD_ARRAY));

	/* separate arrays, no indices (glmark2 "build" without VBOs) */
	float *p = malloc(BIG * 12), *n = malloc(BIG * 12), *t = malloc(BIG * 8);

	memset(p, 0, BIG * 12); memset(n, 0, BIG * 12); memset(t, 0, BIG * 8);
	glDisableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, p);
	glNormalPointer(GL_FLOAT, 0, n);
	glTexCoordPointer(2, GL_FLOAT, 0, t);
	RUN("separate 3 attr, glDrawArrays 600",
	    glDrawArrays(GL_TRIANGLES, 0, BIG));
	glXMakeCurrent(dpy, None, NULL);
	return 0;
}

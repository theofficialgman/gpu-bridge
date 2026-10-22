/* SPDX-License-Identifier: MIT */
/*
 * sync_mix.c - synchronous calls of every kind in quick succession, with
 * short random sleeps so that both sides keep going to sleep and waking
 * (glGetError takes the mailbox, the others the ordinary reply path): the
 * answers must stay right and the stream in step. Not a conformance test; it
 * exists because a stale wake-up record once made the next ordinary reply look
 * out of sync.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL line %d: %s ", __LINE__, #c); printf("" __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 60000;
	Display *dpy = XOpenDisplay(NULL);

	if (!dpy)
		return 77;
	int vattr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
	XVisualInfo *vi = glXChooseVisual(dpy, 0, vattr);
	XSetWindowAttributes swa;

	swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual, AllocNone);
	swa.border_pixel = 0;
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 64, 64, 0, vi->depth,
				   InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XMapWindow(dpy, win);
	XSync(dpy, False);
	GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);

	glXMakeCurrent(dpy, win, ctx);
	GLint maxtex = 0, maxtex2 = 0;

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
	CHECK(maxtex >= 1024, "max texture size %d", maxtex);
	unsigned seed = 5;

	for (long i = 0; i < iters; i++) {
		seed = seed * 1664525u + 1013904223u;
		switch ((seed >> 12) % 6) {
		case 0:
			CHECK(glGetError() == GL_NO_ERROR, "no error expected");
			break;
		case 1:
			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex2);
			CHECK(maxtex2 == maxtex, "max texture size %d", maxtex2);
			break;
		case 2:
			glEnable(0x1234); /* invalid enum: the next glGetError says so, once */
			CHECK(glGetError() == GL_INVALID_ENUM, "invalid enum");
			CHECK(glGetError() == GL_NO_ERROR, "consumed");
			break;
		case 3:
			CHECK(glGetString(GL_VERSION) != NULL, "version string");
			break;
		case 4:
			CHECK(glIsEnabled(GL_DEPTH_TEST) == GL_FALSE, "depth test off");
			CHECK(glGetError() == GL_NO_ERROR, "after glIsEnabled");
			break;
		default:
			glClear(GL_COLOR_BUFFER_BIT);
			glFlush();
			CHECK(glGetError() == GL_NO_ERROR, "after clear");
			break;
		}
		if (!((seed >> 20) & 63))
			usleep((seed >> 8) % 400); /* both sides drift into sleep */
		if (g_fail > 20)
			break;
	}
	glXMakeCurrent(dpy, None, NULL);
	printf(g_fail ? "FAILED (%d failures)\n" : "PASSED (0 failures)\n", g_fail);
	return g_fail != 0;
}

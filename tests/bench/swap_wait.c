/* SPDX-License-Identifier: MIT */
/*
 * swap_wait.c - CPU used by a vsynced glXSwapBuffers loop with and without an
 * explicit swap interval call. swap_wait <mode> <seconds>:
 *   none  : leave the driver default
 *   ext   : glXSwapIntervalEXT(1)
 *   sgi   : glXSwapIntervalSGI(1)
 * Prints frames, fps and the process CPU time.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(clockid_t c)
{
	struct timespec t;

	clock_gettime(c, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "none";
	double secs = argc > 2 ? atof(argv[2]) : 10;
	Display *dpy = XOpenDisplay(NULL);
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
	XVisualInfo *vi = glXChooseVisual(dpy, 0, attr);
	XSetWindowAttributes swa;

	swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual, AllocNone);
	swa.border_pixel = 0;
	Window w = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 300, 200, 0, vi->depth,
				 InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XMapWindow(dpy, w);
	GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);

	glXMakeCurrent(dpy, w, ctx);
	if (!strcmp(mode, "ext")) {
		PFNGLXSWAPINTERVALEXTPROC f = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");

		if (f)
			f(dpy, w, 1);
		else
			printf("no glXSwapIntervalEXT\n");
	} else if (!strcmp(mode, "sgi")) {
		PFNGLXSWAPINTERVALSGIPROC f = (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddress((const GLubyte *)"glXSwapIntervalSGI");

		if (f)
			f(1);
		else
			printf("no glXSwapIntervalSGI\n");
	}
	double t0 = now(CLOCK_MONOTONIC), c0 = now(CLOCK_PROCESS_CPUTIME_ID);
	long n = 0;

	while (now(CLOCK_MONOTONIC) - t0 < secs) {
		glClearColor((n & 1) * 0.5f, 0.2f, 0.2f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(dpy, w);
		n++;
	}
	double dt = now(CLOCK_MONOTONIC) - t0, dc = now(CLOCK_PROCESS_CPUTIME_ID) - c0;

	printf("%-5s %ld frames, %.1f fps, cpu %.1f%%\n", mode, n, n / dt, 100 * dc / dt);
	return 0;
}

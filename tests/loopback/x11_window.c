/*
 * x11_window.c - Strategy X test: the app owns an X11 window (its own Xlib
 * connection) and renders to it through EGL. Under the bridge the render
 * server draws into that window over a *second* X connection, using only the
 * window's XID. Verified by reading the window back from the X server.
 *
 * Built natively (reference) and bridged (64- and 32-bit client).
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
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

static unsigned long pixel_at(Display *d, Window w, int x, int y)
{
	XImage *im = XGetImage(d, w, x, y, 1, 1, AllPlanes, ZPixmap);
	if (!im)
		return 0xdeadbeef;
	unsigned long p = XGetPixel(im, 0, 0);
	XDestroyImage(im);
	return p;
}

static void render(float r, float g, float b, int w, int h)
{
	glViewport(0, 0, w, h);
	glClearColor(r, g, b, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
}

int main(void)
{
	Display *xd = XOpenDisplay(NULL);
	if (!xd) {
		printf("SKIP: no X display\n");
		return 77;
	}
	printf("X display: %s, client pointer size %zu\n", DisplayString(xd),
	       sizeof(void *));

	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xd);
	EGLint maj, min;
	CHECK(dpy != EGL_NO_DISPLAY, "eglGetDisplay(Display*)");
	CHECK(eglInitialize(dpy, &maj, &min), "eglInitialize");
	eglBindAPI(EGL_OPENGL_ES_API);

	static const EGLint attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint n = 0;
	CHECK(eglChooseConfig(dpy, attr, &cfg, 1, &n) && n == 1, "choose cfg");

	EGLint vid = 0;
	CHECK(eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid),
	      "native visual id");
	XVisualInfo tmpl = { .visualid = (VisualID)vid }, *vi;
	int nvi = 0;
	vi = XGetVisualInfo(xd, VisualIDMask, &tmpl, &nvi);
	CHECK(vi && nvi >= 1, "XGetVisualInfo(0x%x)", vid);
	if (!vi)
		return 1;

	XSetWindowAttributes swa = { 0 };
	swa.colormap = XCreateColormap(xd, DefaultRootWindow(xd), vi->visual,
				       AllocNone);
	swa.border_pixel = 0;
	swa.event_mask = StructureNotifyMask | ExposureMask;
	Window win = XCreateWindow(xd, DefaultRootWindow(xd), 0, 0, 160, 120, 0,
				   vi->depth, InputOutput, vi->visual,
				   CWColormap | CWBorderPixel | CWEventMask,
				   &swa);
	XMapWindow(xd, win);
	XSync(xd, False);
	printf("window XID 0x%lx\n", (unsigned long)win);

	EGLSurface surf = eglCreateWindowSurface(dpy, cfg,
						 (EGLNativeWindowType)win, NULL);
	CHECK(surf != EGL_NO_SURFACE, "eglCreateWindowSurface (err 0x%x)",
	      eglGetError());
	static const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
	CHECK(ctx != EGL_NO_CONTEXT, "context");
	CHECK(eglMakeCurrent(dpy, surf, surf, ctx), "makeCurrent");

	EGLint w = 0, h = 0;
	eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
	CHECK(w == 160 && h == 120, "surface size %dx%d", w, h);

	/* Frame 1: red. Present, then read the window back from the X server. */
	render(1.f, 0.f, 0.f, 160, 120);
	CHECK(eglSwapBuffers(dpy, surf), "swap 1");
	XSync(xd, False);
	usleep(100 * 1000);
	unsigned long px = pixel_at(xd, win, 80, 60);
	printf("frame 1 pixel: 0x%06lx (want 0xff0000)\n", px & 0xffffff);
	CHECK((px & 0xffffff) == 0xff0000, "frame 1 pixel 0x%lx", px);

	/* Frame 2: blue, proves repeated presentation. */
	render(0.f, 0.f, 1.f, 160, 120);
	CHECK(eglSwapBuffers(dpy, surf), "swap 2");
	XSync(xd, False);
	usleep(100 * 1000);
	px = pixel_at(xd, win, 10, 10);
	printf("frame 2 pixel: 0x%06lx (want 0x0000ff)\n", px & 0xffffff);
	CHECK((px & 0xffffff) == 0x0000ff, "frame 2 pixel 0x%lx", px);

	/* The *client* resizes its own window; the surface must follow. */
	XResizeWindow(xd, win, 200, 150);
	XSync(xd, False);
	usleep(100 * 1000);
	render(0.f, 1.f, 0.f, 200, 150);
	eglSwapBuffers(dpy, surf);
	eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
	printf("after resize: surface %dx%d\n", w, h);
	CHECK(w == 200 && h == 150, "resized surface %dx%d", w, h);
	XSync(xd, False);
	usleep(100 * 1000);
	px = pixel_at(xd, win, 190, 140);
	CHECK((px & 0xffffff) == 0x00ff00, "frame 3 pixel 0x%lx", px);

	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	XDestroyWindow(xd, win);
	XCloseDisplay(xd);

	printf("%s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}

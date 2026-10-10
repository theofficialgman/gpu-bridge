/*
 * gb_server_x11.c - the server's own Xlib access.
 *
 * The server never links libX11: it dlopens it. It opens its *own*
 * connection(s) to the X server by name; window/pixmap XIDs created by the
 * client's connection are valid on these because XIDs are global to the X
 * server (ARCHITECTURE.md, Strategy X).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "gb_server_impl.h"

static void *g_x11;
static Display *(*p_XOpenDisplay)(const char *);
static XVisualInfo *(*p_XGetVisualInfo)(Display *, long, XVisualInfo *,
					int *);
static int (*p_XFree)(void *);

struct xconn {
	struct xconn *next;
	char *name;
	Display *dpy;
};
static struct xconn *g_conns;
static pthread_mutex_t g_xlock = PTHREAD_MUTEX_INITIALIZER;

/* An unhandled X error terminates the whole process by default (Xlib's
 * fatal handler). A client handing us a bad XID must not kill the server. */
static int x_error(Display *d, XErrorEvent *e)
{
	gb_log("X error: type %d request %d.%d resource 0x%lx code %d",
	       e->type, e->request_code, e->minor_code, e->resourceid,
	       e->error_code);
	return 0;
}

void gb_x_init(void)
{
	int (*p_XInitThreads)(void);
	void *(*p_XSetErrorHandler)(void *);

	if (g_x11)
		return;
	g_x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
	if (!g_x11)
		return;
	/* Must precede every other Xlib call: worker threads share displays. */
	p_XInitThreads = (int (*)(void))dlsym(g_x11, "XInitThreads");
	if (p_XInitThreads)
		p_XInitThreads();
	p_XSetErrorHandler =
		(void *(*)(void *))dlsym(g_x11, "XSetErrorHandler");
	if (p_XSetErrorHandler)
		p_XSetErrorHandler((void *)x_error);
	p_XOpenDisplay = (Display *(*)(const char *))dlsym(g_x11,
							   "XOpenDisplay");
	p_XGetVisualInfo = (XVisualInfo *(*)(Display *, long, XVisualInfo *,
					     int *))dlsym(g_x11, "XGetVisualInfo");
	p_XFree = (int (*)(void *))dlsym(g_x11, "XFree");
}

Display *gb_x_open(const char *name)
{
	Display *d = NULL;

	pthread_mutex_lock(&g_xlock);
	for (struct xconn *c = g_conns; c; c = c->next)
		if (!strcmp(c->name, name)) {
			d = c->dpy;
			break;
		}
	if (!d && p_XOpenDisplay) {
		d = p_XOpenDisplay(name);
		if (d) {
			struct xconn *c = calloc(1, sizeof(*c));
			c->name = strdup(name);
			c->dpy = d;
			c->next = g_conns;
			g_conns = c;
		}
	}
	pthread_mutex_unlock(&g_xlock);
	return d;
}

XVisualInfo *gb_x_visual(Display *dpy, uint32_t visualid, uint32_t screen)
{
	XVisualInfo tmpl, *vi;
	int n = 0;

	if (!p_XGetVisualInfo)
		return NULL;
	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.visualid = visualid;
	vi = p_XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
	(void)screen; /* visual ids are unique per X server */
	return n > 0 ? vi : NULL;
}

void gb_x_free(void *p)
{
	if (p && p_XFree)
		p_XFree(p);
}

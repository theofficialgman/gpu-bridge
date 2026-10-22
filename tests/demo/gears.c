/* SPDX-License-Identifier: MIT */
/*
 * gears.c - visual demo for the GLX / desktop-GL bridge: three meshing
 * gears using legacy fixed-function OpenGL (display lists, lighting, matrix
 * stack, immediate mode). Built 32-bit and linked against the bridge's
 * libGL.so.1 replacement it is an arm32/i386 GL application with no GPU
 * driver of its own. The window title shows pointer size, renderer and fps.
 *
 * Usage: gears [seconds]   (Esc or window close quits)
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
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

/* One gear as a display list: front/back faces, teeth, outer and bore. */
static GLuint make_gear(float inner, float outer, float width, int teeth,
			float depth, const GLfloat color[4])
{
	GLuint list = glGenLists(1);
	const float r0 = inner, r1 = outer - depth / 2, r2 = outer + depth / 2;
	const float da = 2.f * (float)M_PI / teeth / 4.f;
	const float hw = width * 0.5f;

	glNewList(list, GL_COMPILE);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, color);
	glShadeModel(GL_FLAT);

	for (int side = 0; side < 2; side++) {
		float z = side ? -hw : hw;
		glNormal3f(0.f, 0.f, side ? -1.f : 1.f);
		glBegin(GL_QUAD_STRIP);
		for (int i = 0; i <= teeth; i++) {
			float a = i * 2.f * (float)M_PI / teeth;
			glVertex3f(r0 * cosf(a), r0 * sinf(a), z);
			glVertex3f(r1 * cosf(a), r1 * sinf(a), z);
			if (i < teeth) {
				glVertex3f(r0 * cosf(a), r0 * sinf(a), z);
				glVertex3f(r1 * cosf(a + 3 * da),
					   r1 * sinf(a + 3 * da), z);
			}
		}
		glEnd();
		glBegin(GL_QUADS);
		for (int i = 0; i < teeth; i++) {
			float a = i * 2.f * (float)M_PI / teeth;
			glVertex3f(r1 * cosf(a), r1 * sinf(a), z);
			glVertex3f(r2 * cosf(a + da), r2 * sinf(a + da), z);
			glVertex3f(r2 * cosf(a + 2 * da),
				   r2 * sinf(a + 2 * da), z);
			glVertex3f(r1 * cosf(a + 3 * da),
				   r1 * sinf(a + 3 * da), z);
		}
		glEnd();
	}

	/* outward faces of the teeth */
	glBegin(GL_QUAD_STRIP);
	for (int i = 0; i < teeth; i++) {
		float a = i * 2.f * (float)M_PI / teeth;
		float u, v, len;
		glVertex3f(r1 * cosf(a), r1 * sinf(a), hw);
		glVertex3f(r1 * cosf(a), r1 * sinf(a), -hw);
		u = r2 * cosf(a + da) - r1 * cosf(a);
		v = r2 * sinf(a + da) - r1 * sinf(a);
		len = sqrtf(u * u + v * v);
		glNormal3f(v / len, -u / len, 0.f);
		glVertex3f(r2 * cosf(a + da), r2 * sinf(a + da), hw);
		glVertex3f(r2 * cosf(a + da), r2 * sinf(a + da), -hw);
		glNormal3f(cosf(a), sinf(a), 0.f);
		glVertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), hw);
		glVertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -hw);
		u = r1 * cosf(a + 3 * da) - r2 * cosf(a + 2 * da);
		v = r1 * sinf(a + 3 * da) - r2 * sinf(a + 2 * da);
		glNormal3f(v, -u, 0.f);
		glVertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), hw);
		glVertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -hw);
		glNormal3f(cosf(a), sinf(a), 0.f);
	}
	glVertex3f(r1, 0.f, hw);
	glVertex3f(r1, 0.f, -hw);
	glEnd();

	/* bore */
	glShadeModel(GL_SMOOTH);
	glBegin(GL_QUAD_STRIP);
	for (int i = 0; i <= teeth; i++) {
		float a = i * 2.f * (float)M_PI / teeth;
		glNormal3f(-cosf(a), -sinf(a), 0.f);
		glVertex3f(r0 * cosf(a), r0 * sinf(a), -hw);
		glVertex3f(r0 * cosf(a), r0 * sinf(a), hw);
	}
	glEnd();
	glEndList();
	return list;
}

int main(int argc, char **argv)
{
	double run_for = argc > 1 ? atof(argv[1]) : 0.0;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot open X display\n");
		return 1;
	}
	static int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16,
			      None };
	XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr);
	if (!vi) {
		fprintf(stderr, "no suitable visual\n");
		return 1;
	}
	XSetWindowAttributes swa = { 0 };
	swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual,
				       AllocNone);
	swa.event_mask = StructureNotifyMask | KeyPressMask | ExposureMask;
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), 120, 120, 460,
				   460, 0, vi->depth, InputOutput, vi->visual,
				   CWColormap | CWEventMask, &swa);
	Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &del, 1);
	XMapWindow(dpy, win);

	GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
	if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
		fprintf(stderr, "cannot create/make current a GL context\n");
		return 1;
	}
	typedef void (*swap_int_fn)(Display *, GLXDrawable, int);
	swap_int_fn swap_interval = (swap_int_fn)glXGetProcAddressARB(
		(const GLubyte *)"glXSwapIntervalEXT");
	if (swap_interval)
		swap_interval(dpy, win, 1);

	const char *rend = (const char *)glGetString(GL_RENDERER);
	char shortr[64];
	snprintf(shortr, sizeof(shortr), "%.60s", rend ? rend : "?");
	char *par = strchr(shortr, '(');
	if (par && par > shortr)
		par[-1] = 0;
	printf("%zu-bit process; GL_RENDERER: %s\nGL_VERSION: %s\n",
	       sizeof(void *) * 8, rend ? rend : "?",
	       (const char *)glGetString(GL_VERSION));
	fflush(stdout);

	static const GLfloat pos[4] = { 5.f, 5.f, 10.f, 0.f };
	static const GLfloat red[4] = { 0.8f, 0.1f, 0.f, 1.f };
	static const GLfloat green[4] = { 0.f, 0.8f, 0.2f, 1.f };
	static const GLfloat blue[4] = { 0.2f, 0.2f, 1.f, 1.f };
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_NORMALIZE);
	GLuint g1 = make_gear(1.f, 4.f, 1.f, 20, 0.7f, red);
	GLuint g2 = make_gear(0.5f, 2.f, 2.f, 10, 0.7f, green);
	GLuint g3 = make_gear(1.3f, 2.f, 0.5f, 10, 0.7f, blue);

	int w = 460, h = 460, running = 1;
	double t0 = now(), tfps = t0;
	long frames = 0, total = 0;
	while (running) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == ConfigureNotify) {
				w = ev.xconfigure.width;
				h = ev.xconfigure.height;
			} else if (ev.type == ClientMessage &&
				   (Atom)ev.xclient.data.l[0] == del) {
				running = 0;
			} else if (ev.type == KeyPress &&
				   XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
				running = 0;
			}
		}
		double t = now() - t0;
		if (run_for > 0 && t > run_for)
			break;
		float angle = (float)(t * 70.0);

		glViewport(0, 0, w, h);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		float ar = (float)h / (float)w;
		glFrustum(-1.0, 1.0, -ar, ar, 5.0, 60.0);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glTranslatef(0.f, 0.f, -40.f);
		glClearColor(0.05f, 0.06f, 0.1f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glPushMatrix();
		glRotatef(20.f, 1.f, 0.f, 0.f);
		glRotatef(30.f, 0.f, 1.f, 0.f);
		glPushMatrix();
		glTranslatef(-3.f, -2.f, 0.f);
		glRotatef(angle, 0.f, 0.f, 1.f);
		glCallList(g1);
		glPopMatrix();
		glPushMatrix();
		glTranslatef(3.1f, -2.f, 0.f);
		glRotatef(-2.f * angle - 9.f, 0.f, 0.f, 1.f);
		glCallList(g2);
		glPopMatrix();
		glPushMatrix();
		glTranslatef(-3.1f, 4.2f, 0.f);
		glRotatef(-2.f * angle - 25.f, 0.f, 0.f, 1.f);
		glCallList(g3);
		glPopMatrix();
		glPopMatrix();
		glXSwapBuffers(dpy, win);

		frames++;
		total++;
		double tn = now();
		if (tn - tfps >= 1.0) {
			char title[200];
			snprintf(title, sizeof(title),
				 "%zu-bit gears | %s | %.0f fps",
				 sizeof(void *) * 8, shortr,
				 frames / (tn - tfps));
			XStoreName(dpy, win, title);
			printf("%s\n", title);
			fflush(stdout);
			frames = 0;
			tfps = tn;
		}
	}
	printf("done: %ld frames\n", total);
	glXMakeCurrent(dpy, None, NULL);
	glXDestroyContext(dpy, ctx);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}

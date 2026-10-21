/*
 * call_bench.c - cost per GL call for the call patterns of legacy
 * applications (immediate mode, matrix stack, state changes). Prints ns per
 * call and Mcalls/s for each pattern; run against the system libGL (reference)
 * and through the bridge. Not a correctness test.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int only; /* argv[2]: run just this pattern (1-based), 0 = all */
static int pat;

#define RUN(name, calls_per_iter, ...)                                    \
	if (++pat, !only || only == pat) do {                                \
		double t0 = now();                                           \
		long iters = 0;                                              \
		while (now() - t0 < secs) {                                  \
			for (int k = 0; k < 20; k++) { __VA_ARGS__; iters++; }      \
			glFinish();                                          \
		}                                                            \
		double dt = now() - t0;                                      \
		double calls = (double)iters * (calls_per_iter);             \
		printf("%-34s %7.1f ns/call  %6.2f Mcalls/s\n", name,        \
		       dt / calls * 1e9, calls / dt / 1e6);                  \
		fflush(stdout);                                              \
	} while (0)

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
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);

	/* immediate mode: normal + vertex per vertex, 3000 vertices per block */
	RUN("glNormal3f+glVertex3f (Begin/End)", 6002,
	    glBegin(GL_TRIANGLES);
	    for (int i = 0; i < 3000; i++) {
		    glNormal3f(0, 0, 1);
		    glVertex3f(i * 1e-4f, 0.5f, 0);
	    }
	    glEnd());
	RUN("glVertex3f only (Begin/End)", 3002,
	    glBegin(GL_TRIANGLES);
	    for (int i = 0; i < 3000; i++)
		    glVertex3f(i * 1e-4f, 0.5f, 0);
	    glEnd());
	RUN("glColor4ub+glVertex3d+glTexCoord2d", 9002,
	    glBegin(GL_QUADS);
	    for (int i = 0; i < 3000; i++) {
		    glColor4ub(255, 0, 0, 255);
		    glTexCoord2d(0.5, 0.5);
		    glVertex3d(i * 1e-4, 0.5, 0);
	    }
	    glEnd());
	/* matrix stack / state: no vertices */
	RUN("glPush/Rotate/Translate/PopMatrix", 4000,
	    for (int i = 0; i < 1000; i++) {
		    glPushMatrix();
		    glRotatef(1.f, 0, 1, 0);
		    glTranslatef(0.1f, 0, 0);
		    glPopMatrix();
	    });
	RUN("glEnable/glDisable pair", 2000,
	    for (int i = 0; i < 1000; i++) {
		    glEnable(GL_CULL_FACE);
		    glDisable(GL_CULL_FACE);
	    });
	RUN("glMaterialfv", 1000,
	    for (int i = 0; i < 1000; i++) {
		    static const float m[4] = { 0.5f, 0.5f, 0.5f, 1 };
		    glMaterialfv(GL_FRONT, GL_DIFFUSE, m);
	    });
	/* a 2D sprite batch as LinCity-NG draws it: state, bind, colour and one
	 * small Begin/End block per sprite */
	{
		GLuint tex;

		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		RUN("sprite: enable+bind+color+quad (calls/iter)", 14,
		    for (int i = 0; i < 1000; i++) {
			    glEnable(GL_TEXTURE_2D);
			    glBindTexture(GL_TEXTURE_2D, tex);
			    glColor4ub(255, 255, 255, 255);
			    glBegin(GL_QUADS);
			    glTexCoord2f(0, 0); glVertex2f(-1, -1);
			    glTexCoord2f(1, 0); glVertex2f(1, -1);
			    glTexCoord2f(1, 1); glVertex2f(1, 1);
			    glTexCoord2f(0, 1); glVertex2f(-1, 1);
			    glEnd();
			    glDisable(GL_TEXTURE_2D);
		    });
		RUN("sprite: glBegin/glEnd only (4 vertices)", 6,
		    for (int i = 0; i < 1000; i++) {
			    glBegin(GL_QUADS);
			    glVertex2f(-1, -1);
			    glVertex2f(1, -1);
			    glVertex2f(1, 1);
			    glVertex2f(-1, 1);
			    glEnd();
		    });
		RUN("glEnable/glDisable(GL_TEXTURE_2D)", 2000,
		    for (int i = 0; i < 1000; i++) {
			    glEnable(GL_TEXTURE_2D);
			    glDisable(GL_TEXTURE_2D);
		    });
	}
	/* synchronous calls: a reply is awaited each time */
	RUN("glGetError (nothing queued)", 1000,
	    for (int i = 0; i < 1000; i++)
		    (void)glGetError());
	RUN("glEnable + glGetError", 2000,
	    for (int i = 0; i < 1000; i++) {
		    glEnable(GL_CULL_FACE);
		    (void)glGetError();
	    });
	RUN("glBindTexture + glGetError", 2000,
	    for (int i = 0; i < 1000; i++) {
		    glBindTexture(GL_TEXTURE_2D, 0);
		    (void)glGetError();
	    });
	glXMakeCurrent(dpy, None, NULL);
	return 0;
}

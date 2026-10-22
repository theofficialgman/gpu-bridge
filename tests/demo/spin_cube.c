/* SPDX-License-Identifier: MIT */
/*
 * spin_cube.c - visual demo: a spinning, depth-tested, per-face-coloured cube
 * in an X11 window, rendered with EGL + GLES2. Linked against the bridge it
 * is a 32-bit process whose GPU work is executed by the 64-bit gbd.
 *
 * Usage: spin_cube [seconds]   (default: runs until closed; Esc / window
 * close button quits). The window title shows pointer size, renderer, fps.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* 4x4 column-major helpers */
static void mat_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void mat_mul(float *out, const float *a, const float *b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++) {
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += a[k * 4 + rr] * b[c * 4 + k];
			r[c * 4 + rr] = s;
		}
	memcpy(out, r, sizeof(r));
}

static void mat_perspective(float *m, float fovy, float aspect, float n, float f)
{
	float t = 1.f / tanf(fovy * 0.5f);
	memset(m, 0, 16 * sizeof(float));
	m[0] = t / aspect;
	m[5] = t;
	m[10] = (f + n) / (n - f);
	m[11] = -1.f;
	m[14] = 2.f * f * n / (n - f);
}

static void mat_rot_y(float *m, float a)
{
	mat_identity(m);
	m[0] = cosf(a); m[8] = sinf(a);
	m[2] = -sinf(a); m[10] = cosf(a);
}

static void mat_rot_x(float *m, float a)
{
	mat_identity(m);
	m[5] = cosf(a); m[9] = -sinf(a);
	m[6] = sinf(a); m[10] = cosf(a);
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		GLsizei n = 0;
		glGetShaderInfoLog(s, sizeof(log), &n, log);
		fprintf(stderr, "shader error: %.*s\n", (int)n, log);
		exit(1);
	}
	return s;
}

int main(int argc, char **argv)
{
	double run_for = argc > 1 ? atof(argv[1]) : 0.0;
	Display *xd = XOpenDisplay(NULL);
	if (!xd) {
		fprintf(stderr, "cannot open X display\n");
		return 1;
	}

	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xd);
	EGLint maj, min;
	if (!eglInitialize(dpy, &maj, &min)) {
		fprintf(stderr, "eglInitialize failed\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 16, EGL_NONE
	};
	EGLConfig cfg;
	EGLint n = 0;
	if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "no EGL config\n");
		return 1;
	}
	EGLint vid = 0;
	eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
	XVisualInfo tmpl = { .visualid = (VisualID)vid };
	int nvi = 0;
	XVisualInfo *vi = XGetVisualInfo(xd, VisualIDMask, &tmpl, &nvi);
	if (!vi) {
		fprintf(stderr, "no matching X visual\n");
		return 1;
	}

	XSetWindowAttributes swa = { 0 };
	swa.colormap = XCreateColormap(xd, DefaultRootWindow(xd), vi->visual,
				       AllocNone);
	swa.event_mask = StructureNotifyMask | KeyPressMask | ExposureMask;
	Window win = XCreateWindow(xd, DefaultRootWindow(xd), 100, 100, 640, 480,
				   0, vi->depth, InputOutput, vi->visual,
				   CWColormap | CWEventMask, &swa);
	Atom del = XInternAtom(xd, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(xd, win, &del, 1);
	XMapWindow(xd, win);

	EGLSurface surf = eglCreateWindowSurface(dpy, cfg,
						 (EGLNativeWindowType)win, NULL);
	static const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "EGL surface/context setup failed (0x%x)\n",
			eglGetError());
		return 1;
	}
	eglSwapInterval(dpy, 1);

	const char *renderer = (const char *)glGetString(GL_RENDERER);
	char shortr[64];
	snprintf(shortr, sizeof(shortr), "%.60s", renderer ? renderer : "?");
	char *comma = strchr(shortr, '(');
	if (comma) *comma = 0;
	printf("client: %zu-bit process, EGL %d.%d\nGL_RENDERER: %s\n",
	       sizeof(void *) * 8, maj, min, renderer);
	fflush(stdout);

	GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER,
		"attribute vec3 pos; attribute vec3 col; uniform mat4 mvp;"
		"varying vec3 v_col;"
		"void main(){ gl_Position = mvp * vec4(pos, 1.0); v_col = col; }"));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER,
		"precision mediump float; varying vec3 v_col;"
		"void main(){ gl_FragColor = vec4(v_col, 1.0); }"));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "col");
	glLinkProgram(prog);
	glUseProgram(prog);
	GLint u_mvp = glGetUniformLocation(prog, "mvp");

	/* 6 faces x 4 verts: position (3) + colour (3) */
	static const float face_col[6][3] = {
		{ 0.95f, 0.25f, 0.25f }, { 0.25f, 0.85f, 0.30f },
		{ 0.25f, 0.45f, 0.95f }, { 0.95f, 0.85f, 0.20f },
		{ 0.85f, 0.30f, 0.90f }, { 0.20f, 0.85f, 0.85f },
	};
	static const float corner[8][3] = {
		{ -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
		{ -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 },
	};
	static const int face_idx[6][4] = {
		{ 4, 5, 6, 7 }, { 1, 0, 3, 2 }, { 0, 4, 7, 3 },
		{ 5, 1, 2, 6 }, { 3, 7, 6, 2 }, { 0, 1, 5, 4 },
	};
	float verts[24 * 6];
	GLushort idx[36];
	for (int f = 0; f < 6; f++) {
		for (int v = 0; v < 4; v++) {
			float *o = verts + (f * 4 + v) * 6;
			memcpy(o, corner[face_idx[f][v]], 3 * sizeof(float));
			/* slight per-vertex shade so edges read as 3D */
			float sh = 0.75f + 0.25f * (v & 1);
			o[3] = face_col[f][0] * sh;
			o[4] = face_col[f][1] * sh;
			o[5] = face_col[f][2] * sh;
		}
		GLushort b = (GLushort)(f * 4);
		GLushort *i = idx + f * 6;
		i[0] = b; i[1] = b + 1; i[2] = b + 2;
		i[3] = b; i[4] = b + 2; i[5] = b + 3;
	}
	GLuint bufs[2];
	glGenBuffers(2, bufs);
	glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
			      (const void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
			      (const void *)(3 * sizeof(float)));
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);

	int w = 640, h = 480, running = 1;
	double t0 = now(), tfps = t0;
	long frames = 0, total = 0;
	while (running) {
		while (XPending(xd)) {
			XEvent ev;
			XNextEvent(xd, &ev);
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

		float proj[16], ry[16], rx[16], tr[16], m[16], mvp[16];
		mat_perspective(proj, 0.9f, (float)w / (float)h, 0.5f, 20.f);
		mat_rot_y(ry, (float)t * 0.9f);
		mat_rot_x(rx, (float)t * 0.55f + 0.4f);
		mat_identity(tr);
		tr[14] = -5.2f;
		mat_mul(m, rx, ry);
		mat_mul(m, tr, m);
		mat_mul(mvp, proj, m);

		glViewport(0, 0, w, h);
		glClearColor(0.07f, 0.08f, 0.11f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
		glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, (void *)0);
		eglSwapBuffers(dpy, surf);

		frames++;
		total++;
		double tn = now();
		if (tn - tfps >= 1.0) {
			char title[200];
			snprintf(title, sizeof(title),
				 "%zu-bit client | %s | %.0f fps",
				 sizeof(void *) * 8, shortr, frames / (tn - tfps));
			XStoreName(xd, win, title);
			XFlush(xd);
			printf("%s\n", title);
			fflush(stdout);
			frames = 0;
			tfps = tn;
		}
	}
	printf("done: %ld frames\n", total);

	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	XDestroyWindow(xd, win);
	XCloseDisplay(xd);
	return 0;
}

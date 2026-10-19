/*
 * fpscount.c - LD_PRELOAD frame counter. Wraps glXSwapBuffers / eglSwapBuffers /
 * SDL_GL_SwapWindow
 * and prints "FPSCOUNT <seconds> <frames> <fps>" to stderr every 2 s and at
 * exit, so unmodified applications can be compared with the frame rate
 * unlocked (run with __GL_SYNC_TO_VBLANK=0).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>

static unsigned long frames, last_frames;
static __thread int in_sdl; /* SDL_GL_SwapWindow calls glXSwapBuffers: count once */
static double t_start, t_last;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void report(int final)
{
	double t = now();

	if (final) {
		fprintf(stderr, "FPSCOUNT total %.2f %lu %.1f\n", t - t_start,
			frames, frames / (t - t_start));
		return;
	}
	if (t - t_last >= 2.0) {
		fprintf(stderr, "FPSCOUNT %.1f %lu %.1f\n", t - t_start, frames,
			(frames - last_frames) / (t - t_last));
		last_frames = frames;
		t_last = t;
	}
}

static void tick(void)
{
	if (!frames) {
		t_start = t_last = now();
	}
	frames++;
	report(0);
}

__attribute__((destructor)) static void fini(void)
{
	if (frames)
		report(1);
}

void glXSwapBuffers(void *dpy, unsigned long drawable)
{
	static void (*real)(void *, unsigned long);

	if (!real)
		real = dlsym(RTLD_NEXT, "glXSwapBuffers");
	if (!in_sdl)
		tick();
	real(dpy, drawable);
}

/* SDL2 applications load libGL themselves, so wrap the SDL call as well */
void SDL_GL_SwapWindow(void *window)
{
	static void (*real)(void *);

	if (!real)
		real = dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
	tick();
	in_sdl = 1;
	real(window);
	in_sdl = 0;
}

unsigned eglSwapBuffers(void *dpy, void *surf)
{
	static unsigned (*real)(void *, void *);

	if (!real)
		real = dlsym(RTLD_NEXT, "eglSwapBuffers");
	if (!in_sdl)
		tick();
	return real(dpy, surf);
}

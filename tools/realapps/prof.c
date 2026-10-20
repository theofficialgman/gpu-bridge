/*
 * prof.c - LD_PRELOAD CPU profiler for processes perf cannot be used on:
 * SIGPROF every 1 ms of CPU time (all threads), program counter histogram,
 * written at exit to $PROF_OUT (default /tmp/prof.<pid>.txt) as
 * "samples module symbol+offset". Resolve static functions offline with
 * addr2line against the unstripped library.
 * Build: gcc -shared -fPIC -O2 -o prof64.so prof.c -ldl  (armhf: arm-linux-gnueabihf-gcc)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAXS 400000
static volatile unsigned long pcs[MAXS];
static volatile unsigned n;
static unsigned long total_tid[8];

static void handler(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = uc_;
	unsigned long pc;

	(void)sig; (void)si;
#if defined(__aarch64__)
	pc = uc->uc_mcontext.pc;
#elif defined(__arm__)
	pc = uc->uc_mcontext.arm_pc;
#else
	pc = 0;
#endif
	unsigned i = __sync_fetch_and_add(&n, 1);
	if (i < MAXS)
		pcs[i] = pc;
}

static int cmp(const void *a, const void *b)
{
	unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;

	return x < y ? -1 : x > y;
}

__attribute__((destructor)) static void dump(void)
{
	struct itimerval z = { { 0, 0 }, { 0, 0 } };
	char path[256];
	unsigned cnt = n < MAXS ? n : MAXS;

	setitimer(ITIMER_PROF, &z, NULL);
	if (!cnt)
		return;
	const char *o = getenv("PROF_OUT");
	if (o)
		snprintf(path, sizeof path, "%s", o);
	else
		snprintf(path, sizeof path, "/tmp/prof.%d.txt", (int)getpid());
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	/* aggregate by (module, symbol or 64-byte bucket) */
	struct ent { char key[200]; unsigned c; } *e = calloc(cnt, sizeof *e);
	unsigned ne = 0;
	for (unsigned i = 0; i < cnt; i++) {
		Dl_info di;
		char key[200];

		if (dladdr((void *)pcs[i], &di) && di.dli_fname) {
			const char *base = strrchr(di.dli_fname, '/');
			base = base ? base + 1 : di.dli_fname;
			if (di.dli_sname)
				snprintf(key, sizeof key, "%s %s", base, di.dli_sname);
			else
				snprintf(key, sizeof key, "%s +0x%lx", base,
					 (unsigned long)((char *)pcs[i] - (char *)di.dli_fbase) & ~0x3fUL);
		} else {
			snprintf(key, sizeof key, "? 0x%lx", pcs[i] & ~0xfffUL);
		}
		unsigned j;
		for (j = 0; j < ne; j++)
			if (!strcmp(e[j].key, key))
				break;
		if (j == ne) {
			strcpy(e[ne].key, key);
			ne++;
		}
		e[j].c++;
	}
	for (unsigned a = 0; a < ne; a++) /* selection sort of the top 60 */
		for (unsigned b = a + 1; b < ne && a < 60; b++)
			if (e[b].c > e[a].c) {
				struct ent t = e[a]; e[a] = e[b]; e[b] = t;
			}
	fprintf(f, "total %u samples\n", cnt);
	for (unsigned a = 0; a < ne && a < 60; a++)
		fprintf(f, "%6u %5.1f%% %s\n", e[a].c, 100.0 * e[a].c / cnt, e[a].key);
	fclose(f);
	(void)cmp; (void)total_tid;
}

static void usr2(int sig)
{
	(void)sig;
	dump();
}

__attribute__((constructor)) static void init(void)
{
	struct sigaction sa;
	struct itimerval it = { { 0, 1000 }, { 0, 1000 } };

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	signal(SIGUSR2, usr2); /* dump now (processes that _exit) */
	setitimer(ITIMER_PROF, &it, NULL);
}

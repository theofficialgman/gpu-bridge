/*
 * pingpong.c - what a synchronous request/reply costs between two processes
 * for different ways of waiting: the server does `work_us` of busy work per
 * request (a driver call), the client waits for the reply. Prints round trip
 * time and the CPU time each side burns per round trip.
 *
 *   pingpong <strategy> <work_us> [iters]
 *   strategy: pause (spin with the CPU yield hint), yield (pause 64 then
 *   sched_yield, like the ring today), futex (sleep at once), wfe (wait for
 *   event on the exclusive monitor), hybrid_pause / hybrid_wfe (spin or wfe
 *   for spin_us, then futex).
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct shm {
	_Alignas(64) volatile uint32_t req;
	_Alignas(64) volatile uint32_t rsp;
	_Alignas(64) volatile uint32_t req_sleep, rsp_sleep;
};

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static double cpu(void)
{
	struct timespec t;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static inline void relax(void)
{
#if defined(__aarch64__) || defined(__arm__)
	__asm__ volatile("yield" ::: "memory");
#else
	__asm__ volatile("pause" ::: "memory");
#endif
}

static void fwait(volatile uint32_t *p, uint32_t val)
{
	syscall(SYS_futex, p, FUTEX_WAIT, val, NULL, NULL, 0);
}

static void fwake(volatile uint32_t *p)
{
	syscall(SYS_futex, p, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static inline uint32_t wfe_until_not(volatile uint32_t *p, uint32_t cur, double limit_s)
{
	uint32_t v;
	double t0 = limit_s > 0 ? now() : 0;

#if defined(__aarch64__)
	for (;;) {
		__asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
		if (v != cur)
			return v;
		if (limit_s > 0 && now() - t0 > limit_s)
			return v;
		__asm__ volatile("wfe" ::: "memory");
	}
#elif defined(__arm__)
	for (;;) {
		__asm__ volatile("ldrex %0, [%1]" : "=r"(v) : "r"(p) : "memory");
		if (v != cur)
			return v;
		if (limit_s > 0 && now() - t0 > limit_s)
			return v;
		__asm__ volatile("wfe" ::: "memory");
	}
#else
	(void)t0;
	while ((v = *p) == cur)
		relax();
	return v;
#endif
}

static void pin(const char *env)
{
	const char *e = getenv(env);

	if (e && *e) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(atoi(e), &set);
		sched_setaffinity(0, sizeof set, &set);
	}
}

static int strat; /* 0 pause 1 yield 2 futex 3 wfe 4 hybrid_pause 5 hybrid_wfe */
static double spin_s = 25e-6;

/* wait until *p != cur; sleepflag is set while a futex sleeper is parked */
static uint32_t wait_change(volatile uint32_t *p, uint32_t cur, volatile uint32_t *sleepflag)
{
	uint32_t v;
	double t0;
	int n = 0;

	switch (strat) {
	case 0:
		while ((v = *p) == cur)
			relax();
		return v;
	case 1:
		while ((v = *p) == cur) {
			if (n++ < 64)
				relax();
			else
				sched_yield();
		}
		return v;
	case 3:
		return wfe_until_not(p, cur, 0);
	case 4:
		t0 = now();
		while ((v = *p) == cur && now() - t0 < spin_s)
			relax();
		break;
	case 5:
		v = wfe_until_not(p, cur, spin_s);
		break;
	default:
		v = *p;
	}
	while (v == cur) { /* futex */
		*sleepflag = 1;
		__sync_synchronize();
		if (*p == cur)
			fwait(p, cur);
		*sleepflag = 0;
		v = *p;
	}
	return v;
}

static void publish(volatile uint32_t *p, uint32_t v, volatile uint32_t *sleepflag)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
	if (strat >= 2 && strat != 3) {
		__sync_synchronize();
		if (*sleepflag)
			fwake(p);
	}
}

static void busy(double us)
{
	double t0 = now();

	while ((now() - t0) * 1e6 < us)
		;
}

int main(int argc, char **argv)
{
	const char *names[] = { "pause", "yield", "futex", "wfe", "hybrid_pause", "hybrid_wfe" };
	double work_us = argc > 2 ? atof(argv[2]) : 0;
	long iters = argc > 3 ? atol(argv[3]) : 100000;
	struct shm *s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (argc < 2)
		return 2;
	for (strat = 0; strat < 6; strat++)
		if (!strcmp(argv[1], names[strat]))
			break;
	if (strat == 6)
		return 2;
	if (getenv("SPIN_US"))
		spin_s = atof(getenv("SPIN_US")) * 1e-6;
	memset((void *)s, 0, sizeof *s);
	double srv_cpu_fd[2];
	int pf[2];

	pipe(pf);
	pid_t pid = fork();
	if (!pid) { /* server */
		uint32_t seen = 0;

		pin("PIN_SERVER");

		for (long i = 0; i < iters; i++) {
			uint32_t r = wait_change(&s->req, seen, &s->req_sleep);
			seen = r;
			if (work_us > 0)
				busy(work_us);
			publish(&s->rsp, r, &s->rsp_sleep);
		}
		double c = cpu();
		write(pf[1], &c, sizeof c);
		_exit(0);
	}
	(void)srv_cpu_fd;
	pin("PIN_CLIENT"); /* env PIN_CLIENT / PIN_SERVER: core numbers */
	double t0 = now(), c0 = cpu();

	for (long i = 1; i <= iters; i++) {
		publish(&s->req, (uint32_t)i, &s->req_sleep);
		wait_change(&s->rsp, (uint32_t)i - 1, &s->rsp_sleep);
		/* rsp already equals i when the server was fast: wait_change on
		 * (i-1) returns at once only if rsp != i-1 */
	}
	double dt = now() - t0, cc = cpu() - c0, sc = 0;

	read(pf[0], &sc, sizeof sc);
	waitpid(pid, NULL, 0);
	printf("%-13s work %5.1f us: round trip %7.2f us  client cpu %6.2f us  server cpu %6.2f us  (overhead over work+server: %6.2f us)\n",
	       names[strat], work_us, dt / iters * 1e6, cc / iters * 1e6, sc / iters * 1e6,
	       (cc + sc) / iters * 1e6 - work_us);
	return 0;
}

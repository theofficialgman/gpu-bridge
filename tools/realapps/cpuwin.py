#!/usr/bin/env python3
"""Run a command and report CPU use per process name over its run.

usage: cpuwin.py [--idle SECS] [--top N] -- cmd args...

Samples /proc/<pid>/stat (utime+stime) of every process 2x a second, so
processes that start or exit during the run are included. Prints the top N
process names by CPU seconds (100% = one core of the 4), the sum over all
processes, and the same figures for an idle window taken first, so the
compositor/desktop baseline can be read off.
"""
import os, sys, time, subprocess, threading, collections

TCK = os.sysconf("SC_CLK_TCK")


def snap():
    out = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            s = open(f"/proc/{p}/stat").read()
            comm = s[s.index("(") + 1:s.rindex(")")]
            f = s[s.rindex(")") + 2:].split()
            out[int(p)] = (comm, int(f[11]) + int(f[12]), int(s[s.rindex(")") + 2:].split()[19]))
        except Exception:
            pass
    return out


class Sampler(threading.Thread):
    def __init__(self):
        super().__init__()
        self.stop = threading.Event()
        self.first, self.last = {}, {}

    def run(self):
        while not self.stop.is_set():
            for pid, (comm, ticks, st) in snap().items():
                key = (pid, st)  # start time distinguishes recycled pids
                self.first.setdefault(key, (comm, ticks))
                self.last[key] = (comm, ticks)
            time.sleep(0.5)

    def totals(self):
        t = collections.Counter()
        for key, (comm, ticks) in self.last.items():
            t[comm] += ticks - self.first[key][1]
        return t


def window(fn):
    s = Sampler()
    s.start()
    t0 = time.time()
    ret = fn()
    wall = time.time() - t0
    s.stop.set()
    s.join()
    return s.totals(), wall, ret


def show(title, tot, wall, top):
    print(f"--- {title} (wall {wall:.1f} s; 100% = one core) ---")
    allsum = sum(tot.values())
    for comm, ticks in tot.most_common(top):
        print(f"  {comm:<22s} {ticks / TCK:7.1f} cpu-s  {100 * ticks / TCK / wall:6.1f}%")
    print(f"  {'ALL PROCESSES':<22s} {allsum / TCK:7.1f} cpu-s  {100 * allsum / TCK / wall:6.1f}%")


args = sys.argv[1:]
idle, top = 10.0, 10
while args and args[0] != "--":
    if args[0] == "--idle":
        idle = float(args[1]); args = args[2:]
    elif args[0] == "--top":
        top = int(args[1]); args = args[2:]
    else:
        break
cmd = args[1:]
if idle > 0:
    tot, wall, _ = window(lambda: time.sleep(idle))
    show("idle baseline", tot, wall, top)
tot, wall, rc = window(lambda: subprocess.call(cmd))
show("run: " + " ".join(cmd)[:80], tot, wall, top)
print(f"  exit status {rc}")

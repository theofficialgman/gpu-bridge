#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""why_missing.py native.txt bridged.txt build/gen/gb_client_all_gen.c

For each extension the native driver advertises but the bridge does not, say
why: absent from the registry tables (probably enum-only: add to
EXT_NO_COMMANDS) or the entry points that are not implemented."""
import re, sys
nat, br, gen = sys.argv[1:4]
def exts(f):
    d = {}
    for l in open(f):
        t = l.split()
        if len(t) == 2 and (t[1].startswith(("GL_", "EGL_", "GLX_"))):
            d.setdefault(t[0], set()).add(t[1])
    return d
n, b = exts(nat), exts(br)
src = open(gen).read()
tab = {m[0]: [x.strip().strip('"') for x in m[2].split(",")
              if x.strip().strip('"') and x.strip() != "0"]
       for m in re.findall(r'\{ "((?:GL|EGL|GLX)_\w+)", (\d+), \(const char \*const\[\]\)\{ ([^}]*)\}', src)}
procs = set(re.findall(r'\{ "(\w+)", \(void \*\)\w+ \}', src))
seen = {}
for tag, s in n.items():
    for e in sorted(s - b.get(tag, set())):
        if e in seen:
            continue
        if e not in tab:
            seen[e] = "not in registry tables (enum-only? add to EXT_NO_COMMANDS)"
        else:
            miss = [f for f in tab[e] if f not in procs]
            seen[e] = ("missing entry points: " + " ".join(miss)) if miss else \
                      "all entry points implemented (filtered for another reason)"
for e, why in sorted(seen.items()):
    print("%-40s %s" % (e, why))

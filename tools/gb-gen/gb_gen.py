#!/usr/bin/env python3
"""
gb-gen: generate gpu-bridge encoders (client) and decoders (server) from the
Khronos registries.

Usage: gb_gen.py <registry-dir> <out-dir> [--report]

Principles (see ARCHITECTURE.md section 5.3):
  * Never guess a length. A pointer parameter whose size we cannot derive
    from the registry (or from a rule in gen_config.py) is classified
    UNSUPPORTED and gets a stub that logs + returns an error; it is listed in
    the report so a human decides.
  * Wire types are fixed width. Pointer-width quantities travel as 64-bit.
  * The server never trusts a client length: it recomputes the expected byte
    count from the already-decoded scalar parameters and rejects mismatches.
"""
import re
import sys
import zlib
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gen_config as cfg  # noqa: E402

# --------------------------------------------------------------------------
# Registry parsing
# --------------------------------------------------------------------------


def c_type_of(e):
    """C type text of a <proto>/<param> element, without the <name>."""
    s = e.text or ""
    for ch in e:
        if ch.tag != "name":
            s += ch.text or ""
        s += ch.tail or ""
    return " ".join(s.split())


def split_type(ctype):
    """'const GLchar *const*' -> (base, ptr_depth, is_const_base)."""
    t = ctype.replace("*", " * ")
    toks = t.split()
    depth = toks.count("*")
    words = [w for w in toks if w not in ("*", "const", "struct")]
    base = " ".join(words)
    # const on the pointee: 'const T *' -> first token is const.
    const_base = toks and toks[0] == "const"
    return base, depth, const_base


class Cmd:
    def __init__(self, name, ret, params, api):
        self.name = name
        self.ret = ret            # C type text
        self.params = params      # list of dict(name, ctype, len)
        self.api = api            # 'gl' or 'egl'
        self.status = None        # generated|wrapped|manual|unsupported
        self.reason = ""
        self.sync = False


def load_commands(regdir):
    out = {}
    gl = ET.parse(regdir / "gl.xml").getroot()
    egl = ET.parse(regdir / "egl.xml").getroot()
    for root, api in ((gl, "gl"), (egl, "egl")):
        cmds = root.find("commands")
        for c in cmds.findall("command"):
            p = c.find("proto")
            name = p.find("name").text
            params = []
            for pe in c.findall("param"):
                params.append(dict(name=pe.find("name").text,
                                   ctype=c_type_of(pe),
                                   len=pe.get("len")))
            out[name] = Cmd(name, c_type_of(p), params, api)
    return out, gl, egl


def feature_commands(root, api, versions):
    names = []
    for f in root.findall("feature"):
        if f.get("api") != api:
            continue
        if f.get("number") not in versions and f.get("name") not in versions:
            continue
        for r in f.findall("require"):
            for c in r.findall("command"):
                names.append(c.get("name"))
    return names


def extension_commands(root, ext_names, api_ok):
    names = []
    for e in root.find("extensions").findall("extension"):
        if e.get("name") not in ext_names:
            continue
        if api_ok and e.get("supported") and not any(
                a in e.get("supported").split("|") for a in api_ok):
            continue
        for r in e.findall("require"):
            if r.get("api") and r.get("api") not in api_ok:
                continue
            for c in r.findall("command"):
                names.append(c.get("name"))
    return names


# --------------------------------------------------------------------------
# Classification
# --------------------------------------------------------------------------

# C type -> (wire kind, wire bytes)
VAL = {}
for t in ("GLenum", "GLbitfield", "GLuint", "GLubyte", "GLushort", "GLboolean",
          "GLhalf", "GLhalfNV", "GLchar", "GLcharARB", "EGLBoolean", "EGLenum",
          "EGLuint64KHR_lo"):
    VAL[t] = ("u32", 4)
for t in ("GLint", "GLsizei", "GLbyte", "GLshort", "GLfixed", "GLclampx",
          "EGLint"):
    VAL[t] = ("i32", 4)
for t in ("GLfloat", "GLclampf"):
    VAL[t] = ("f32", 4)
for t in ("GLdouble", "GLclampd"):
    VAL[t] = ("f64", 8)
for t in ("GLint64", "GLint64EXT", "GLintptr", "GLsizeiptr", "GLintptrARB",
          "GLsizeiptrARB", "EGLAttrib", "EGLnsecsANDROID", "EGLTime",
          "EGLTimeKHR"):
    VAL[t] = ("i64", 8)
for t in ("GLuint64", "GLuint64EXT", "EGLuint64KHR"):
    VAL[t] = ("u64", 8)
# EGLTime is khronos_utime_nanoseconds_t (unsigned); fix up:
VAL["EGLTime"] = VAL["EGLTimeKHR"] = ("u64", 8)

HANDLES = {
    "EGLDisplay": "DISPLAY", "EGLConfig": "CONFIG", "EGLSurface": "SURFACE",
    "EGLContext": "CONTEXT", "EGLImage": "IMAGE", "EGLImageKHR": "IMAGE",
    "EGLSync": "SYNC", "EGLSyncKHR": "SYNC", "EGLStreamKHR": "STREAM",
    "EGLDeviceEXT": "DEVICE", "GLsync": "GLSYNC",
    "GLeglImageOES": "IMAGE",
}

# element types usable inside arrays: base -> (mode, wire bytes)
ELEM = {}
for t in ("GLchar", "GLcharARB", "GLbyte", "GLubyte", "GLboolean", "char",
          "void", "GLvoid"):
    ELEM[t] = ("raw", 1)
for t in ("GLshort", "GLushort", "GLhalf"):
    ELEM[t] = ("raw", 2)
for t in ("GLint", "GLuint", "GLenum", "GLfloat", "GLsizei", "GLfixed",
          "GLbitfield", "EGLint", "EGLBoolean", "EGLenum"):
    ELEM[t] = ("raw", 4)
for t in ("GLint64", "GLuint64", "GLdouble", "GLint64EXT", "GLuint64EXT"):
    ELEM[t] = ("raw", 8)
for t in ("GLintptr", "GLsizeiptr", "EGLAttrib", "GLintptrARB",
          "GLsizeiptrARB"):
    ELEM[t] = ("widen", 8)
for t, _ in HANDLES.items():
    ELEM[t] = ("handle", 4)

NUM_RE = re.compile(r"^\d+$")
ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def parse_len(cmd, pname, expr, params):
    """Return a (kind, payload) describing how to size an array.

    ('expr', 'count*4')            plain expression of scalar params
    ('compsize', 'pname'|'pixels') a helper-defined size
    None                           unknown -> unsupported
    """
    override = cfg.LEN_OVERRIDES.get((cmd, pname))
    if override:
        expr = override
    if expr is None:
        return None
    pnames = {p["name"] for p in params}
    m = re.match(r"^COMPSIZE\((.*)\)$", expr)
    if m:
        args = tuple(a.strip() for a in m.group(1).split(","))
        key = cfg.COMPSIZE.get((cmd, pname)) or cfg.COMPSIZE.get(args)
        if key and all(a in pnames for a in args):
            return ("compsize", key, args)
        return None
    # arithmetic over identifiers and integers only
    ids = ID_RE.findall(expr)
    if any(i not in pnames for i in ids):
        return None
    if not re.match(r"^[A-Za-z0-9_\s\*\+\-\(\)]+$", expr):
        return None
    return ("expr", expr)


def classify(cmd, params_by_type):
    """Fill cmd.status/reason and annotate each param with 'k'."""
    name = cmd.name
    if name in cfg.MANUAL:
        cmd.status = "manual"
        return
    if name in cfg.UNSUPPORTED:
        cmd.status = "unsupported"
        cmd.reason = cfg.UNSUPPORTED[name]
        return

    # return type
    rb, rd, _ = split_type(cmd.ret)
    if rd:
        cmd.status, cmd.reason = "unsupported", f"pointer return '{cmd.ret}'"
        return
    if rb == "void":
        cmd.rk = ("void",)
    elif rb in HANDLES:
        cmd.rk = ("handle", HANDLES[rb])
    elif rb in VAL:
        cmd.rk = ("val",) + VAL[rb]
    else:
        cmd.status, cmd.reason = "unsupported", f"return type '{cmd.ret}'"
        return

    for p in cmd.params:
        base, depth, const = split_type(p["ctype"])
        p["base"], p["depth"], p["const"] = base, depth, const
        n = p["name"]
        if depth == 0:
            if base in HANDLES:
                p["k"] = ("handle", HANDLES[base])
            elif base in VAL:
                p["k"] = ("val",) + VAL[base]
            else:
                cmd.status = "unsupported"
                cmd.reason = f"param {n}: type '{p['ctype']}'"
                return
            continue
        if depth > 1:
            cmd.status = "unsupported"
            cmd.reason = f"param {n}: '{p['ctype']}'"
            return

        # one level of pointer
        is_in = const
        if (name, n) in cfg.ATTRIB_LISTS or (
                cmd.api == "egl" and n in ("attrib_list", "attribs")
                and base in ("EGLint", "EGLAttrib") and const):
            mode, wsz = ELEM[base]
            p["k"] = ("attrib", base, mode, wsz)
            continue
        if base in ("GLchar", "GLcharARB", "char") and is_in and \
                (name, n) not in cfg.LEN_OVERRIDES and \
                (p["len"] is None or
                 re.match(r"^COMPSIZE\(\s*(%s)?\s*\)$" % re.escape(n),
                          p["len"] or "")):
            p["k"] = ("str",)
            continue
        if base not in ELEM:
            cmd.status = "unsupported"
            cmd.reason = f"param {n}: element type '{p['ctype']}'"
            return
        mode, wsz = ELEM[base]
        ln = parse_len(name, n, p["len"], cmd.params)
        if ln is None:
            if p["len"] is None and base != "void":
                ln = ("expr", "1")     # single element (no len attr)
            else:
                cmd.status = "unsupported"
                cmd.reason = f"param {n}: cannot size '{p['ctype']}' " \
                             f"(len={p['len']})"
                return
        p["k"] = ("arr_in" if is_in else "arr_out", base, mode, wsz, ln)

    cmd.status = "generated"
    cmd.sync = (cmd.rk[0] != "void" or
                any(p["k"][0] == "arr_out" for p in cmd.params) or
                name in cfg.FORCE_SYNC)
    if name in cfg.WRAPPED:
        cmd.status = "wrapped"


# --------------------------------------------------------------------------
# C emission helpers
# --------------------------------------------------------------------------

WIRE_W = {"u32": "gb_w_u32", "i32": "gb_w_u32", "f32": "gb_w_f32",
          "u64": "gb_w_u64", "i64": "gb_w_u64", "f64": "gb_w_f64"}
WIRE_R = {"u32": "gb_r_u32", "i32": "gb_r_u32", "f32": "gb_r_f32",
          "u64": "gb_r_u64", "i64": "gb_r_u64", "f64": "gb_r_f64"}


def w_expr(kind, e):
    if kind == "i32":
        return f"(uint32_t)(int32_t)({e})"
    if kind == "u32":
        return f"(uint32_t)({e})"
    if kind == "i64":
        return f"(uint64_t)(int64_t)({e})"
    if kind == "u64":
        return f"(uint64_t)({e})"
    return f"({e})"


def r_expr(kind, call):
    if kind == "i32":
        return f"(int32_t){call}"
    if kind == "i64":
        return f"(int64_t){call}"
    return call


def align8(n):
    return (n + 7) & ~7


class Layout:
    """Simulate codec alignment to get the constant end of the fixed part."""

    def __init__(self, start=16):
        self.off = start

    def u32(self):
        self.off = (self.off + 3) & ~3
        self.off += 4

    def u64(self):
        self.off = align8(self.off)
        self.off += 8


def count_c(ln, names):
    """C expression yielding the element count as int64_t (>=0 clamped)."""
    kind = ln[0]
    if kind == "expr":
        # widen every operand first so GLsizei*N cannot overflow in int
        e = ID_RE.sub(
            lambda m: f"((int64_t){names[m.group(0)]})"
            if m.group(0) in names else m.group(0), ln[1])
        return f"gb_capcount({e})"
    key, args = ln[1], ln[2]
    a = ", ".join(names[x] for x in args)
    if key == "pname":
        return f"gb_capcount(GB_COUNT_PNAME(__op, {a}))"
    if key == "pixels":
        return f"gb_capcount(GB_BYTES_PIXELS(__op, {a}))"
    raise SystemExit(f"unknown compsize key {key}")


# --------------------------------------------------------------------------
# Client emission
# --------------------------------------------------------------------------


def emit_client(cmd, public, opname):
    P = cmd.params
    fn = cmd.name if public else "gbx_" + cmd.name
    ret_t = cmd.ret
    plist = ", ".join(f"{p['ctype']} {p['name']}" for p in P) or "void"
    L = []
    api = cmd.api
    vis = '__attribute__((visibility("default"))) ' if public else ""
    L.append(f"{vis}{ret_t} {fn}({plist})")
    L.append("{")
    L.append(f"\tconst uint32_t __op = {opname};")
    L.append("\tstruct gb_cl *__cx = gb_cl_get();")
    zero = {"void": "", }.get(cmd.rk[0], "")
    if cmd.rk[0] == "void":
        L.append("\tif (!__cx) return;")
    else:
        L.append(f"\t{ret_t} __ret = ({ret_t})0;")
        L.append("\tif (!__cx) return __ret;")
    retstmt = "return;" if cmd.rk[0] == "void" else "return __ret;"

    names = {p["name"]: p["name"] for p in P}
    lay = Layout()
    # pass 1: blob byte lengths
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] in ("val",):
            (lay.u64 if k[2] == 8 else lay.u32)()
        elif k[0] == "handle":
            lay.u32()
        elif k[0] in ("str", "arr_in", "arr_out", "attrib"):
            lay.u32()
    fixed_end = lay.off

    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "str":
            L.append(f"\tuint32_t __l_{n} = {n} ? (uint32_t)strlen({n}) + 1 "
                     f": GB_NULL_LEN;")
        elif k[0] in ("arr_in", "arr_out"):
            cexp = count_c(k[4], names)
            L.append(f"\tint64_t __n_{n} = {cexp};")
            L.append(f"\tif (__n_{n} < 0) __n_{n} = 0; /* unsized */")
            L.append(f"\tuint32_t __l_{n} = {n} ? (uint32_t)(__n_{n} * "
                     f"{k[3]}) : GB_NULL_LEN;")
            L.append(f"\tif ({n} && __n_{n} * {k[3]} > GB_MAX_ARRAY_BYTES) "
                     f"{{ gb_cl_set_error(__cx, __op); {retstmt} }}")
        elif k[0] == "attrib":
            L.append(f"\tint64_t __n_{n} = {n} ? gb_attrib_count_{k[1]}({n}) "
                     f": -1;")
            L.append(f"\tif ({n} && __n_{n} < 0) {{ gb_cl_set_error(__cx, "
                     f"__op); {retstmt} }}")
            L.append(f"\tuint32_t __l_{n} = {n} ? (uint32_t)(__n_{n} * "
                     f"{k[3]}) : GB_NULL_LEN;")

    # total size
    L.append(f"\tuint32_t __t = {fixed_end}u;")
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] in ("str", "arr_in", "attrib"):
            L.append(f"\tif (__l_{n} != GB_NULL_LEN) __t = "
                     f"GB_ALIGN_UP(__t, 8u) + __l_{n};")
    L.append("\t__t = GB_ALIGN_UP(__t, 8u);")

    flags = "GB_CMD_F_WANT_REPLY" if cmd.sync else "0"
    L.append("\tstruct gb_out __o;")
    L.append(f"\tif (gb_cl_begin(__cx, &__o, __op, __t, {flags})) "
             f"{{ {retstmt} }}")
    L.append("\tstruct gb_w __w;")
    L.append("\tgb_w_init(&__w, __o.p);")

    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "val":
            L.append(f"\t{WIRE_W[k[1]]}(&__w, {w_expr(k[1], n)});")
        elif k[0] == "handle":
            L.append(f"\tgb_w_u32(&__w, (uint32_t)(uintptr_t)({n}));")
        else:
            L.append(f"\tgb_w_u32(&__w, __l_{n});")
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "str":
            L.append(f"\tif (__l_{n} != GB_NULL_LEN) memcpy(gb_w_blob(&__w, "
                     f"__l_{n}), {n}, __l_{n});")
        elif k[0] in ("arr_in", "attrib"):
            mode, wsz = (k[2], k[3])
            L.append(f"\tif (__l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tvoid *__d = gb_w_blob(&__w, __l_{n});")
            if mode == "raw":
                L.append(f"\t\tmemcpy(__d, {n}, __l_{n});")
            else:
                cnt = f"__n_{n}" if k[0] == "attrib" else f"__n_{n}"
                L.append(f"\t\tfor (int64_t __i = 0; __i < {cnt}; __i++) {{")
                if mode == "widen":
                    L.append(f"\t\t\t((uint64_t *)__d)[__i] = (uint64_t)"
                             f"(int64_t){n}[__i];")
                else:  # handle
                    L.append(f"\t\t\t((uint32_t *)__d)[__i] = (uint32_t)"
                             f"(uintptr_t){n}[__i];")
                L.append("\t\t}")
            L.append("\t}")

    L.append("\tgb_cl_end(__cx, &__o);")

    if cmd.sync:
        L.append("\tstruct gb_in __in;")
        L.append(f"\tif (gb_cl_wait_reply(__cx, &__in, __op)) {{ {retstmt} }}")
        L.append("\tstruct gb_r __r;")
        L.append("\tgb_r_init(&__r, __in.p, __in.size);")
        if cmd.rk[0] == "val":
            k = cmd.rk
            if k[1] == "f32":
                L.append("\t__ret = gb_r_f32(&__r); (void)gb_r_u32(&__r);")
            elif k[1] == "f64":
                L.append("\t__ret = gb_r_f64(&__r);")
            else:
                L.append(f"\t__ret = ({ret_t}){r_expr(k[1], 'gb_r_u64(&__r)')};")
        elif cmd.rk[0] == "handle":
            L.append(f"\t__ret = ({ret_t})(uintptr_t)(uint32_t)"
                     f"gb_r_u64(&__r);")
        for p in P:
            k = p["k"]
            n = p["name"]
            if k[0] != "arr_out":
                continue
            mode, wsz = k[2], k[3]
            L.append(f"\t{{ uint32_t __rl = gb_r_u32(&__r);")
            L.append(f"\t  if (__rl != GB_NULL_LEN) {{")
            L.append(f"\t    const void *__s = gb_r_blob(&__r, __rl);")
            L.append(f"\t    if (__s && {n} && __rl == __l_{n}) {{")
            if mode == "raw":
                L.append(f"\t      memcpy({n}, __s, __rl);")
            elif mode == "widen":
                L.append(f"\t      for (int64_t __i = 0; __i < __n_{n}; "
                         f"__i++) {n}[__i] = (__typeof__({n}[0]))"
                         f"((const int64_t *)__s)[__i];")
            else:
                L.append(f"\t      for (int64_t __i = 0; __i < __n_{n}; "
                         f"__i++) {n}[__i] = (__typeof__({n}[0]))"
                         f"(uintptr_t)((const uint32_t *)__s)[__i];")
            L.append("\t    }")
            L.append("\t  }")
            L.append("\t}")
        L.append("\tif (__r.err || (((const struct gb_cmd_hdr *)__in.p)->flags"
                 " & GB_CMD_F_ERROR)) gb_cl_set_error(__cx, __op);")
        L.append("\tgb_in_release(&__in);")
        L.append(f"\t{retstmt}")
    L.append("}")
    return "\n".join(L)


def emit_client_stub(cmd, opname):
    plist = ", ".join(f"{p['ctype']} {p['name']}" for p in cmd.params) or "void"
    L = [f'__attribute__((visibility("default"))) {cmd.ret} '
         f'{cmd.name}({plist})', "{"]
    L.append(f'\tgb_cl_unsupported_{cmd.api}("{cmd.name}");')
    b, d, _ = split_type(cmd.ret)
    if b == "void" and not d:
        L.append("\treturn;")
    else:
        L.append(f"\treturn ({cmd.ret})0;")
    L.append("}")
    return "\n".join(L)


# --------------------------------------------------------------------------
# Server emission
# --------------------------------------------------------------------------


def native_cast(p):
    return p["ctype"]


def emit_server(cmd, opname):
    P = cmd.params
    L = []
    L.append(f"static void srv_{cmd.name}(struct gb_sth *__t, const struct "
             f"gb_cmd_hdr *__h, struct gb_r *__r)")
    L.append("{")
    L.append(f"\tconst uint32_t __op = {opname}; (void)__op;")
    names = {p["name"]: "a_" + p["name"] for p in P}

    # fixed fields
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "val":
            t = {"u32": "uint32_t", "i32": "int32_t", "f32": "float",
                 "u64": "uint64_t", "i64": "int64_t", "f64": "double"}[k[1]]
            L.append(f"\t{t} v_{n} = {r_expr(k[1], WIRE_R[k[1]] + '(__r)')};")
            L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})v_{n};")
        elif k[0] == "handle":
            L.append(f"\tuint32_t h_{n} = gb_r_u32(__r);")
        else:
            L.append(f"\tuint32_t l_{n} = gb_r_u32(__r);")
    L.append("\tif (__r->err) goto proto_error;")

    # resolve handles
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "handle":
            L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})gb_h2p(HK_{k[1]}, "
                     f"h_{n});")

    # blobs
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "str":
            L.append(f"\tconst char *s_{n} = NULL;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tif (l_{n} == 0 || l_{n} > GB_MAX_ARRAY_BYTES) "
                     f"goto proto_error;")
            L.append(f"\t\ts_{n} = gb_r_blob(__r, l_{n});")
            L.append(f"\t\tif (!s_{n} || s_{n}[l_{n} - 1] != 0) "
                     f"goto proto_error;")
            L.append("\t}")
            L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})s_{n};")
        elif k[0] == "arr_in":
            mode, wsz = k[2], k[3]
            cexp = count_c(k[4], names)
            L.append(f"\tconst void *b_{n} = NULL; int64_t n_{n} = 0;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tn_{n} = {cexp};")
            L.append(f"\t\tif (n_{n} < 0) n_{n} = 0; else b_{n} = "
                     f"(const void *)1; /* sized */")
            L.append(f"\t\tif (n_{n} * {wsz} != (int64_t)l_{n}) "
                     f"goto proto_error;")
            L.append(f"\t\tconst void *blob_{n} = gb_r_blob(__r, l_{n});")
            L.append(f"\t\tif (!blob_{n}) goto proto_error;")
            L.append(f"\t\tif (b_{n}) b_{n} = blob_{n}; /* else: unsized "
                     f"-> native gets NULL */")
            L.append("\t}")
            if mode == "raw":
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})b_{n};")
            else:
                et = "void *" if mode == "handle" else "intptr_t"
                L.append(f"\t{et} *t_{n} = NULL;")
                L.append(f"\tif (b_{n}) {{ t_{n} = gb_tmp(__t, "
                         f"(size_t)n_{n} * sizeof({et}) + 8);")
                if mode == "widen":
                    L.append(f"\t\tfor (int64_t i = 0; i < n_{n}; i++) "
                             f"t_{n}[i] = (intptr_t)((const int64_t *)"
                             f"b_{n})[i]; }}")
                else:
                    hk = HANDLES[k[1]]
                    L.append(f"\t\tfor (int64_t i = 0; i < n_{n}; i++) "
                             f"t_{n}[i] = (void *)gb_h2p(HK_{hk}, "
                             f"((const uint32_t *)b_{n})[i]); }}")
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})t_{n};")
        elif k[0] == "attrib":
            mode, wsz = k[2], k[3]
            L.append(f"\tconst void *b_{n} = NULL; int64_t n_{n} = 0;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tif (l_{n} == 0 || l_{n} % {wsz} || "
                     f"l_{n} > 65536) goto proto_error;")
            L.append(f"\t\tn_{n} = l_{n} / {wsz};")
            L.append(f"\t\tb_{n} = gb_r_blob(__r, l_{n});")
            L.append(f"\t\tif (!b_{n}) goto proto_error;")
            L.append(f"\t\tif (!gb_attrib_valid_{k[1]}(b_{n}, n_{n})) "
                     f"goto proto_error;")
            L.append("\t}")
            if mode == "raw":
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})b_{n};")
            else:
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})b_{n};"
                         f" /* 64-bit server: wire == native */")
        elif k[0] == "arr_out":
            mode, wsz = k[2], k[3]
            cexp = count_c(k[4], names)
            L.append(f"\tint64_t n_{n} = 0; void *o_{n} = NULL; int uns_{n} = 0;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tif (l_{n} > GB_MAX_ARRAY_BYTES) goto proto_error;")
            L.append(f"\t\tn_{n} = {cexp};")
            L.append(f"\t\tuns_{n} = n_{n} < 0; if (uns_{n}) n_{n} = 0;")
            L.append(f"\t\tif (n_{n} * {wsz} != (int64_t)l_{n}) "
                     f"goto proto_error;")
            L.append(f"\t\to_{n} = gb_tmp_zero(__t, (size_t)n_{n} * "
                     f"sizeof(void *) + 16);")
            L.append("\t}")
            L.append(f"\t{p['ctype']} a_{n} = uns_{n} ? NULL : "
                     f"({p['ctype']})o_{n};")

    if cmd.rk[0] != "void":
        L.append(f"\t{cmd.ret} ret = ({cmd.ret})0;")
    ptypes = ", ".join(p["ctype"] for p in P) or "void"
    args = ", ".join(f"a_{p['name']}" for p in P)
    L.append(f"\tGB_NATIVE({cmd.name}, {cmd.ret}, ({ptypes}));")
    L.append(f"\tif (fn_{cmd.name}) " +
             ("ret = " if cmd.rk[0] != "void" else "") +
             f"fn_{cmd.name}({args});")

    if cmd.api == "egl" and cmd.rk[0] in ("val", "handle"):
        L.append(f'\tGB_EGL_TRACE("{cmd.name}", (intptr_t)ret);')

    # post-call handle bookkeeping
    if cmd.name in cfg.HANDLE_FREE:
        hp, ok = cfg.HANDLE_FREE[cmd.name]
        hk = [p for p in P if p["name"] == hp][0]["k"][1]
        cond = "1" if ok is None else ok
        L.append(f"\tif ({cond}) gb_h_free(HK_{hk}, h_{hp});")

    if cmd.sync:
        L.append("\tif (!(__h->flags & GB_CMD_F_WANT_REPLY)) goto done;")
        L.append("\t{")
        # compute reply size
        L.append("\tuint32_t rs = 16;")
        if cmd.rk[0] != "void":
            L.append("\trs = GB_ALIGN_UP(rs, 8u) + 8;")
        outs = [p for p in P if p["k"][0] == "arr_out"]
        for p in outs:
            n = p["name"]
            L.append(f"\trs += 4;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) rs = GB_ALIGN_UP(rs, 8u)"
                     f" + l_{n};")
        L.append("\trs = GB_ALIGN_UP(rs, 8u);")
        L.append("\tstruct gb_out o; struct gb_w w;")
        L.append("\tif (gb_sv_reply_begin(__t, __h, &o, rs)) goto done;")
        L.append("\tgb_w_init(&w, o.p);")
        if cmd.rk[0] == "val":
            k = cmd.rk
            if k[1] == "f32":
                L.append("\tgb_w_f32(&w, ret); gb_w_u32(&w, 0);")
            elif k[1] == "f64":
                L.append("\tgb_w_f64(&w, ret);")
            else:
                L.append(f"\tgb_w_u64(&w, {w_expr(k[1] if k[1] in ('u64','i64') else ('i64' if k[1]=='i32' else 'u64'), 'ret')});")
        elif cmd.rk[0] == "handle":
            L.append(f"\tgb_w_u64(&w, (uint64_t)gb_p2h(HK_{cmd.rk[1]}, "
                     f"(void *)ret));")
        for p in outs:
            n = p["name"]
            k = p["k"]
            mode = k[2]
            L.append(f"\tgb_w_u32(&w, l_{n});")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tvoid *d = gb_w_blob(&w, l_{n});")
            if mode == "raw":
                L.append(f"\t\tmemcpy(d, o_{n}, l_{n});")
            elif mode == "widen":
                L.append(f"\t\tfor (int64_t i = 0; i < n_{n}; i++) "
                         f"((int64_t *)d)[i] = (int64_t)((intptr_t *)"
                         f"o_{n})[i];")
            else:
                hk = HANDLES[k[1]]
                L.append(f"\t\tfor (int64_t i = 0; i < n_{n}; i++) "
                         f"((uint32_t *)d)[i] = gb_p2h(HK_{hk}, "
                         f"((void **)o_{n})[i]);")
            L.append("\t}")
        L.append("\tgb_sv_reply_end(__t, &o);")
        L.append("\t}")
    L.append("done:")
    L.append("\treturn;")
    L.append("proto_error:")
    L.append("\tgb_sv_proto_error(__t, __h);")
    L.append("}")
    return "\n".join(L)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    report = "--report" in sys.argv
    regdir, outdir = Path(args[0]), Path(args[1])
    outdir.mkdir(parents=True, exist_ok=True)

    cmds, gl_root, egl_root = load_commands(regdir)
    want = []
    want += feature_commands(egl_root, "egl", cfg.EGL_VERSIONS)
    want += feature_commands(gl_root, "gles2", cfg.GLES_VERSIONS)
    want += extension_commands(egl_root, cfg.EGL_EXTENSIONS, ["egl"])
    want += extension_commands(gl_root, cfg.GLES_EXTENSIONS, ["gles2"])
    seen, order = set(), []
    for n in want:
        if n in cmds and n not in seen:
            seen.add(n)
            order.append(n)
    order.sort()

    for n in order:
        classify(cmds[n], None)

    if report:
        by = {}
        for n in order:
            c = cmds[n]
            by.setdefault(c.status, []).append(c)
        for st, lst in by.items():
            print(f"== {st}: {len(lst)}")
            if st in ("unsupported", "manual", "wrapped"):
                for c in lst:
                    print(f"   {c.name:42s} {c.reason}")
        return 0

    # opcode numbering: every command gets one (stable sorted order)
    ops = {n: i + 1 for i, n in enumerate(order)}
    h = zlib.crc32("\n".join(order).encode()) & 0xFFFFFFFF

    with open(outdir / "gb_ops.h", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                "#ifndef GB_OPS_H\n#define GB_OPS_H\n#include <stdint.h>\n")
        f.write(f"#define GB_OPS_HASH 0x{h:08X}u\n")
        f.write(f"#define GB_OP_COUNT {len(order) + 1}u\n")
        for n in order:
            f.write(f"#define GB_OP_{n} {ops[n]}u\n")
        f.write("#ifdef GB_OPS_NAMES\n"
                "static const char *const gb_op_names[GB_OP_COUNT] = {\n"
                '\t"(none)",\n')
        for n in order:
            f.write(f'\t"{n}",\n')
        f.write("};\n#endif\n#endif\n")

    # handle-kind enum (shared)
    with open(outdir / "gb_handle_kinds.h", "w") as f:
        f.write("/* generated */\n#ifndef GB_HANDLE_KINDS_H\n"
                "#define GB_HANDLE_KINDS_H\nenum gb_handle_kind {\n"
                "\tHK_NONE = 0,\n")
        for k in sorted(set(HANDLES.values())):
            f.write(f"\tHK_{k},\n")
        f.write("\tHK_COUNT\n};\n#endif\n")

    # client
    with open(outdir / "gb_client_gen.c", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                '#include "gb_client_impl.h"\n\n'
                "/* prototypes: extension entry points have none in the\n"
                " * stock Khronos headers */\n")
        for n in order:
            c = cmds[n]
            if c.status in ("generated", "wrapped", "manual"):
                pl = ", ".join(f"{p['ctype']} {p['name']}"
                               for p in c.params) or "void"
                f.write('__attribute__((visibility("default"))) '
                        f"{c.ret} {n}({pl});\n")
        f.write("\n")
        for n in order:
            c = cmds[n]
            op = f"GB_OP_{n}"
            if c.status == "generated":
                f.write(emit_client(c, True, op) + "\n\n")
            elif c.status == "wrapped":
                f.write(emit_client(c, False, op) + "\n\n")
            elif c.status == "unsupported":
                f.write(emit_client_stub(c, op) + "\n\n")
        # wrapped prototypes for hand-written wrappers
        f.write("/* proc-address table (implemented entry points only) */\n"
                "const struct gb_proc gb_proc_table[] = {\n")
        for n in order:
            if cmds[n].status != "unsupported":
                f.write(f'\t{{ "{n}", (void *){n} }},\n')
        f.write("\t{ 0, 0 }\n};\n\n")
        # extension -> commands, for filtering advertised extension strings
        f.write("const struct gb_ext gb_ext_table[] = {\n")
        for root, apis in ((gl_root, ("gles2",)), (egl_root, ("egl",))):
            for e in root.find("extensions").findall("extension"):
                sup = (e.get("supported") or "").split("|")
                if not any(a in sup for a in apis):
                    continue
                cl = []
                for r in e.findall("require"):
                    if r.get("api") and r.get("api") not in apis:
                        continue
                    cl += [c.get("name") for c in r.findall("command")]
                items = ", ".join(f'"{c}"' for c in cl)
                f.write(f'\t{{ "{e.get("name")}", {len(cl)}, '
                        f'(const char *const[]){{ {items}{", " if cl else ""}0 }} }},\n')
        f.write("\t{ 0, 0, 0 }\n};\n")

    with open(outdir / "gb_client_gen.h", "w") as f:
        f.write("/* generated */\n#ifndef GB_CLIENT_GEN_H\n"
                "#define GB_CLIENT_GEN_H\n")
        for n in order:
            c = cmds[n]
            if c.status == "wrapped":
                pl = ", ".join(f"{p['ctype']} {p['name']}"
                               for p in c.params) or "void"
                f.write(f"{c.ret} gbx_{n}({pl});\n")
        f.write("#endif\n")

    # server
    with open(outdir / "gb_server_gen.c", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                '#include "gb_server_impl.h"\n\n')
        gen = []
        for n in order:
            c = cmds[n]
            if c.status in ("generated", "wrapped"):
                f.write(emit_server(c, f"GB_OP_{n}") + "\n\n")
                gen.append(n)
        f.write("const gb_srv_fn gb_srv_table[GB_OP_COUNT] = {\n")
        for n in gen:
            f.write(f"\t[GB_OP_{n}] = srv_{n},\n")
        f.write("};\n")

    st = {}
    for n in order:
        st[cmds[n].status] = st.get(cmds[n].status, 0) + 1
    print(f"gb-gen: {len(order)} commands, ops hash 0x{h:08X}: {st}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

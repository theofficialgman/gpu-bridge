#!/usr/bin/env python3
"""
gb-gen: generate gpu-bridge encoders (client) and decoders (server) from the
Khronos registries.

Usage: gb_gen.py <registry-dir> <out-dir> [--report]

One server and one opcode space serve several client libraries (see LIBS in
gen_config.py): `gles` (EGL + GLES) and `gl` (GLX + desktop GL).

Principles (ARCHITECTURE.md section 5.3):
  * Never guess a length. A pointer parameter whose size we cannot derive
    from the registry (or from a rule in gen_config.py) makes the command
    UNSUPPORTED; it gets a stub that logs + returns an error and is listed by
    --report so a human decides.
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
    const_base = bool(toks) and toks[0] == "const"
    return base, depth, const_base


class Cmd:
    def __init__(self, name, ret, params, api):
        self.name = name
        self.ret = ret
        self.params = params
        self.api = api            # 'gl' | 'egl' | 'glx' (which registry)
        self.gen = False          # server decoder + client encoder possible
        self.reason = ""
        self.sync = False
        self.rk = ("void",)
        self.status = {}          # lib -> generated|wrapped|manual|unsupported


def load_registries(regdir):
    cmds, roots = {}, {}
    for key, fname in (("gl", "gl.xml"), ("egl", "egl.xml"),
                       ("glx", "glx.xml")):
        root = ET.parse(regdir / fname).getroot()
        roots[key] = root
        for c in root.find("commands").findall("command"):
            p = c.find("proto")
            name = p.find("name").text
            params = [dict(name=pe.find("name").text, ctype=c_type_of(pe),
                           len=pe.get("len"), group=pe.get("group"))
                      for pe in c.findall("param")]
            cmds[name] = Cmd(name, c_type_of(p), params, key)
    return cmds, roots


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


def extension_table(root, apis):
    """name -> [commands] for extensions supported on any of `apis`."""
    out = {}
    for e in root.find("extensions").findall("extension"):
        sup = (e.get("supported") or "").split("|")
        if not any(a in sup for a in apis):
            continue
        cl = []
        for r in e.findall("require"):
            if r.get("api") and r.get("api") not in apis:
                continue
            cl += [c.get("name") for c in r.findall("command")]
        out[e.get("name")] = cl
    return out


def select_ext(name, sel):
    names, prefixes = sel
    return name in names or any(name.startswith(p) for p in prefixes)


# --------------------------------------------------------------------------
# Classification
# --------------------------------------------------------------------------

VAL = {}
for t in ("GLenum", "GLbitfield", "GLuint", "GLubyte", "GLushort", "GLboolean",
          "GLhalf", "GLhalfNV", "GLchar", "GLcharARB", "EGLBoolean", "EGLenum",
          "unsigned int", "uint32_t", "GLhandleARB",
          # X11 / GLX XIDs: 32-bit on the wire (client long may be 32-bit)
          "EGLNativePixmapType", "EGLNativeWindowType", "EGLClientBuffer",
          "XID", "Window", "Pixmap", "Font", "Colormap", "GLXDrawable",
          "GLXWindow", "GLXPixmap", "GLXPbuffer", "GLXContextID",
          "GLXPbufferSGIX", "GLXVideoSourceSGIX", "GLXVideoDeviceNV"):
    VAL[t] = ("u32", 4)
for t in ("GLint", "GLsizei", "GLbyte", "GLshort", "GLfixed", "GLclampx",
          "EGLint", "int", "Bool", "Status", "int32_t",
          "EGLNativeFileDescriptorKHR"):
    VAL[t] = ("i32", 4)
for t in ("GLfloat", "GLclampf"):
    VAL[t] = ("f32", 4)
for t in ("GLdouble", "GLclampd"):
    VAL[t] = ("f64", 8)
for t in ("GLint64", "GLint64EXT", "GLintptr", "GLsizeiptr", "GLintptrARB",
          "GLsizeiptrARB", "EGLAttrib", "EGLAttribKHR", "EGLnsecsANDROID",
          "int64_t"):
    VAL[t] = ("i64", 8)
for t in ("GLuint64", "GLuint64EXT", "EGLuint64KHR", "EGLTime", "EGLTimeKHR",
          "uint64_t", "unsigned long", "EGLuint64NV"):
    VAL[t] = ("u64", 8)

HANDLES = {
    "EGLDisplay": "DISPLAY", "EGLConfig": "CONFIG", "EGLSurface": "SURFACE",
    "EGLContext": "CONTEXT", "EGLImage": "IMAGE", "EGLImageKHR": "IMAGE",
    "EGLSync": "SYNC", "EGLSyncKHR": "SYNC", "EGLStreamKHR": "STREAM",
    "EGLDeviceEXT": "DEVICE", "GLsync": "GLSYNC",
    "EGLOutputLayerEXT": "OUTLAYER", "EGLOutputPortEXT": "OUTPORT",
    "GLeglImageOES": "IMAGE",
    "GLXContext": "GLXCTX", "GLXFBConfig": "GLXFBC",
    "GLXFBConfigSGIX": "GLXFBC",
}

ELEM = {}
for t in ("GLchar", "GLcharARB", "GLbyte", "GLubyte", "GLboolean", "char",
          "void", "GLvoid"):
    ELEM[t] = ("raw", 1)
for t in ("GLshort", "GLushort", "GLhalf", "GLhalfNV"):
    ELEM[t] = ("raw", 2)
for t in ("GLint", "GLuint", "GLenum", "GLfloat", "GLsizei", "GLfixed",
          "GLbitfield", "EGLint", "EGLBoolean", "EGLenum", "int",
          "unsigned int", "int32_t", "uint32_t", "GLhandleARB", "GLclampf"):
    ELEM[t] = ("raw", 4)
for t in ("GLint64", "GLuint64", "GLdouble", "GLint64EXT", "GLuint64EXT",
          "int64_t", "uint64_t", "GLclampd", "EGLuint64KHR", "EGLuint64NV",
          "EGLTimeKHR", "EGLTime"):
    ELEM[t] = ("raw", 8)
for t in ("GLintptr", "GLsizeiptr", "EGLAttrib", "EGLAttribKHR", "GLintptrARB",
          "GLsizeiptrARB", "unsigned long"):
    ELEM[t] = ("widen", 8)
for t in HANDLES:
    ELEM[t] = ("handle", 4)

ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

PBO_UNPACK_RE = re.compile(cfg.PBO_UNPACK_REGEX)
PBO_PACK_RE = re.compile(cfg.PBO_PACK_REGEX)
QUERY_RE = re.compile(r"^glGetQueryObject(i|ui|i64|ui64)v(ARB|EXT)?$")


def pbo_kind(cmd, base, pname):
    """'pack' | 'unpack' | None for a pointer parameter."""
    n = cmd
    kind = None
    if QUERY_RE.match(n):
        # the result pointer is a buffer offset while GL_QUERY_BUFFER is bound
        return "query" if pname == "params" else None
    if PBO_PACK_RE.match(n) or n in cfg.PBO_PACK_EXTRA:
        kind = "pack"
    elif PBO_UNPACK_RE.match(n) or n in cfg.PBO_UNPACK_EXTRA:
        kind = "unpack"
    if not kind:
        return None
    if base in ("void", "GLvoid") or (n, pname) in cfg.PBO_TYPED_PARAMS:
        return kind
    return None


def pack_sparse_expr(cmd, pname_):
    """C expression: does this pixel-pack transfer leave gaps in the
    application's buffer (row length / skip / image height / row padding)?
    The driver then only writes the texels; everything else must keep the
    application's bytes, so the client sends them along. None: not a
    candidate (compressed images, server-sized data)."""
    names = {p["name"] for p in cmd.params}
    if not (PBO_PACK_RE.match(cmd.name) or cmd.name in cfg.PBO_PACK_EXTRA):
        return None
    if "Compressed" in cmd.name or "Polygon" in cmd.name or "PixelMap" in \
            cmd.name or "Histogram" in cmd.name or "Minmax" in cmd.name or \
            "ColorTable" in cmd.name or "Convolution" in cmd.name or \
            "Separable" in cmd.name:
        return None
    if {"format", "type"} <= names and {"width", "height"} <= names:
        d = "depth" if "depth" in names else "1"
        return (f"gb_cl_pack_sparse(__cx, (int64_t)width, (int64_t)height, "
                f"(int64_t){d}, format, type)")
    if {"format", "type"} <= names:
        return "gb_cl_pack_sparse(__cx, -1, -1, -1, format, type)"
    return None


def pname_class(name):
    for rx, cls in cfg.PNAME_CLASSES:
        if re.match(rx, name):
            return cls
    return "generic"


def parse_len(cmd, pname, expr, params):
    override = cfg.LEN_OVERRIDES.get((cmd, pname))
    if override:
        expr = override
    pnames = {p["name"] for p in params}
    if (expr is None or expr == "1") and "pname" in pnames and \
            pname_class(cmd) != "generic":
        # the registry gives no length, but the command is a known
        # pname-keyed getter/setter: its count follows the enum
        expr = "COMPSIZE(pname)"
    if expr is None:
        return None
    for entry in cfg.SERVER_SIZED:
        rx, key, cands = entry[:3]
        if len(entry) > 3 and entry[3] != pname:
            continue
        if re.match(rx, cmd):
            for cand in cands:
                if all(a in pnames for a in cand) and (
                        key not in ("pixels_tex", "ctex")
                        or "texture" not in pnames):
                    return ("server", key, tuple(cand))
    m = re.match(r"^COMPSIZE\((.*)\)$", expr)
    if m:
        args = tuple(a.strip() for a in m.group(1).split(",") if a.strip())
        key = cfg.COMPSIZE.get((cmd, pname)) or cfg.COMPSIZE.get(args)
        # these depend on server-held texture state: server-sized or nothing
        if key in ("pixels_tex", "ctex"):
            return None
        if key and all(a in pnames for a in args):
            return ("compsize", key, args)
        return None
    ids = ID_RE.findall(expr)
    # a length may only depend on by-value parameters (never on the contents
    # of another pointer, which the decoder does not have yet)
    valnames = {p["name"] for p in params if "*" not in p["ctype"]}
    if any(i not in valnames for i in ids):
        return None
    if not re.match(r"^[A-Za-z0-9_\s\*\+\-\(\)/]+$", expr):
        return None
    # division only by integer literals (never by a parameter)
    for m in re.finditer(r"/\s*([A-Za-z0-9_]+)", expr):
        if not m.group(1).isdigit() or int(m.group(1)) == 0:
            return None
    return ("expr", expr)


def classify(cmd):
    """Lib-independent: can this command be marshalled automatically?"""
    name = cmd.name
    if name in cfg.UNSUPPORTED:
        cmd.reason = cfg.UNSUPPORTED[name]
        return
    if name in cfg.ALL_MANUAL:
        cmd.reason = "hand-written"
        return

    rb, rd, _ = split_type(cmd.ret)
    if rd:
        if rb == "XVisualInfo" and rd == 1:
            cmd.rk = ("xvis",)
        else:
            cmd.reason = f"pointer return '{cmd.ret}'"
            return
    elif rb == "void":
        cmd.rk = ("void",)
    elif rb in HANDLES:
        cmd.rk = ("handle", HANDLES[rb])
    elif rb in VAL:
        cmd.rk = ("val",) + VAL[rb]
    else:
        cmd.reason = f"return type '{cmd.ret}'"
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
                cmd.reason = f"param {n}: type '{p['ctype']}'"
                return
            continue
        if depth == 2 and base in ("GLchar", "GLcharARB", "char") and const:
            ln = parse_len(name, n, p["len"], cmd.params)
            if ln and ln[0] == "expr":
                p["k"] = ("strarr", ln)
                continue
        if depth > 1:
            cmd.reason = f"param {n}: '{p['ctype']}'"
            return

        # --- one level of pointer ---
        if base == "Display":
            p["k"] = ("xdpy",)
            continue
        if base == "XVisualInfo":
            p["k"] = ("xvis",)
            continue
        if (name, n) in cfg.OFFSET_ONLY:
            p["k"] = ("offset",)
            continue
        is_in = const
        if cmd.api == "egl" and n in ("attrib_list", "attribs") and \
                base in ("EGLint", "EGLAttrib") and const:
            mode, wsz = ELEM[base]
            p["k"] = ("attrib", base, mode, wsz, "egl", "pairs")
            continue
        if cmd.api == "glx" and n in ("attribList", "attrib_list") and \
                base == "int":
            style = "visual" if name == "glXChooseVisual" else "pairs"
            p["k"] = ("attrib", "int", "raw", 4, "glx", style)
            continue
        if base in ("GLchar", "GLcharARB", "char") and is_in and \
                (name, n) not in cfg.LEN_OVERRIDES and \
                (p["len"] is None or
                 re.match(r"^COMPSIZE\(\s*(%s)?\s*\)$" % re.escape(n),
                          p["len"] or "")):
            p["k"] = ("str",)
            continue
        if base not in ELEM:
            cmd.reason = f"param {n}: element type '{p['ctype']}'"
            return
        mode, wsz = ELEM[base]
        ln = parse_len(name, n, p["len"], cmd.params)
        if ln is None:
            if p["len"] is None and base != "void":
                ln = ("expr", "1")
            else:
                cmd.reason = (f"param {n}: cannot size '{p['ctype']}' "
                              f"(len={p['len']})")
                return
        pbo = pbo_kind(name, base, n) if mode == "raw" else None
        p["k"] = ("arr_in" if is_in else "arr_out", base, mode, wsz, ln, pbo)

    if any(p["k"][0] == "xvis" for p in cmd.params) or cmd.rk[0] == "xvis":
        if not any(p["k"][0] == "xdpy" for p in cmd.params):
            cmd.reason = "XVisualInfo without a Display parameter"
            return
    cmd.gen = True
    cmd.sync = (cmd.rk[0] != "void" or
                any(p["k"][0] == "arr_out" for p in cmd.params) or
                name in cfg.FORCE_SYNC)


# --------------------------------------------------------------------------
# C emission helpers
# --------------------------------------------------------------------------

WIRE_W = {"u32": "gb_w_u32", "i32": "gb_w_u32", "f32": "gb_w_f32",
          "u64": "gb_w_u64", "i64": "gb_w_u64", "f64": "gb_w_f64"}
WIRE_R = {"u32": "gb_r_u32", "i32": "gb_r_u32", "f32": "gb_r_f32",
          "u64": "gb_r_u64", "i64": "gb_r_u64", "f64": "gb_r_f64"}
CTYPE = {"u32": "uint32_t", "i32": "int32_t", "f32": "float",
         "u64": "uint64_t", "i64": "int64_t", "f64": "double"}


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
    def __init__(self, start=16):
        self.off = start

    def u32(self):
        self.off = (self.off + 3) & ~3
        self.off += 4

    def u64(self):
        self.off = align8(self.off)
        self.off += 8


def count_c(cmd, ln, names):
    """C expression for the element count (int64; may be negative=unsized)."""
    kind = ln[0]
    if kind == "expr":
        e = ID_RE.sub(
            lambda m: f"((int64_t){names[m.group(0)]})"
            if m.group(0) in names else m.group(0), ln[1])
        return f"gb_capcount({e})"
    key, args = ln[1], ln[2]
    a = ", ".join(f"(int64_t){names[x]}" if key.startswith("map") else
                  names[x] for x in args)
    if key == "pname":
        cls = pname_class(cmd.name)
        return f"gb_capcount(GB_COUNT_PNAME(__op, GB_PNC_{cls}, {a}))"
    if key.startswith("pixel") or key == "bitmap":
        mode = "PACK" if (PBO_PACK_RE.match(cmd.name) or
                          cmd.name in cfg.PBO_PACK_EXTRA) else "UNPACK"
        return f"gb_capcount(GB_BYTES_{key}(GB_PM_{mode}, {a}))"
    return f"gb_capcount(GB_BYTES_{key}(__op, {a}))"


def dpy_param(cmd):
    for p in cmd.params:
        if p["k"][0] == "xdpy":
            return p["name"]
    return None


# --------------------------------------------------------------------------
# Client emission
# --------------------------------------------------------------------------


def emit_client(cmd, public, opname):
    P = cmd.params
    fn = cmd.name if public else "gbx_" + cmd.name
    ret_t = cmd.ret
    plist = ", ".join(f"{p['ctype']} {p['name']}" for p in P) or "void"
    L = []
    vis = '__attribute__((visibility("default"))) ' if public else ""
    L.append(f"{vis}{ret_t} {fn}({plist})")
    L.append("{")
    L.append(f"\tconst uint32_t __op = {opname};")
    L.append("\tstruct gb_cl *__cx = gb_cl_get();")
    if cmd.rk[0] == "void":
        L.append("\tif (!__cx) return;")
    else:
        L.append(f"\t{ret_t} __ret = ({ret_t})0;")
        L.append("\tif (!__cx) return __ret;")
    retstmt = "return;" if cmd.rk[0] == "void" else "return __ret;"
    if cmd.api == "gl":
        what = []
        if re.match(cfg.PERSIST_UP_REGEX, cmd.name):
            what.append("GB_PS_UP")
        if re.match(cfg.PERSIST_DOWN_REGEX, cmd.name):
            what.append("GB_PS_DOWN")
        if what:
            L.append("\tif (__builtin_expect(gb_cl_persist_pending(__cx), "
                     f"0)) gb_cl_persist_sync(__cx, {' | '.join(what)});")
    elif cmd.name in ("eglSwapBuffers", "eglSwapBuffersWithDamageKHR",
                      "eglSwapBuffersWithDamageEXT", "glXSwapBuffers"):
        L.append("\tif (__builtin_expect(gb_cl_persist_pending(__cx), 0)) "
                 "gb_cl_persist_sync(__cx, GB_PS_UP | GB_PS_DOWN);")

    names = {p["name"]: p["name"] for p in P}
    # The server uses its own X connection: make this app's pending X
    # requests (e.g. window creation) visible before naming the resource.
    xid_types = ("GLXDrawable", "GLXWindow", "GLXPixmap", "GLXPbuffer",
                 "Window", "Pixmap", "Font", "Colormap", "GLXContextID")
    xdpy = dpy_param(cmd)
    if xdpy and (any(p["base"] in xid_types for p in P if p["depth"] == 0)
                 or any(p["k"][0] == "xvis" for p in P)):
        L.append(f"\tgb_cl_xsync({xdpy});")
    lay = Layout()
    for p in P:
        k = p["k"]
        if k[0] == "val":
            (lay.u64 if k[2] == 8 else lay.u32)()
        elif k[0] == "offset":
            lay.u64()
        elif k[0] == "xvis":
            lay.u32()
            lay.u32()
        elif k[0] in ("handle", "xdpy", "str", "arr_in", "arr_out", "attrib",
                      "strarr"):
            lay.u32()
    fixed_end = lay.off

    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "xdpy":
            L.append(f"\tuint32_t __xd_{n} = gb_cl_xdpy_id({n});")
            L.append(f"\tif (!__xd_{n}) {{ gb_cl_set_error(__cx, __op); "
                     f"{retstmt} }}")
        elif k[0] == "xvis":
            L.append(f"\tuint32_t __xv_{n} = {n} ? (uint32_t){n}->visualid "
                     f": 0;")
            L.append(f"\tuint32_t __xs_{n} = {n} ? (uint32_t){n}->screen "
                     f": 0;")
        elif k[0] == "str":
            L.append(f"\tuint32_t __l_{n} = {n} ? (uint32_t)strlen({n}) + 1 "
                     f": GB_NULL_LEN;")
        elif k[0] == "strarr":
            cexp = count_c(cmd, k[1], names)
            L.append(f"\tint64_t __n_{n} = {cexp};")
            L.append(f"\tif (__n_{n} < 0) __n_{n} = 0;")
            L.append(f"\tuint32_t __l_{n} = GB_NULL_LEN;")
            L.append(f"\tif ({n}) {{ uint64_t __sum = 0;")
            # a NULL entry is sent as the empty string (the drivers treat
            # it as a name that does not exist)
            L.append(f"\t  for (int64_t __i = 0; __i < __n_{n}; __i++) "
                     f"__sum += {n}[__i] ? strlen({n}[__i]) + 1 : 1;")
            L.append(f"\t  if (__sum > GB_MAX_ARRAY_BYTES) "
                     f"{{ gb_cl_set_error(__cx, __op); {retstmt} }}")
            L.append(f"\t  __l_{n} = (uint32_t)__sum; }}")
        elif k[0] in ("arr_in", "arr_out") and k[4][0] == "server":
            pbo = k[5]
            if pbo:
                bound = {"pack": "GB_BUF_PACK", "unpack": "GB_BUF_UNPACK",
                         "query": "GB_BUF_QUERY"}[pbo]
                L.append(f"\tint __pbo_{n} = gb_cl_buf_bound(__cx, {bound});")
                L.append(f"\tuint32_t __l_{n} = __pbo_{n} ? GB_PBO_LEN : "
                         f"({n} ? GB_SRV_LEN : GB_NULL_LEN);")
            else:
                L.append(f"\tuint32_t __l_{n} = {n} ? GB_SRV_LEN : "
                         f"GB_NULL_LEN;")
        elif k[0] in ("arr_in", "arr_out"):
            pbo = k[5]
            cexp = count_c(cmd, k[4], names)
            L.append(f"\tint64_t __n_{n} = {cexp};")
            L.append(f"\tif (__n_{n} < 0) __n_{n} = 0; /* unsized */")
            if pbo:
                bound = {"pack": "GB_BUF_PACK", "unpack": "GB_BUF_UNPACK",
                         "query": "GB_BUF_QUERY"}[pbo]
                L.append(f"\tint __pbo_{n} = gb_cl_buf_bound(__cx, {bound});")
                L.append(f"\tuint32_t __l_{n} = __pbo_{n} ? GB_PBO_LEN : "
                         f"({n} ? (uint32_t)(__n_{n} * {k[3]}) : "
                         f"GB_NULL_LEN);")
                L.append(f"\tif (!__pbo_{n} && {n} && __n_{n} * {k[3]} > "
                         f"GB_MAX_ARRAY_BYTES) "
                         f"{{ gb_cl_set_error(__cx, __op); {retstmt} }}")
                if k[0] == "arr_out" and pbo == "pack" and k[2] == "raw":
                    sx = pack_sparse_expr(cmd, n)
                    if sx:
                        L.append(f"\tint __pf_{n} = (!__pbo_{n} && {n} && "
                                 f"__n_{n} > 0 && {sx});")
            else:
                L.append(f"\tuint32_t __l_{n} = {n} ? (uint32_t)(__n_{n} * "
                         f"{k[3]}) : GB_NULL_LEN;")
                # a count no buffer can hold: uniforms report
                # INVALID_OPERATION (count beyond the array), the rest
                # INVALID_VALUE
                gle = "0x0502" if re.match(
                    r"^gl(Program)?Uniform", cmd.name) else "0x0501"
                L.append(f"\tif ({n} && __n_{n} * {k[3]} > "
                         f"GB_MAX_ARRAY_BYTES) "
                         f"{{ gb_cl_set_error(__cx, __op); "
                         f"gb_cl_gl_error({gle}); {retstmt} }}")
        elif k[0] == "attrib":
            if k[4] == "egl":
                scan = f"gb_attrib_count_{k[1]}({n})"
            else:
                scan = f"gb_glx_attrib_count({n}, {1 if k[5] == 'visual' else 0})"
            L.append(f"\tint64_t __n_{n} = {n} ? {scan} : -1;")
            seterr = ("gb_cl_egl_error(0x3004); /* EGL_BAD_ATTRIBUTE */ "
                      if k[4] == "egl" else "")
            L.append(f"\tif ({n} && __n_{n} < 0) {{ {seterr}"
                     f"gb_cl_set_error(__cx, __op); {retstmt} }}")
            L.append(f"\tuint32_t __l_{n} = {n} ? (uint32_t)(__n_{n} * "
                     f"{k[3]}) : GB_NULL_LEN;")

    L.append(f"\tuint32_t __t = {fixed_end}u;")
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] in ("str", "attrib", "strarr"):
            L.append(f"\tif (__l_{n} != GB_NULL_LEN) __t = "
                     f"GB_ALIGN_UP(__t, 8u) + __l_{n};")
        elif k[0] == "arr_in":
            if k[5]:
                L.append(f"\tif (__l_{n} == GB_PBO_LEN) __t = "
                         f"GB_ALIGN_UP(__t, 8u) + 8u;")
                L.append(f"\telse if (__l_{n} != GB_NULL_LEN) __t = "
                         f"GB_ALIGN_UP(__t, 8u) + __l_{n};")
            else:
                L.append(f"\tif (__l_{n} != GB_NULL_LEN) __t = "
                         f"GB_ALIGN_UP(__t, 8u) + __l_{n};")
        elif k[0] == "arr_out" and k[5]:
            L.append(f"\tif (__l_{n} == GB_PBO_LEN) __t = "
                     f"GB_ALIGN_UP(__t, 8u) + 8u;")
            if (k[5] == "pack" and k[2] == "raw" and k[4][0] != "server"
                    and pack_sparse_expr(cmd, n)):
                L.append(f"\tif (__pf_{n}) __t = GB_ALIGN_UP(__t, 8u) + "
                         f"__l_{n};")
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
        elif k[0] == "offset":
            L.append(f"\tgb_w_u64(&__w, (uint64_t)(uintptr_t)({n}));")
        elif k[0] == "handle":
            L.append(f"\tgb_w_u32(&__w, (uint32_t)(uintptr_t)({n}));")
        elif k[0] == "xdpy":
            L.append(f"\tgb_w_u32(&__w, __xd_{n});")
        elif k[0] == "xvis":
            L.append(f"\tgb_w_u32(&__w, __xv_{n});")
            L.append(f"\tgb_w_u32(&__w, __xs_{n});")
        elif (k[0] == "arr_out" and k[5] == "pack" and k[2] == "raw"
              and k[4][0] != "server" and pack_sparse_expr(cmd, n)):
            # high bit pattern: the application's current bytes follow
            L.append(f"\tgb_w_u32(&__w, __l_{n} | (__pf_{n} ? "
                     f"GB_PREFILL_BIT : 0));")
        else:
            L.append(f"\tgb_w_u32(&__w, __l_{n});")
    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "str":
            L.append(f"\tif (__l_{n} != GB_NULL_LEN) memcpy(gb_w_blob(&__w, "
                     f"__l_{n}), {n}, __l_{n});")
        elif k[0] == "strarr":
            L.append(f"\tif (__l_{n} != GB_NULL_LEN) {{ char *__d = "
                     f"gb_w_blob(&__w, __l_{n});")
            L.append(f"\t  for (int64_t __i = 0; __i < __n_{n}; __i++) {{ "
                     f"size_t __k = {n}[__i] ? strlen({n}[__i]) + 1 : 1; "
                     f"if ({n}[__i]) memcpy(__d, {n}[__i], __k); "
                     f"else *__d = 0; __d += __k; }} }}")
        elif k[0] in ("arr_in", "attrib", "arr_out"):
            pbo = k[5] if k[0] != "attrib" else None
            if k[0] == "arr_out":
                if pbo:
                    L.append(f"\tif (__l_{n} == GB_PBO_LEN) {{ uint64_t "
                             f"__off = (uint64_t)(uintptr_t){n}; "
                             f"memcpy(gb_w_blob(&__w, 8), &__off, 8); }}")
                    if (pbo == "pack" and k[2] == "raw" and
                            k[4][0] != "server" and pack_sparse_expr(cmd, n)):
                        L.append(f"\tif (__pf_{n}) {{ void *__d = "
                                 f"gb_w_blob(&__w, __l_{n}); "
                                 f"gb_cl_copy_in(__d, {n}, __l_{n}); }}")
                continue
            mode, wsz = (k[2], k[3])
            if pbo:
                L.append(f"\tif (__l_{n} == GB_PBO_LEN) {{ uint64_t __off = "
                         f"(uint64_t)(uintptr_t){n}; "
                         f"memcpy(gb_w_blob(&__w, 8), &__off, 8); }}")
                L.append(f"\telse if (__l_{n} != GB_NULL_LEN) {{")
            else:
                L.append(f"\tif (__l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tvoid *__d = gb_w_blob(&__w, __l_{n});")
            if mode == "raw":
                L.append(f"\t\tgb_cl_copy_in(__d, {n}, __l_{n});")
            else:
                L.append(f"\t\tfor (int64_t __i = 0; __i < __n_{n}; __i++) {{")
                if mode == "widen":
                    L.append(f"\t\t\t((uint64_t *)__d)[__i] = (uint64_t)"
                             f"(int64_t){n}[__i];")
                else:
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
        elif cmd.rk[0] == "xvis":
            L.append("\t{ uint32_t __vid = gb_r_u32(&__r); "
                     "uint32_t __scr = gb_r_u32(&__r);")
            L.append(f"\t  __ret = __vid ? gb_cl_xvisual({dpy_param(cmd)}, "
                     f"__vid, __scr) : NULL; }}")
        for p in P:
            k = p["k"]
            n = p["name"]
            if k[0] != "arr_out":
                continue
            mode = k[2]
            L.append(f"\t{{ uint32_t __rl = gb_r_u32(&__r);")
            L.append(f"\t  if (__rl != GB_NULL_LEN && __rl != GB_PBO_LEN) {{")
            L.append(f"\t    const void *__s = gb_r_blob(&__r, __rl);")
            if k[4][0] == "server":
                L.append(f"\t    if (__s && {n} && __rl <= "
                         f"GB_MAX_ARRAY_BYTES) {{")
            else:
                L.append(f"\t    if (__s && {n} && __rl == __l_{n}) {{")
            if mode == "raw" and k[1] in ("GLchar", "GLcharARB", "char"):
                # A string out-parameter: the driver writes the characters
                # and the terminator only, never the whole bufSize (the
                # application's buffer may be exactly as long as the name).
                L.append(f"\t      {{ size_t __k = strnlen((const char *)"
                         f"__s, __rl); if (__k < __rl) __k++; "
                         f"memcpy({n}, __s, __k); }}")
            elif mode == "raw":
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


def emit_client_stub(cmd):
    plist = ", ".join(f"{p['ctype']} {p['name']}" for p in cmd.params) or "void"
    L = [f'__attribute__((visibility("default"))) {cmd.ret} '
         f'{cmd.name}({plist})', "{"]
    api = "egl" if cmd.api in ("egl",) else ("glx" if cmd.api == "glx"
                                              else "gl")
    L.append(f'\tgb_cl_unsupported_{api}("{cmd.name}");')
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


def emit_server(cmd, opname):
    P = cmd.params
    L = []
    L.append(f"void srv_{cmd.name}(struct gb_sth *__t, const struct "
             f"gb_cmd_hdr *__h, struct gb_r *__r)")
    L.append("{")
    L.append(f"\tconst uint32_t __op = {opname}; (void)__op;")
    names = {p["name"]: "a_" + p["name"] for p in P}
    dpy = dpy_param(cmd)

    # locals that need cleanup are declared before any goto
    for p in P:
        if p["k"][0] == "xvis":
            L.append(f"\tXVisualInfo *a_{p['name']} = NULL;")

    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "val":
            L.append(f"\t{CTYPE[k[1]]} v_{n} = "
                     f"{r_expr(k[1], WIRE_R[k[1]] + '(__r)')};")
            if (cmd.name, n) in cfg.FD_PARAMS:
                # the client sent a token; adopt the matching fd
                L.append(f"\t{p['ctype']} a_{n} = "
                         f"({p['ctype']})gb_sv_fd_take(__t, (uint32_t)v_{n});")
            else:
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})v_{n};")
        elif k[0] == "offset":
            L.append(f"\tuint64_t v_{n} = gb_r_u64(__r);")
            L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})(uintptr_t)v_{n};")
        elif k[0] == "handle":
            L.append(f"\tuint32_t h_{n} = gb_r_u32(__r);")
        elif k[0] == "xdpy":
            L.append(f"\tuint32_t h_{n} = gb_r_u32(__r);")
        elif k[0] == "xvis":
            L.append(f"\tuint32_t xv_{n} = gb_r_u32(__r);")
            L.append(f"\tuint32_t xs_{n} = gb_r_u32(__r);")
        else:
            L.append(f"\tuint32_t l_{n} = gb_r_u32(__r);")
            if (k[0] == "arr_out" and k[5] == "pack" and k[2] == "raw" and
                    k[4][0] != "server" and pack_sparse_expr(cmd, n)):
                L.append(f"\tint pf_{n} = 0;")
                L.append(f"\tif (l_{n} < 0x80000000u && "
                         f"(l_{n} & GB_PREFILL_BIT)) "
                         f"{{ pf_{n} = 1; l_{n} &= ~GB_PREFILL_BIT; }}")
    L.append("\tif (__r->err) goto proto_error;")

    for p in P:
        k = p["k"]
        n = p["name"]
        if k[0] == "handle":
            L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})gb_h2p(HK_{k[1]}, "
                     f"h_{n});")
        elif k[0] == "xdpy":
            L.append(f"\tDisplay *a_{n} = (Display *)gb_h2p(HK_XDPY, h_{n});")
            L.append(f"\tif (!a_{n}) goto proto_error;")
        elif k[0] == "xvis":
            L.append(f"\tif (xv_{n}) {{ a_{n} = gb_x_visual(a_{dpy}, xv_{n}, "
                     f"xs_{n}); if (!a_{n}) goto proto_error; }}")

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
            mode, wsz, pbo = k[2], k[3], k[5]
            cexp = count_c(cmd, k[4], names)
            L.append(f"\tconst void *b_{n} = NULL; int64_t n_{n} = 0; "
                     f"int uns_{n} = 0;")
            if pbo:
                L.append(f"\tif (l_{n} == GB_PBO_LEN) {{")
                L.append(f"\t\tconst uint64_t *o_ = gb_r_blob(__r, 8);")
                L.append(f"\t\tif (!o_) goto proto_error;")
                L.append(f"\t\tb_{n} = (const void *)(uintptr_t)*o_;")
                L.append("\t} else")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tn_{n} = {cexp};")
            L.append(f"\t\tif (n_{n} < 0) {{ n_{n} = 0; uns_{n} = 1; }} "
                     f"else b_{n} = (const void *)1; /* sized */")
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
        elif k[0] == "strarr":
            cexp = count_c(cmd, k[1], names)
            L.append(f"\tconst char **sa_{n} = NULL;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tint64_t n_ = {cexp}; if (n_ < 0) n_ = 0;")
            L.append(f"\t\tif (n_ > 65536 || l_{n} > GB_MAX_ARRAY_BYTES) "
                     f"goto proto_error;")
            L.append(f"\t\tconst char *blob_ = gb_r_blob(__r, l_{n});")
            L.append(f"\t\tif (!blob_ && l_{n}) goto proto_error;")
            L.append(f"\t\tsa_{n} = gb_tmp(__t, (size_t)n_ * sizeof(char *) "
                     f"+ 8);")
            L.append(f"\t\tuint32_t o_ = 0;")
            L.append(f"\t\tfor (int64_t i = 0; i < n_; i++) {{")
            L.append(f"\t\t\tconst char *e_ = memchr(blob_ + o_, 0, "
                     f"l_{n} - o_);")
            L.append(f"\t\t\tif (!e_) goto proto_error;")
            L.append(f"\t\t\tsa_{n}[i] = blob_ + o_; "
                     f"o_ = (uint32_t)(e_ - blob_) + 1;")
            L.append(f"\t\t}}")
            L.append(f"\t\tif (o_ != l_{n}) goto proto_error;")
            L.append("\t}")
            L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})sa_{n};")
        elif k[0] == "attrib":
            mode, wsz = k[2], k[3]
            L.append(f"\tconst void *b_{n} = NULL; int64_t n_{n} = 0;")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tif (l_{n} == 0 || l_{n} % {wsz} || "
                     f"l_{n} > 65536) goto proto_error;")
            L.append(f"\t\tn_{n} = l_{n} / {wsz};")
            L.append(f"\t\tb_{n} = gb_r_blob(__r, l_{n});")
            L.append(f"\t\tif (!b_{n}) goto proto_error;")
            if k[4] == "egl":
                L.append(f"\t\tif (!gb_attrib_valid_{k[1]}(b_{n}, n_{n})) "
                         f"goto proto_error;")
            else:
                L.append(f"\t\tif (!gb_glx_attrib_valid(b_{n}, n_{n}, "
                         f"{1 if k[5] == 'visual' else 0})) "
                         f"goto proto_error;")
            L.append("\t}")
            if cmd.name in cfg.FD_ATTRIB_CMDS:
                # fd-valued attributes arrive as tokens: swap in adopted fds
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})"
                         f"gb_sv_xlat_fd_attribs(__t, b_{n}, n_{n}, {wsz});")
            else:
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})b_{n};")
        elif k[0] == "arr_out" and k[4][0] == "server":
            mode, wsz, pbo = k[2], k[3], k[5]
            key, sargs = k[4][1], k[4][2]
            sa = ", ".join(names[a] for a in sargs)
            L.append(f"\tint64_t n_{n} = 0; void *o_{n} = NULL; "
                     f"int uns_{n} = 0; uint64_t off_{n} = 0; "
                     f"uint32_t lr_{n} = l_{n};")
            if pbo:
                L.append(f"\tif (l_{n} == GB_PBO_LEN) {{")
                L.append(f"\t\tconst uint64_t *o_ = gb_r_blob(__r, 8);")
                L.append(f"\t\tif (!o_) goto proto_error;")
                L.append(f"\t\toff_{n} = *o_;")
                L.append("\t} else")
            L.append(f"\tif (l_{n} == GB_SRV_LEN) {{")
            L.append(f"\t\tint64_t nb_ = GB_SZS_{key}(__t, __op, {wsz}, "
                     f"{sa});")
            L.append(f"\t\tif (nb_ < 0 || nb_ > GB_MAX_ARRAY_BYTES) "
                     f"{{ nb_ = 0; uns_{n} = 1; }}")
            L.append(f"\t\tlr_{n} = (uint32_t)nb_; n_{n} = nb_ / {wsz};")
            L.append(f"\t\to_{n} = gb_tmp_zero(__t, (size_t)nb_ + 512);")
            L.append("\t}")
            if pbo:
                L.append(f"\t{p['ctype']} a_{n} = (l_{n} == GB_PBO_LEN) ? "
                         f"({p['ctype']})(uintptr_t)off_{n} : "
                         f"({p['ctype']})o_{n};")
            else:
                L.append(f"\t{p['ctype']} a_{n} = ({p['ctype']})o_{n};")
        elif k[0] == "arr_out":
            mode, wsz, pbo = k[2], k[3], k[5]
            cexp = count_c(cmd, k[4], names)
            L.append(f"\tint64_t n_{n} = 0; void *o_{n} = NULL; "
                     f"int uns_{n} = 0; uint64_t off_{n} = 0; "
                     f"uint32_t lr_{n} = l_{n};")
            if pbo:
                L.append(f"\tif (l_{n} == GB_PBO_LEN) {{")
                L.append(f"\t\tconst uint64_t *o_ = gb_r_blob(__r, 8);")
                L.append(f"\t\tif (!o_) goto proto_error;")
                L.append(f"\t\toff_{n} = *o_;")
                L.append("\t} else")
            L.append(f"\tif (l_{n} != GB_NULL_LEN) {{")
            L.append(f"\t\tif (l_{n} > GB_MAX_ARRAY_BYTES) goto proto_error;")
            L.append(f"\t\tn_{n} = {cexp};")
            L.append(f"\t\tuns_{n} = n_{n} < 0; if (uns_{n}) n_{n} = 0;")
            L.append(f"\t\tif (n_{n} * {wsz} != (int64_t)l_{n}) "
                     f"goto proto_error;")
            # +512: a wrong (too small) size rule must not corrupt the heap.
            # Only widened arrays (32-bit wire elements, 64-bit native) need
            # more than the wire size per element; sizing raw arrays at 8x
            # made every large glReadPixels allocate and zero 8x the image.
            esz = 8 if mode == "widen" else wsz
            L.append(f"\t\to_{n} = gb_tmp_zero(__t, (size_t)n_{n} * "
                     f"{esz} + 512);")
            if (pbo == "pack" and mode == "raw" and
                    pack_sparse_expr(cmd, n)):
                L.append(f"\t\tif (pf_{n}) {{ const void *pb_ = "
                         f"gb_r_blob(__r, l_{n}); if (!pb_) goto "
                         f"proto_error; memcpy(o_{n}, pb_, l_{n}); }}")
            L.append("\t}")
            if pbo:
                L.append(f"\t{p['ctype']} a_{n} = (l_{n} == GB_PBO_LEN) ? "
                         f"({p['ctype']})(uintptr_t)off_{n} : "
                         f"(uns_{n} ? NULL : ({p['ctype']})o_{n});")
            else:
                L.append(f"\t{p['ctype']} a_{n} = uns_{n} ? NULL : "
                         f"({p['ctype']})o_{n};")

    if cmd.rk[0] != "void":
        L.append(f"\t{cmd.ret} ret = ({cmd.ret})0;")
    ptypes = ", ".join(p["ctype"] for p in P) or "void"
    args = ", ".join(f"a_{p['name']}" for p in P)
    L.append(f"\tGB_NATIVE({cmd.name}, {cmd.ret}, ({ptypes}));")
    # An array we could not size must not reach the driver as NULL (the
    # driver dereferences it): skip the call and raise INVALID_VALUE.
    uns = [f"uns_{p['name']}" for p in P
           if p["k"][0] in ("arr_in", "arr_out")]
    # A sync name the server never issued is INVALID_VALUE (a NULL handle
    # would be silently ignored by the driver)
    if not cmd.name.startswith("glIsSync"):
        uns += [f"(h_{p['name']} && !a_{p['name']})" for p in P
                if p["k"][0] == "handle" and p["k"][1] == "GLSYNC"]
    L.append(f"\tint __skip = {' || '.join(uns) if uns else '0'};")
    # Format/type problems are enum errors; anything else a value error.
    enum_err = any(p["k"][0] == "arr_out" and p["k"][4][0] == "server" and
                   p["k"][4][1] in ("pixels_tex", "ctex") for p in P)
    # A uniform that cannot be read (bad location, unlinked program) is
    # INVALID_OPERATION, not INVALID_VALUE.
    op_err = any(p["k"][0] == "arr_out" and p["k"][4][0] == "server" and
                 p["k"][4][1] == "uniform" for p in P)
    L.append("\tif (__skip) gb_sv_raise_%s();" %
             ("enum" if enum_err else "operation" if op_err else "invalid"))
    # A getter that fails leaves the application's output untouched: find
    # out whether *this* call raised an error (preserving any error that was
    # already pending, which glGetError would otherwise consume) and, if so,
    # send no data back.
    outs_ = [p for p in P if p["k"][0] == "arr_out"]
    chk = (cmd.api == "gl" and cmd.sync and outs_ and
           cmd.name not in ("glGetError", "glGetString", "glGetStringi"))
    if chk:
        L.append("\tstruct gb_errs __e0, __e1;")
        L.append("\tint __chk = (__h->flags & GB_CMD_F_WANT_REPLY) && "
                 "!__skip;")
        L.append("\tif (__chk) gb_sv_err_pop(&__e0);")
    L.append("\tuint64_t __t0 = gb_stat_now();")
    L.append(f"\tif (fn_{cmd.name} && !__skip) " +
             ("ret = " if cmd.rk[0] != "void" else "") +
             f"fn_{cmd.name}({args});")
    L.append("\tgb_stat_native(__op, __t0);")
    if chk:
        L.append("\tif (__chk) { gb_sv_err_pop(&__e1); "
                 "gb_sv_err_raise(&__e0); gb_sv_err_raise(&__e1); }")
        for p in outs_:
            L.append(f"\tif (__skip || (__chk && __e1.n)) "
                     f"lr_{p['name']} = GB_NULL_LEN;")
    # EGL: a call that returns EGL_FALSE leaves the application's outputs
    # untouched
    if (cmd.api == "egl" and cmd.sync and cmd.ret.strip() == "EGLBoolean"
            and any(p["k"][0] == "arr_out" for p in P)):
        for p in P:
            if p["k"][0] == "arr_out":
                L.append(f"\tif (!ret) lr_{p['name']} = GB_NULL_LEN;")
    # descriptors the driver took ownership of (on success) stay open
    for (cn, pn) in cfg.FD_PARAMS:
        if cn == cmd.name:
            L.append(f"\tif (a_{pn} >= 0) gb_sv_fd_keep(__t, a_{pn});")
    if cfg.FD_ATTRIB_CMDS.get(cmd.name):
        L.append("\tif (ret) gb_sv_fd_keep_all(__t);")
    if cmd.name in cfg.CTX_CHANGERS:
        L.append("\t__t->ctx_valid = 0; /* current context may have changed */")

    if cmd.api == "egl" and cmd.rk[0] in ("val", "handle"):
        L.append(f'\tGB_EGL_TRACE("{cmd.name}", (intptr_t)ret);')

    if cmd.name in cfg.HANDLE_FREE:
        hp, ok = cfg.HANDLE_FREE[cmd.name]
        hk = [p for p in P if p["name"] == hp][0]["k"][1]
        cond = "1" if ok is None else ok
        L.append(f"\tif ({cond}) gb_h_free(HK_{hk}, h_{hp});")

    # release native XVisualInfo structures
    cleanup = [p["name"] for p in P if p["k"][0] == "xvis"]

    if cmd.sync:
        L.append("\tif (!(__h->flags & GB_CMD_F_WANT_REPLY)) goto done;")
        L.append("\t{")
        L.append("\tuint32_t rs = 16;")
        if cmd.rk[0] != "void":
            if cmd.rk[0] == "xvis":
                L.append("\trs += 8;")
            else:
                L.append("\trs = GB_ALIGN_UP(rs, 8u) + 8;")
        outs = [p for p in P if p["k"][0] == "arr_out"]
        for p in outs:
            n = p["name"]
            L.append("\trs += 4;")
            L.append(f"\tif (lr_{n} != GB_NULL_LEN && lr_{n} != GB_PBO_LEN "
                     f"&& lr_{n} != GB_SRV_LEN) "
                     f"rs = GB_ALIGN_UP(rs, 8u) + lr_{n};")
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
                wk = k[1] if k[1] in ("u64", "i64") else (
                    "i64" if k[1] == "i32" else "u64")
                L.append(f"\tgb_w_u64(&w, {w_expr(wk, 'ret')});")
        elif cmd.rk[0] == "handle":
            L.append(f"\tgb_w_u64(&w, (uint64_t)gb_p2h(HK_{cmd.rk[1]}, "
                     f"(void *)ret));")
        elif cmd.rk[0] == "xvis":
            L.append("\tgb_w_u32(&w, ret ? (uint32_t)ret->visualid : 0);")
            L.append("\tgb_w_u32(&w, ret ? (uint32_t)ret->screen : 0);")
            L.append("\tif (ret) gb_x_free(ret);")
        for p in outs:
            n = p["name"]
            k = p["k"]
            mode = k[2]
            L.append(f"\tgb_w_u32(&w, lr_{n} == GB_SRV_LEN ? 0 : lr_{n});")
            L.append(f"\tif (lr_{n} != GB_NULL_LEN && lr_{n} != GB_PBO_LEN "
                     f"&& lr_{n} != GB_SRV_LEN) {{")
            L.append(f"\t\tvoid *d = gb_w_blob(&w, lr_{n});")
            if mode == "raw":
                L.append(f"\t\tmemcpy(d, o_{n}, lr_{n});")
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
    elif cmd.rk[0] == "xvis":
        L.append("\tif (ret) gb_x_free(ret);")
    L.append("done:")
    for n in cleanup:
        L.append(f"\tif (a_{n}) gb_x_free(a_{n});")
    L.append("\treturn;")
    L.append("proto_error:")
    for n in cleanup:
        L.append(f"\tif (a_{n}) gb_x_free(a_{n});")
    L.append("\tgb_sv_proto_error(__t, __h);")
    L.append("}")
    return "\n".join(L)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------


def lib_command_names(lib, roots):
    spec = cfg.LIBS[lib]
    names = []
    for rootkey, api, versions in spec["features"]:
        names += feature_commands(roots[rootkey], api, versions)
    exts = {}
    for rootkey, apis, sel in spec["extensions"]:
        for ename, cl in extension_table(roots[rootkey], apis).items():
            if select_ext(ename, sel):
                names += cl
                exts[ename] = cl
    return names, exts


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    report = "--report" in sys.argv
    regdir, outdir = Path(args[0]), Path(args[1])
    outdir.mkdir(parents=True, exist_ok=True)

    cmds, roots = load_registries(regdir)

    lib_cmds = {}
    for lib in cfg.LIBS:
        names, _ = lib_command_names(lib, roots)
        seen, order = set(), []
        for n in names:
            if n in cmds and n not in seen:
                seen.add(n)
                order.append(n)
        lib_cmds[lib] = sorted(order)

    union = sorted(set(n for v in lib_cmds.values() for n in v))
    for n in union:
        classify(cmds[n])

    for lib, order in lib_cmds.items():
        manual = cfg.MANUAL[lib]
        wrapped = cfg.WRAPPED[lib]
        for n in order:
            c = cmds[n]
            if n in cfg.ALIASES and cfg.ALIASES[n] in cmds:
                c.status[lib] = "alias"
            elif n in manual:
                c.status[lib] = "manual"
            elif not c.gen:
                c.status[lib] = "unsupported"
            elif n in wrapped:
                c.status[lib] = "wrapped"
            else:
                c.status[lib] = "generated"

    if report:
        for lib, order in lib_cmds.items():
            cnt = {}
            for n in order:
                cnt.setdefault(cmds[n].status[lib], []).append(cmds[n])
            print(f"##### lib {lib}: {len(order)} commands")
            for st, lst in sorted(cnt.items()):
                print(f"== {st}: {len(lst)}")
                if st in ("unsupported", "manual", "wrapped"):
                    for c in lst:
                        print(f"   {c.name:44s} {c.reason}")
        return 0

    ops = {n: i + 1 for i, n in enumerate(union)}
    h = zlib.crc32("\n".join(union).encode()) & 0xFFFFFFFF

    with open(outdir / "gb_ops.h", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                "#ifndef GB_OPS_H\n#define GB_OPS_H\n#include <stdint.h>\n")
        f.write(f"#define GB_OPS_HASH 0x{h:08X}u\n")
        f.write(f"#define GB_OP_COUNT {len(union) + 1}u\n")
        for n in union:
            f.write(f"#define GB_OP_{n} {ops[n]}u\n")
        f.write("#ifdef GB_OPS_NAMES\n"
                "static const char *const gb_op_names[GB_OP_COUNT] = {\n"
                '\t"(none)",\n')
        for n in union:
            f.write(f'\t"{n}",\n')
        f.write("};\n#endif\n#endif\n")

    # Which ES version made each function core: an ES context older than that
    # only offers it under an extension suffix (see gb_native_sym).
    intro = {}
    for f in roots["gl"].findall("feature"):
        if f.get("api") != "gles2":
            continue
        num = f.get("number") or ""
        if num in ("2.0", "") or "." not in num:
            continue
        ver = int(num.split(".")[0]) * 10 + int(num.split(".")[1])
        for r in f.findall("require"):
            for c in r.findall("command"):
                intro.setdefault(c.get("name"), ver)
    with open(outdir / "gb_es_intro.h", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                "#ifndef GB_ES_INTRO_H\n#define GB_ES_INTRO_H\n"
                "static const struct { const char *name; int ver; } "
                "gb_es_intro[] = {\n")
        for n, v in sorted(intro.items()):
            f.write(f'\t{{ "{n}", {v} }},\n')
        f.write("\t{ 0, 0 }\n};\n#endif\n")

    kinds = sorted(set(HANDLES.values()) | {"XDPY"})
    with open(outdir / "gb_handle_kinds.h", "w") as f:
        f.write("/* generated */\n#ifndef GB_HANDLE_KINDS_H\n"
                "#define GB_HANDLE_KINDS_H\nenum gb_handle_kind {\n"
                "\tHK_NONE = 0,\n")
        for k in kinds:
            f.write(f"\tHK_{k},\n")
        f.write("\tHK_COUNT\n};\n#endif\n")

    # pname classes (shared enum used by generated size calls)
    classes = sorted(set(c for _, c in cfg.PNAME_CLASSES) | {"generic"})
    with open(outdir / "gb_pname_classes.h", "w") as f:
        f.write("/* generated */\n#ifndef GB_PNAME_CLASSES_H\n"
                "#define GB_PNAME_CLASSES_H\nenum gb_pname_class {\n")
        for c in classes:
            f.write(f"\tGB_PNC_{c},\n")
        f.write("};\n#endif\n")

    # The generated sources are split into NCHUNKS translation units: the
    # compiler's cost for one unit grows much faster than its size, and one
    # huge unit also cannot use more than one core.
    nch = cfg.GEN_CHUNKS

    def chunk_of(i):
        return i % nch

    # per-library client sources
    for lib, order in lib_cmds.items():
        names, exts = lib_command_names(lib, roots)
        with open(outdir / f"gb_client_{lib}_protos.h", "w") as f:
            f.write("/* generated by gb_gen.py - do not edit */\n"
                    "#ifndef GB_CLIENT_PROTOS_H\n#define GB_CLIENT_PROTOS_H\n"
                    "/* prototypes: many entry points have none in the\n"
                    " * stock Khronos headers */\n")
            for n in order:
                c = cmds[n]
                if c.status[lib] in ("generated", "wrapped", "manual",
                                     "alias"):
                    pl = ", ".join(f"{p['ctype']} {p['name']}"
                                   for p in c.params) or "void"
                    f.write('__attribute__((visibility("default"))) '
                            f"{c.ret} {n}({pl});\n")
            f.write("#endif\n")
        chunks = [open(outdir / f"gb_client_{lib}_gen_{k:02d}.c", "w")
                  for k in range(nch)]
        for f in chunks:
            f.write("/* generated by gb_gen.py - do not edit */\n"
                    '#include "gb_client_impl.h"\n'
                    f'#include "gb_client_{lib}_protos.h"\n\n')
        for idx, n in enumerate(order):
            c = cmds[n]
            op = f"GB_OP_{n}"
            st = c.status[lib]
            f = chunks[chunk_of(idx)]
            if st == "generated":
                f.write(emit_client(c, True, op) + "\n\n")
            elif st == "wrapped":
                f.write(emit_client(c, False, op) + "\n\n")
            elif st == "alias":
                args = ", ".join(p["name"] for p in c.params)
                pl = ", ".join(f"{p['ctype']} {p['name']}"
                               for p in c.params) or "void"
                core = cfg.ALIASES[n]
                ret = "" if c.ret.strip() == "void" else "return "
                f.write('__attribute__((visibility("default"))) '
                        f"{c.ret} {n}({pl})\n{{\n\t{ret}{core}({args});"
                        "\n}\n\n")
            elif st == "unsupported":
                f.write(emit_client_stub(c) + "\n\n")
        for f in chunks:
            f.close()
        with open(outdir / f"gb_client_{lib}_gen_tab.c", "w") as f:
            f.write("/* generated by gb_gen.py - do not edit */\n"
                    '#include "gb_client_impl.h"\n'
                    f'#include "gb_client_{lib}_protos.h"\n\n')
            for n in order:
                c = cmds[n]
                if c.status[lib] == "unsupported":
                    pl = ", ".join(f"{p['ctype']} {p['name']}"
                                   for p in c.params) or "void"
                    f.write('__attribute__((visibility("default"))) '
                            f"{c.ret} {n}({pl});\n")
            for n in sorted(cfg.EXTRA_PROCS):
                f.write(f"extern void {n}();\n")
            f.write("const struct gb_proc gb_proc_table[] = {\n")
            for n in order:
                if cmds[n].status[lib] != "unsupported":
                    f.write(f'\t{{ "{n}", (void *){n} }},\n')
            for n in sorted(cfg.EXTRA_PROCS):
                f.write(f'\t{{ "{n}", (void *){n} }},\n')
            f.write("\t{ 0, 0 }\n};\n\n")
            f.write("const struct gb_ext gb_ext_table[] = {\n")
            spec = cfg.LIBS[lib]
            for rootkey, apis, _sel in spec["ext_filter"]:
                for ename, cl in extension_table(roots[rootkey], apis).items():
                    items = ", ".join(f'"{c}"' for c in cl)
                    f.write(f'\t{{ "{ename}", {len(cl)}, (const char *const[])'
                            f'{{ {items}{", " if cl else ""}0 }} }},\n')
            for ename, cl in sorted(cfg.EXT_EXTRA.items()):
                items = ", ".join(f'"{c}"' for c in cl)
                f.write(f'\t{{ "{ename}", {len(cl)}, (const char *const[])'
                        f'{{ {items}, 0 }} }},\n')
            for ename in sorted(cfg.EXT_NO_COMMANDS):
                f.write(f'\t{{ "{ename}", 0, (const char *const[]){{ 0 }} }},\n')
            f.write("\t{ 0, 0, 0 }\n};\n")

        with open(outdir / f"gb_client_{lib}_gen.h", "w") as f:
            f.write("/* generated */\n#ifndef GB_CLIENT_GEN_H\n"
                    "#define GB_CLIENT_GEN_H\n")
            for n in order:
                c = cmds[n]
                if c.status[lib] == "wrapped":
                    pl = ", ".join(f"{p['ctype']} {p['name']}"
                                   for p in c.params) or "void"
                    f.write(f"{c.ret} gbx_{n}({pl});\n")
            f.write("#endif\n")

    # server: decoders for every generable command some library uses
    gen = []
    for n in union:
        c = cmds[n]
        if c.gen and any(c.status[l] in ("generated", "wrapped")
                         for l in c.status):
            gen.append(n)
    with open(outdir / "gb_server_gen.h", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                "#ifndef GB_SERVER_GEN_H\n#define GB_SERVER_GEN_H\n"
                '#include "gb_server_impl.h"\n')
        for n in gen:
            f.write(f"void srv_{n}(struct gb_sth *, const struct "
                    "gb_cmd_hdr *, struct gb_r *);\n")
        f.write("#endif\n")
    chunks = [open(outdir / f"gb_server_gen_{k:02d}.c", "w")
              for k in range(nch)]
    for f in chunks:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                '#include "gb_server_impl.h"\n\n')
    for idx, n in enumerate(gen):
        chunks[chunk_of(idx)].write(emit_server(cmds[n], f"GB_OP_{n}") +
                                    "\n\n")
    for f in chunks:
        f.close()
    with open(outdir / "gb_server_gen_tab.c", "w") as f:
        f.write("/* generated by gb_gen.py - do not edit */\n"
                '#include "gb_server_gen.h"\n\n')
        f.write("const gb_srv_fn gb_srv_table[GB_OP_COUNT] = {\n")
        for n in gen:
            f.write(f"\t[GB_OP_{n}] = srv_{n},\n")
        f.write("};\n")

    for lib, order in lib_cmds.items():
        st = {}
        for n in order:
            st[cmds[n].status[lib]] = st.get(cmds[n].status[lib], 0) + 1
        print(f"gb-gen[{lib}]: {len(order)} commands {st}")
    print(f"gb-gen: ops hash 0x{h:08X}, {len(union)} opcodes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/bin/bash
# SPDX-License-Identifier: MIT
# build-deb.sh ARMHF_BUILD_DIR ARM64_BUILD_DIR [OUTDIR] : build the gpu-bridge Debian package
#   one arm64 package that carries
#     /usr/libexec/gpu-bridge/gbd                    the 64-bit server (arm64)
#     /usr/lib/arm-linux-gnueabihf/libGL.so.1.7.0    the 32-bit client library (armhf), plus symlinks
#                                                    libGL.so.1 libEGL.so.1 libGLESv2.so.2 libGLESv1_CM.so.1
#                                                    libGLX.so.0 libOpenGL.so.0, replacing libglvnd's
# (an arm64 package shipping files into the armhf library directory is not multiarch-clean, but it is what
# this device needs: one thing to install, one thing to remove). The libglvnd files it replaces are kept as dpkg
# diversions and come back when the package is removed.
# ARMHF_BUILD_DIR holds shim32/ (the 32-bit client library), ARM64_BUILD_DIR holds gbd.
# Run it where dpkg-deb is (the Tegra builds both trees). See packaging/README.md.
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$(dirname "$here")
mkdir -p "${3:-.}"
b32=$(cd "$1" && pwd); b64=$(cd "$2" && pwd); out=$(cd "${3:-.}" && pwd)
[ -f "$b32/shim32/libGL.so.1.7.0" ] && [ -x "$b64/gbd" ] || { echo "need $b32/shim32/libGL.so.1.7.0 and $b64/gbd" >&2; exit 2; }
ver=${GB_VERSION:-0.1.0~git$(date +%Y%m%d).$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo local)}
maint="theofficialgman <dofficialgman@gmail.com>"
L=usr/lib/arm-linux-gnueabihf
# the names the client library answers to, and the files libglvnd (armhf) owns under them today
SONAMES="libGL.so.1 libEGL.so.1 libGLESv2.so.2 libGLESv1_CM.so.1 libGLX.so.0 libOpenGL.so.0"
DIVERT="libGL.so.1.7.0 $SONAMES"
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
p="$w/pkg"; install -d "$p/DEBIAN" "$p/usr/libexec/gpu-bridge" "$p/$L"

install -m 755 "$b64/gbd" "$p/usr/libexec/gpu-bridge/gbd"
${STRIP64:-strip} --strip-unneeded "$p/usr/libexec/gpu-bridge/gbd" 2>/dev/null || true
install -m 755 "$b32/shim32/libGL.so.1.7.0" "$p/$L/libGL.so.1.7.0"
${STRIP32:-arm-linux-gnueabihf-strip} --strip-unneeded "$p/$L/libGL.so.1.7.0" 2>/dev/null || true
for n in $SONAMES; do ln -s libGL.so.1.7.0 "$p/$L/$n"; done

install -d "$p/usr/share/doc/gpu-bridge"
install -m 644 "$root/LICENSE" "$p/usr/share/doc/gpu-bridge/LICENSE"
install -m 644 "$root/THIRD_PARTY_NOTICES.md" "$p/usr/share/doc/gpu-bridge/THIRD_PARTY_NOTICES.md"
cat > "$p/usr/share/doc/gpu-bridge/copyright" <<C
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: gpu-bridge
Source: https://github.com/theofficialgman/32bit-to-64bit-gpu-translation

Files: *
Copyright: 2026 theofficialgman
License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a copy of this
 software and associated documentation files (the "Software"), to deal in the Software
 without restriction, including without limitation the rights to use, copy, modify,
 merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to the following
 conditions:
 .
 The above copyright notice and this permission notice shall be included in all copies
 or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 DEALINGS IN THE SOFTWARE.

Comment: The code generator reads the Khronos API registry (Apache-2.0); see
 THIRD_PARTY_NOTICES.md. No registry file is shipped in this package.
C

chmod 644 "$p/usr/share/doc/gpu-bridge/copyright"
echo "activate-noawait ldconfig" > "$p/DEBIAN/triggers"
cat > "$p/DEBIAN/control" <<C
Package: gpu-bridge
Version: $ver
Architecture: arm64
Maintainer: $maint
Installed-Size: $(du -sk "$p" | cut -f1)
Section: libs
Priority: optional
Depends: libc6 (>= 2.34), libgl1, libegl1
Description: run 32-bit OpenGL, GLES, EGL and GLX programs on the 64-bit GPU driver
 A 32-bit library installed as libGL.so.1, libEGL.so.1, libGLESv2.so.2,
 libGLESv1_CM.so.1, libGLX.so.0 and libOpenGL.so.0 (in place of libglvnd's) sends the
 calls of 32-bit programs to gbd, a 64-bit server that runs them on the system's
 own driver. gbd is started by the library on demand; nothing runs unless a 32-bit
 program uses it. Only the armhf libglvnd files are replaced, kept as dpkg diversions
 (in gpu-bridge-orig/) and restored when this package is removed; 64-bit programs are
 not affected.
C
cat > "$p/DEBIAN/preinst" <<C
#!/bin/sh
# Divert libglvnd's armhf libraries: the SONAME links and also the versioned files they
# point to (libEGL.so.1.1.0, ...), otherwise ldconfig re-points the SONAME links at them.
set -e
L=/$L
# kept in a subdirectory: ldconfig reads every *.so* file in \$L and would prefer these
O=\$L/gpu-bridge-orig
case "\$1" in
install|upgrade)
	mkdir -p "\$O"
	real=
	for f in $SONAMES; do
		t=\$(readlink -f "\$L/\$f" 2>/dev/null) || t=
		case "\$t" in "\$L"/*) real="\$real \${t##*/}" ;; esac
	done
	for f in $DIVERT \$real; do
		[ -e "\$L/\$f" ] || [ -L "\$L/\$f" ] || continue
		dpkg-divert --package gpu-bridge --divert "\$O/\$f" --rename --add "\$L/\$f"
	done ;;
esac
exit 0
C
cat > "$p/DEBIAN/postrm" <<C
#!/bin/sh
set -e
L=/$L
case "\$1" in
remove|abort-install|abort-upgrade)
	dpkg-divert --list "\$L/*" | sed -n 's/^diversion of \(.*\) to .* by gpu-bridge\$/\1/p' |
	while read -r f; do
		dpkg-divert --package gpu-bridge --rename --remove "\$f"
	done
	rmdir "\$L/gpu-bridge-orig" 2>/dev/null || true ;;
esac
exit 0
C
chmod 755 "$p/DEBIAN/preinst" "$p/DEBIAN/postrm"
dpkg-deb --root-owner-group -Zxz --build "$p" "$out/gpu-bridge_${ver}_arm64.deb" >/dev/null
ls -l "$out/gpu-bridge_${ver}_arm64.deb"

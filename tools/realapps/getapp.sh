#!/bin/bash
# SPDX-License-Identifier: MIT
# getapp.sh pkg...: download the armhf packages (and the dependencies not already installed) and unpack them
# under ~/gb/apps2/root without touching the system.
cd ~/gb/apps2/debs
pk=$(apt-get -s install --no-install-recommends "${@/%/:armhf}" 2>&1 | awk "/^Inst /{print \$2}" | sed "s/:armhf//")
echo "packages: $(echo $pk | wc -w)"
for p in $pk; do apt-get download "$p:armhf" >/dev/null 2>&1 || apt-get download "$p" >/dev/null 2>&1 || echo "failed: $p"; done
for d in *.deb; do dpkg -x "$d" ~/gb/apps2/root; done
du -sh ~/gb/apps2/root

#!/bin/bash
# SPDX-License-Identifier: MIT
# etr_bench.sh native|bridge : Extreme Tux Racer, menus into a race (xdotool), 60 s
mode=$1
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
R=$HOME/gb/apps2/root; ETR=etr
# no apt install: the binaries unpacked by getapp.sh/getapp64.sh (data from the arch-independent apt packages)
if [ $mode = bridge ]; then export LD_LIBRARY_PATH=$HOME/gb/build32/shim32:$R/usr/lib/arm-linux-gnueabihf:$R/lib/arm-linux-gnueabihf; ETR=$R/usr/games/etr; elif [ -x $HOME/gb/apps2/root64/usr/games/etr ]; then ETR=$HOME/gb/apps2/root64/usr/games/etr; fi
rm -rf /tmp/etr_home; mkdir -p /tmp/etr_home
( sleep 10; for i in 1 2 3 4 5 6 7 8 9 10 11; do xdotool key Return; sleep 2; done ) &
HOME=/tmp/etr_home timeout -k 3 60 $ETR 2>&1 | grep -a -i "FPSCOUNT" | tail -4

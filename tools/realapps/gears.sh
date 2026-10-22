#!/bin/bash
# SPDX-License-Identifier: MIT
# gears.sh bridge|native|sw glxgears|es2gears_x11 : 30 s of a tiny GL app (CPU cost of the bridge with almost no GL work)
mode=$1; app=$2
R=$HOME/gb/apps2/root; A=arm-linux-gnueabihf
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
case $mode in
 native) BIN=/usr/bin/$app;;
 bridge) BIN=/usr/bin/$app.$A; export LD_LIBRARY_PATH=$HOME/gb/build32/shim32;;   # the system's armhf mesa-utils-bin
 sw)     BIN=/usr/bin/$app.$A;;
esac
timeout ${T:-30} $BIN 2>&1 | grep -a -i "fps\|frames"

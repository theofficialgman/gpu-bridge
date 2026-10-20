#!/bin/bash
# etr_bench.sh native|bridge : Extreme Tux Racer, menus into a race (xdotool), 60 s
mode=$1
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ $mode = bridge ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
rm -rf /tmp/etr_home; mkdir -p /tmp/etr_home
( sleep 10; for i in 1 2 3 4 5 6 7 8 9 10 11; do xdotool key Return; sleep 2; done ) &
HOME=/tmp/etr_home timeout 60 etr 2>&1 | grep -a -i "FPSCOUNT" | tail -4

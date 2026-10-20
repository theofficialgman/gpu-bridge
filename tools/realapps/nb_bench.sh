#!/bin/bash
# nb_bench.sh native|bridge : neverball on its first easy level for 45 s (apt neverball of the matching architecture installed)
mode=$1
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ $mode = bridge ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
lvl=$(ls /usr/share/games/neverball/map-easy/*.sol | head -1)
rm -rf /tmp/nb_home; mkdir -p /tmp/nb_home
( sleep 8; xdotool key space ) &
HOME=/tmp/nb_home timeout 45 neverball -l $lvl 2>&1 | grep -a -i "FPSCOUNT" | tail -4

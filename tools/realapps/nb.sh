#!/bin/bash
# nb.sh tag : run neverball on the first easy level, screenshot at 6 s and 12 s
tag=$1; export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ -n "$BRIDGE" ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
lvl=$(ls /usr/share/games/neverball/map-easy/*.sol | head -1)
rm -rf /tmp/nb_home; mkdir -p /tmp/nb_home
HOME=/tmp/nb_home timeout 30 neverball -l $lvl >/tmp/nb_$tag.log 2>&1 &
sleep 6; import -window root /tmp/nb_${tag}_1.png
xdotool key space; sleep 6; import -window root /tmp/nb_${tag}_2.png
sleep 2; pkill -x neverball; sleep 1
grep -a -i "renderer\|vendor\|version\|error\|fail" /tmp/nb_$tag.log | grep -v joystick | cut -c1-120 | head -6

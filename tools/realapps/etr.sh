#!/bin/bash
tag=$1; export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ -n "$BRIDGE" ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
rm -rf /tmp/etr_home; mkdir -p /tmp/etr_home
HOME=/tmp/etr_home timeout 60 etr >/tmp/etr_$tag.log 2>&1 &
sleep 10; import -window root /tmp/etr_${tag}_1.png
for i in 1 2 3 4 5; do xdotool key Return; sleep 2; done; sleep 6; import -window root /tmp/etr_${tag}_2.png
for i in 1 2 3 4 5 6; do xdotool key Return; sleep 2; done; sleep 12; import -window root /tmp/etr_${tag}_3.png
pkill -x etr; sleep 1; grep -a -i "renderer\|vendor\|error\|fail\|gl " /tmp/etr_$tag.log | grep -v joystick | cut -c1-120 | head -5

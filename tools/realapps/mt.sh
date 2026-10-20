#!/bin/bash
# mt.sh native|bridge : minetest (devtest game) walking forward in a fresh world, 60 s; fps from the fpscount preload
mode=$1
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ $mode = bridge ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
H=/tmp/mt_home; rm -rf $H; mkdir -p $H/.minetest/worlds/tw
printf 'gameid = devtest\nbackend = sqlite3\nplayer_backend = sqlite3\nauth_backend = sqlite3\n' > $H/.minetest/worlds/tw/world.mt
printf 'vsync = %s\nfps_max = 1000\nfps_max_unfocused = 1000\nviewing_range = 120\nenable_sound = false\nscreen_w = 1024\nscreen_h = 600\nfullscreen = false\nmain_menu_script =\nshow_debug = false\n' "$([ -n "$VSYNC" ] && echo true || echo false)" > $H/.minetest/minetest.conf
( sleep 20; xdotool keydown w; sleep 35; xdotool keyup w ) &
HOME=$H timeout 62 minetest --go --worldname tw --gameid devtest --name bench 2>&1 | grep -a "FPSCOUNT" | tail -4

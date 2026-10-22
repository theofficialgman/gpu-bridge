#!/bin/bash
# SPDX-License-Identifier: MIT
# stprof.sh [lazy] : bridged SuperTux, vsync off (VSYNC=1 for on), CPU profile of the client and of gbd
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
cat > /tmp/gbd_prof.sh <<'W'
#!/bin/sh
LD_PRELOAD=/tmp/prof64.so PROF_OUT=/tmp/prof_gbd.txt exec /home/gman/gb/build64/gbd "$@"
W
chmod +x /tmp/gbd_prof.sh
export GB_SERVER=/tmp/gbd_prof.sh LD_LIBRARY_PATH=$HOME/gb/build32/shim32 LD_PRELOAD=/tmp/prof32.so PROF_OUT=/tmp/prof_client.txt
[ "$1" = lazy ] && export GB_LAZY_GETERROR=1
H=/tmp/g_home; rm -rf $H; mkdir -p $H/.local/share/supertux2
printf '(supertux-config (video (fullscreen #f) (video "auto") (vsync %s)))\n' "$([ -n "$VSYNC" ] && echo '#t' || echo '#f')" > $H/.local/share/supertux2/config
[ -z "$VSYNC" ] && export __GL_SYNC_TO_VBLANK=0
( sleep 9; W=$(xdotool search --name "SuperTux" | head -1); xdotool windowactivate --sync $W 2>/dev/null; xdotool windowfocus $W 2>/dev/null; sleep 1; xdotool key Return; sleep 1.5; xdotool key space; sleep 1; xdotool keydown Right; sleep 30; xdotool keyup Right ) &
( sleep 44; kill -USR2 $(pgrep -x gbd) $(pgrep -x supertux2) ) &
HOME=$H timeout -k 3 50 $HOME/gb/apps2/root/usr/games/supertux2 --datadir /usr/share/games/supertux2 /usr/share/games/supertux2/levels/world1/23rd_airborne.stl > /dev/null 2>&1
sleep 2; echo "== client"; head -22 /tmp/prof_client.txt; echo "== gbd"; head -22 /tmp/prof_gbd.txt

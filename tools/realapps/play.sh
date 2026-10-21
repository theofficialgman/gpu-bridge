#!/bin/bash
# play.sh start|measure|stop ... : games played by hand (a person clicks through the menus)
#   play.sh start <name> <native|bridge> <command...>   launch with the frame counter; log in /tmp/play_<name>_<mode>.log
#   play.sh measure <name> <native|bridge> <seconds>    CPU (top 10 processes) and fps over the next <seconds>
#   play.sh stop <name>                                 end the game
# HOME for the game is /tmp/h_<name> (prepare config there first); VSYNC=1 leaves the swap interval to the game,
# otherwise __GL_SYNC_TO_VBLANK=0.
cmd=$1; name=$2
R=$HOME/gb/apps2/root
case $cmd in
start)
  mode=$3; shift 3
  log=/tmp/play_${name}_${mode}.log
  export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
  [ -z "$VSYNC" ] && export __GL_SYNC_TO_VBLANK=0
  [ -n "$STATS" ] && export GB_SERVER_STATS=1 # then `measure` dumps gbd's counters into /tmp/play_<name>_<mode>.stats
  if [ $mode = bridge ]; then
    export LD_LIBRARY_PATH=$HOME/gb/build32/shim32:$R/usr/lib/arm-linux-gnueabihf:$R/lib/arm-linux-gnueabihf LD_PRELOAD=/tmp/fpscount32.so
  else
    export LD_PRELOAD=/tmp/fpscount64.so
    R64=$HOME/gb/apps2/root64; export LD_LIBRARY_PATH=$R64/usr/lib/aarch64-linux-gnu:$R64/lib/aarch64-linux-gnu
  fi
  mkdir -p /tmp/h_$name
  HOME=/tmp/h_$name setsid nohup "$@" > $log 2>&1 < /dev/null &
  echo $! > /tmp/play_$name.pid
  sleep 3; echo "started pid $(cat /tmp/play_$name.pid), log $log"
  ;;
measure)
  mode=$3; secs=${4:-45}
  log=/tmp/play_${name}_${mode}.log
  export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority
  n0=$(grep -a FPSCOUNT $log | grep -vac total)
  python3 /tmp/cpuwin.py --idle 0 --top 10 -- sleep $secs > /tmp/play_${name}_${mode}.cpu 2>&1
  import -window root /tmp/play_${name}_${mode}.png & # after the window: it costs CPU
  if [ -n "$STATS" ]; then kill -USR1 $(pgrep -x gbd | head -1); sleep 2; grep -a '^gbd\|^   gl' $log | tail -60 > /tmp/play_${name}_${mode}.stats; echo "server stats: /tmp/play_${name}_${mode}.stats"; fi
  grep -a FPSCOUNT $log | grep -va total | tail -n +$((n0+1)) | awk '{f=$4; n++; s+=f; if(n==1||f<mn)mn=f; if(f>mx)mx=f} END{ if(n) printf "fps over %d windows of 2 s: mean %.1f  min %.1f  max %.1f\n", n, s/n, mn, mx; else print "no FPSCOUNT lines (no frames swapped?)"}'
  grep -a "cpu-s\|---" /tmp/play_${name}_${mode}.cpu | cut -c1-110
  ;;
stop)
  [ -f /tmp/play_$name.pid ] && { kill -- -$(cat /tmp/play_$name.pid) 2>/dev/null || kill $(cat /tmp/play_$name.pid) 2>/dev/null; sleep 2; kill -9 -- -$(cat /tmp/play_$name.pid) 2>/dev/null; rm -f /tmp/play_$name.pid; }
  pkill -x gbd 2>/dev/null; echo stopped
  ;;
esac

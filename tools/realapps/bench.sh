#!/bin/bash
# SPDX-License-Identifier: MIT
# bench.sh <scenario> <native|bridge|sw> : unlocked frame rate + per-process CPU (top 10) for one app scenario.
# Needs /tmp/fpscount32.so /tmp/fpscount64.so (fpscount.c), cpuwin.py, and the app wrappers oa.sh stk.sh mpv.sh in /tmp.
sc=$1; mode=$2
mkdir -p /tmp/bench
out=/tmp/bench/${sc}_${mode}${VSYNC:+_vsync}.txt
[ $mode = native ] && export LD_PRELOAD=/tmp/fpscount64.so || export LD_PRELOAD=/tmp/fpscount32.so
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority
# VSYNC=1: presentation locked to the display (what players run); otherwise unlocked
[ -z "$VSYNC" ] && export __GL_SYNC_TO_VBLANK=0 || unset __GL_SYNC_TO_VBLANK
case $sc in
 oa2)  if [ -n "$VSYNC" ]; then cmd="env RENDERER=opengl2 SWAPINT=1 T=45 bash /tmp/oa.sh $mode play3.cfg"; else cmd="env RENDERER=opengl2 T=${OA_T:-120} bash /tmp/oa.sh $mode td3.cfg"; fi ;;
 oa1)  if [ -n "$VSYNC" ]; then cmd="env RENDERER=opengl1 SWAPINT=1 T=45 bash /tmp/oa.sh $mode play3.cfg"; else cmd="env RENDERER=opengl1 T=${OA_T:-120} bash /tmp/oa.sh $mode td3.cfg"; fi ;;
 stk)  cmd="env STK_SWAP=${VSYNC:-0} T=120 bash /tmp/stk.sh $mode --profile-time=40 --track=lighthouse --kart=tux --numkarts=4 --screensize=1024x576 --windowed" ;;
 mpv)  if [ -n "$VSYNC" ]; then cmd="env UNTIMED= T=170 bash /tmp/mpv.sh $mode --vo=gpu --gpu-context=x11egl --profile=high-quality --frames=1800 av://lavfi:testsrc2=size=1280x720:rate=60"; else cmd="env T=170 bash /tmp/mpv.sh $mode --vo=gpu --gpu-context=x11egl --profile=high-quality --frames=1500 av://lavfi:testsrc2=size=1280x720:rate=60"; fi ;;
 gears) cmd="env T=30 bash /tmp/gears.sh $mode glxgears" ;;
 es2gears) cmd="env T=30 bash /tmp/gears.sh $mode es2gears_x11" ;;
 glm)  cmd="bash /tmp/glm.sh $mode glmark2" ;;
 glmes) cmd="bash /tmp/glm.sh $mode glmark2-es2" ;;
 mt)   cmd="bash /tmp/mt.sh $mode" ;;
 cbsu|gltron|stux|xmoto|agt|godot) cmd="bash /tmp/games.sh $sc $mode" ;;
 nb)   cmd="bash /tmp/nb_bench.sh $mode" ;;
 etr)  cmd="bash /tmp/etr_bench.sh $mode" ;;
esac
{ echo "### $sc $mode"; python3 /tmp/cpuwin.py --idle 0 --top 10 -- bash -c "$cmd" 2>&1; } > $out
cat $out | grep -a -v "^ERROR: ld.so\|joystick" | grep -a "###\|---\|cpu-s\|ALL PROC\|fps\|FPSCOUNT\|Average FPS\|wall=" | cut -c1-160

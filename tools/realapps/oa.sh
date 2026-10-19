#!/bin/bash
# oa.sh bridge|native|sw cfgfile : run OpenArena client with an exec'd config, print the interesting lines
mode=$1; cfg=$2
if [ $mode = native ]; then R=$HOME/gb/apps2/root64; A=aarch64-linux-gnu; B=ioquake3; else R=$HOME/gb/apps2/root; A=arm-linux-gnueabihf; fi
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
if [ $mode = bridge ]; then export LD_LIBRARY_PATH=$HOME/gb/build32/shim32:$R/usr/lib/$A:$R/lib/$A; else export LD_LIBRARY_PATH=$R/usr/lib/$A:$R/lib/$A; fi
mkdir -p /tmp/oa_home/baseoa; cd /tmp
timeout ${T:-180} $R/usr/lib/ioquake3/ioquake3 +set com_basegame baseoa +set fs_basepath $R/usr/lib/openarena +set fs_homepath /tmp/oa_home +set com_legacyprotocol 71 +set com_protocol 71 +set r_mode -1 +set r_customwidth 1024 +set r_customheight 576 +set r_fullscreen 0 +set s_initsound 0 +set com_introPlayed 1 +set r_swapInterval 0 +set cl_motd 0 +set cl_renderer ${RENDERER:-opengl2} +exec $cfg 2>&1 | grep -a -i "^GL_RENDERER\|^GL_VERSION\|fps\|seconds\|demo\|frames\|ERROR\|Signal\|recording"
echo "rc=${PIPESTATUS[0]}"

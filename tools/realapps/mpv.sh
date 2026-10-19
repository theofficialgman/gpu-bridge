#!/bin/bash
# mpv.sh bridge|native|sw [mpv args]
mode=$1; shift
R=$HOME/gb/apps2/root; A=arm-linux-gnueabihf; MPV=$R/usr/bin/mpv
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
case $mode in
 native) MPV=/usr/bin/mpv; unset LD_LIBRARY_PATH;;
 bridge) export LD_LIBRARY_PATH=$HOME/gb/build32/shim32:$R/usr/lib/$A:$R/lib/$A:$R/usr/lib/$A/openblas-serial:$R/usr/lib/$A/lapack;;
 sw) export LD_LIBRARY_PATH=$R/usr/lib/$A:$R/lib/$A;;
esac
export XDG_DATA_DIRS=$R/usr/share:/usr/share XDG_CONFIG_HOME=/tmp/mpv_cfg HOME=/tmp/mpv_home; mkdir -p /tmp/mpv_cfg /tmp/mpv_home
s=$(date +%s.%N)
timeout ${T:-120} $MPV --no-config --no-audio ${UNTIMED---untimed} --msg-level=all=info "$@" 2>&1
echo "rc=$? wall=$(echo "$(date +%s.%N) - $s" | bc)"

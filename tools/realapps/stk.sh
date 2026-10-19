#!/bin/bash
# stk.sh bridge|native|sw [extra args]
mode=$1; shift
if [ $mode = native ]; then R=$HOME/gb/apps2/root64; A=aarch64-linux-gnu; else R=$HOME/gb/apps2/root; A=arm-linux-gnueabihf; fi
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
if [ $mode = bridge ]; then export LD_LIBRARY_PATH=$HOME/gb/build32/shim32:$R/usr/lib/$A:$R/lib/$A; else export LD_LIBRARY_PATH=$R/usr/lib/$A:$R/lib/$A; fi
export SUPERTUXKART_DATADIR=$HOME/gb/apps2/root/usr/share/games/supertuxkart SUPERTUXKART_ASSETS_DIR=$HOME/gb/apps2/root/usr/share/games/supertuxkart/data
export HOME_STK=/tmp/stk_home; mkdir -p /tmp/stk_home/.config /tmp/stk_home/.cache
[ -n "$STK_SWAP" ] && [ -f /tmp/stk_home/.config/supertuxkart/config-0.10/config.xml ] && sed -i "s/swap-interval=\"[0-9]\"/swap-interval=\"$STK_SWAP\"/" /tmp/stk_home/.config/supertuxkart/config-0.10/config.xml
cd /tmp
HOME=/tmp/stk_home XDG_CONFIG_HOME=/tmp/stk_home/.config XDG_CACHE_HOME=/tmp/stk_home/.cache XDG_DATA_HOME=/tmp/stk_home/.local timeout ${T:-120} $R/usr/games/supertuxkart --no-start-screen "$@" 2>&1
echo "rc=$?"

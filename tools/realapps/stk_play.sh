#!/bin/bash
# stk_play.sh native|bridge : SuperTuxKart for play.sh (HOME is the scratch dir play.sh sets); a 4-kart lighthouse race starts at once
mode=$1
if [ $mode = native ]; then R=$HOME_REAL/gb/apps2/root64; A=aarch64-linux-gnu; else R=$HOME_REAL/gb/apps2/root; A=arm-linux-gnueabihf; fi
export SUPERTUXKART_DATADIR=$HOME_REAL/gb/apps2/root/usr/share/games/supertuxkart SUPERTUXKART_ASSETS_DIR=$HOME_REAL/gb/apps2/root/usr/share/games/supertuxkart/data
export XDG_CONFIG_HOME=$HOME/.config XDG_CACHE_HOME=$HOME/.cache XDG_DATA_HOME=$HOME/.local
mkdir -p $XDG_CONFIG_HOME $XDG_CACHE_HOME $XDG_DATA_HOME
cd /tmp
exec $R/usr/games/supertuxkart --windowed --screensize=1024x576 --no-start-screen --track=lighthouse --kart=tux --numkarts=4 --laps=3 "${@:2}"

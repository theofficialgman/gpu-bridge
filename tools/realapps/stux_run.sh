#!/bin/bash
# stux_run.sh native|bridge [vsync] [lazy] : one verified SuperTux run (gameplay on screen at the end, else retried up to 3 times)
# prints the bench.sh summary lines
mode=$1; vs=$2; lz=$3
for try in 1 2 3; do
  out=$(env ${lz:+GB_LAZY_GETERROR=1} ${vs:+VSYNC=1} bash /tmp/bench.sh stux $mode 2>&1 | grep -a "FPSCOUNT total\|supertux2  \|gbd  \|ALL")
  sleep 9 # the screenshot is written in the background
  m=$(convert /tmp/gshot_stux_$mode.png -crop 900x600+830+500 +repage -colorspace Gray -format "%[fx:mean]" info: 2>/dev/null)
  if awk -v m="$m" 'BEGIN{exit !(m > 0.18)}'; then echo "$out"; echo "(gameplay on screen, brightness $m, try $try)"; cp /tmp/gshot_stux_$mode.png /tmp/gs_${mode}_${vs:-off}${lz:+_lazy}.png; exit 0; fi
  echo "(try $try: title card or blank, brightness $m)" >&2
done
echo "$out"; echo "(NOT VERIFIED)"

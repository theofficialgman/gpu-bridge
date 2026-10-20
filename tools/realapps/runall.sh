#!/bin/bash
# runall.sh native|bridge : every apt-game scenario, unlocked and vsynced (results in /tmp/bench/*.txt)
mode=$1
for sc in glm glmes; do bash /tmp/bench.sh $sc $mode >/dev/null 2>&1; done
for sc in mt cbsu gltron stux xmoto agt godot; do
  bash /tmp/bench.sh $sc $mode >/dev/null 2>&1
  VSYNC=1 bash /tmp/bench.sh $sc $mode >/dev/null 2>&1
done
touch /tmp/bench/runall_$mode.done

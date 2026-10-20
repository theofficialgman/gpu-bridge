#!/bin/bash
# games.sh <app> native|bridge : small apt games driven with xdotool for ~40 s, fps from the fpscount preload
#   app: cbsu (Chromium B.S.U.), gltron, stux (SuperTux), xmoto, agt (Armagetron Advanced), godot (generated 3D test project)
app=$1; mode=$2
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ $mode = bridge ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
H=/tmp/g_home; rm -rf $H; mkdir -p $H
keys() { ( sleep $1; shift; for k in "$@"; do xdotool key $k; sleep 1.5; done ) & }
case $app in
 cbsu)  T=45; CMD="chromium-bsu -w -v 2 -na"; keys 12 Return space;;
 gltron) T=45; CMD="gltron"; keys 8 Return Return Return Return;;
 stux)  T=50
        if [ $mode = native ]; then ST=$HOME/gb/apps2/root64/usr/games/supertux2; else ST=$HOME/gb/apps2/root/usr/games/supertux2; fi
        CMD="$ST --datadir /usr/share/games/supertux2 /usr/share/games/supertux2/levels/world1/23rd_airborne.stl"
        mkdir -p $H/.local/share/supertux2
        printf '(supertux-config\n  (video\n    (fullscreen #f)\n    (vsync %s)\n    (video "auto")))\n' "$([ -n "$VSYNC" ] && echo "#t" || echo "#f")" > $H/.local/share/supertux2/config
        # the keys only count if the window has the focus (the game pauses without it);
        # the level title card needs a key once loading is done
        ( xdotool search --sync --name "SuperTux" >/dev/null 2>&1; sleep 7; W=$(xdotool search --name "SuperTux" | head -1); xdotool windowactivate --sync $W 2>/dev/null; xdotool windowfocus $W 2>/dev/null; sleep 1; xdotool key Return; sleep 2; xdotool key Return ) & ;;  # Tux then stands at the start of the level: a steady scene (running right kills him within seconds)
 xmoto) T=45; CMD="xmoto -l tut1"; keys 10 Return Return;;
 agt)   T=50; CMD="armagetronad"; keys 8 Return Return Return Return Return Return Return Return;;
 godot) T=45; CMD="godot3 --path /tmp/godot_proj"
        mkdir -p /tmp/godot_proj
        cat > /tmp/godot_proj/project.godot <<'P'
config_version=4
[application]
config/name="gbtest"
run/main_scene="res://main.tscn"
[display]
window/size/width=1024
window/size/height=600
window/vsync/use_vsync=false
[rendering]
quality/driver/driver_name="GLES3"
P
        cat > /tmp/godot_proj/spin.gd <<'P'
extends Spatial
func _process(d):
	rotate_y(d)
P
        cat > /tmp/godot_proj/main.tscn <<'P'
[gd_scene load_steps=5 format=2]
[ext_resource path="res://spin.gd" type="Script" id=1]
[sub_resource type="CubeMesh" id=2]
[sub_resource type="SphereMesh" id=3]
[sub_resource type="SpatialMaterial" id=4]
albedo_color = Color( 0.8, 0.3, 0.2, 1 )
[node name="Root" type="Spatial"]
[node name="Camera" type="Camera" parent="."]
transform = Transform( 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 12 )
[node name="Light" type="DirectionalLight" parent="."]
transform = Transform( 1, 0, 0, 0, 0.7, 0.7, 0, -0.7, 0.7, 0, 5, 5 )
shadow_enabled = true
[node name="Spin" type="Spatial" parent="."]
script = ExtResource( 1 )
[node name="Cube" type="MeshInstance" parent="Spin"]
mesh = SubResource( 2 )
material/0 = SubResource( 4 )
[node name="Sphere" type="MeshInstance" parent="Spin"]
transform = Transform( 1, 0, 0, 0, 1, 0, 0, 0, 1, 3, 0, 0 )
mesh = SubResource( 3 )
P
        ;;
esac
( sleep $((T-6)); import -window root /tmp/gshot_${app}_$mode.png ) &
HOME=$H timeout -k 3 $T $CMD 2>&1 | grep -a "FPSCOUNT" | tail -4

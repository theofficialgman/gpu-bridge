#!/bin/bash
# glm.sh native|bridge glmark2|glmark2-es2 : the glmark2 scene list, 2 s each, per-scene fps (apt glmark2 of the matching architecture installed)
mode=$1; app=$2
export DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json GB_ABORT_ON_SERVER_LOSS=1 GB_SERVER=$HOME/gb/build64/gbd
[ $mode = bridge ] && export LD_LIBRARY_PATH=$HOME/gb/build32/shim32
cat > /tmp/glm.list <<'L'
build:use-vbo=false:duration=2.0
build:use-vbo=true:duration=2.0
texture:texture-filter=nearest:duration=2.0
texture:texture-filter=linear:duration=2.0
texture:texture-filter=mipmap:duration=2.0
shading:shading=gouraud:duration=2.0
shading:shading=blinn-phong-inf:duration=2.0
shading:shading=phong:duration=2.0
shading:shading=cel:duration=2.0
bump:bump-render=high-poly:duration=2.0
bump:bump-render=normals:duration=2.0
bump:bump-render=height:duration=2.0
effect2d:kernel=0,1,0;1,-4,1;0,1,0;:duration=2.0
effect2d:kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1;:duration=2.0
pulsar:quads=5:texture=false:texture-filter=linear:duration=2.0
desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4:duration=2.0
desktop:effect=shadow:windows=4:duration=2.0
buffer:update-fraction=0.5:update-dispersion=0.9:columns=200:update-method=map:interleave=false:duration=2.0
buffer:update-fraction=0.5:update-dispersion=0.9:columns=200:update-method=subdata:interleave=false:duration=2.0
buffer:update-fraction=0.5:update-dispersion=0.9:columns=200:update-method=map:interleave=true:duration=2.0
ideas:speed=duration:duration=2.0
jellyfish:duration=2.0
terrain:duration=2.0
shadow:duration=2.0
refract:duration=2.0
conditionals:fragment-steps=0:vertex-steps=0:duration=2.0
conditionals:fragment-steps=5:vertex-steps=0:duration=2.0
function:fragment-complexity=low:fragment-steps=5:duration=2.0
function:fragment-complexity=medium:fragment-steps=5:duration=2.0
loop:fragment-loop=false:fragment-steps=5:vertex-steps=5:duration=2.0
loop:fragment-steps=5:fragment-uniform=false:vertex-steps=5:duration=2.0
loop:fragment-steps=5:fragment-uniform=true:vertex-steps=5:duration=2.0
L
timeout 300 $app -s 800x600 -f /tmp/glm.list 2>&1 | grep -a "FPS:\|Score\|GL_RENDERER\|GL_VERSION"

cmake_minimum_required(VERSION 3.20)
project(gpu_bridge C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)

find_package(Threads REQUIRED)

# --- build modes ---------------------------------------------------------------
# normal : the native compiler builds the 64-bit server, the 64-bit libraries
#          and tests, and -- where the host can link -m32 (the x86-64 dev
#          machine) -- the 32-bit client side with -m32 as a stand-in for
#          arm32. The bridge logic depends on pointer width, not the ISA.
# pure32 : configured with a 32-bit compiler (e.g. -DCMAKE_C_COMPILER=
#          arm-linux-gnueabihf-gcc on the Tegra). Builds only the client side;
#          the 64-bit server comes from a native build (GB_SERVER_PATH).
# A native build can also pick up 32-bit test binaries produced by a pure32
# build via GB_PEER32_BUILD, so one `ctest` run exercises real 32->64 pairs.
set(GB_SERVER_PATH "" CACHE STRING "path of gbd for tests (pure32 builds)")
set(GB_PEER32_BUILD "" CACHE PATH "pure32 build dir whose binaries pair with this build's server")
set(GB_TEST_DISPLAY "" CACHE STRING "run X11 tests on this display (e.g. :0) instead of a private Xvfb")

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(GB_PURE32 TRUE)
  set(GB_HOST_CAN_M32 TRUE)
  set(GB_M32 "")
else()
  set(GB_PURE32 FALSE)
  set(GB_M32 -m32)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-m32")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-m32")
  check_c_source_compiles("#include <stdio.h>\nint main(void){puts(\"x\");return 0;}"
                          GB_HOST_CAN_M32)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

# The 32-bit multiarch library directory (for linking libX11 by soname when
# there is no -dev symlink).
execute_process(COMMAND ${CMAKE_C_COMPILER} ${GB_M32} -print-multiarch
                OUTPUT_VARIABLE GB_MULTIARCH32 OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT GB_MULTIARCH32)
  set(GB_MULTIARCH32 i386-linux-gnu)
endif()
set(GB_X11_32 /usr/lib/${GB_MULTIARCH32}/libX11.so.6)

# Apply the 32-bit flags (none in pure32 mode) to a target.
function(gb_m32 target)
  if(GB_M32)
    target_compile_options(${target} PRIVATE ${GB_M32})
    target_link_options(${target} PRIVATE ${GB_M32})
  endif()
endfunction()

if(GB_PURE32)
  message(STATUS "gpu-bridge: 32-bit client-only build (${GB_MULTIARCH32})")
  # Linking libX11.so.6 by soname: let ld find its own dependencies.
  add_link_options(-Wl,-rpath-link,/usr/lib/${GB_MULTIARCH32}
                   -Wl,-rpath-link,/lib/${GB_MULTIARCH32})
elseif(NOT GB_HOST_CAN_M32)
  message(STATUS "gpu-bridge: -m32 not usable on this host; 32-bit targets "
                 "come from GB_PEER32_BUILD if set")
endif()

# --- libgb-proto -------------------------------------------------------------
set(GB_PROTO_SRC src/proto/gb_ring.c src/proto/gb_chan.c src/proto/gb_ctl.c)

if(NOT GB_PURE32)
  add_library(gb_proto STATIC ${GB_PROTO_SRC})
  target_include_directories(gb_proto PUBLIC src/proto)
endif()
if(GB_HOST_CAN_M32)
  add_library(gb_proto32 STATIC ${GB_PROTO_SRC})
  target_include_directories(gb_proto32 PUBLIC src/proto)
  if(GB_M32)
    target_compile_options(gb_proto32 PUBLIC ${GB_M32})
    target_link_options(gb_proto32 PUBLIC ${GB_M32})
  endif()
endif()

# --- code generation ---------------------------------------------------------
set(GB_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
set(GB_GEN_CHUNKS 16) # keep equal to GEN_CHUNKS in tools/gb-gen/gen_config.py
set(GB_GEN_CLIENT_SRC ${GB_GEN_DIR}/gb_client_all_gen_tab.c)
set(GB_GEN_SERVER_SRC ${GB_GEN_DIR}/gb_server_gen_tab.c)
foreach(i RANGE 0 15)
  string(LENGTH "${i}" _l)
  if(_l EQUAL 1)
    set(_k "0${i}")
  else()
    set(_k "${i}")
  endif()
  list(APPEND GB_GEN_CLIENT_SRC ${GB_GEN_DIR}/gb_client_all_gen_${_k}.c)
  list(APPEND GB_GEN_SERVER_SRC ${GB_GEN_DIR}/gb_server_gen_${_k}.c)
endforeach()
set(GB_GEN_OUT
    ${GB_GEN_DIR}/gb_ops.h ${GB_GEN_DIR}/gb_handle_kinds.h
    ${GB_GEN_DIR}/gb_pname_classes.h
    ${GB_GEN_DIR}/gb_client_all_gen.h ${GB_GEN_DIR}/gb_client_all_protos.h
    ${GB_GEN_DIR}/gb_server_gen.h ${GB_GEN_DIR}/gb_es_intro.h
    ${GB_GEN_CLIENT_SRC} ${GB_GEN_SERVER_SRC})
add_custom_command(
  OUTPUT ${GB_GEN_OUT}
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gb-gen/gb_gen.py
          ${CMAKE_SOURCE_DIR}/registry ${GB_GEN_DIR}
  DEPENDS tools/gb-gen/gb_gen.py tools/gb-gen/gen_config.py
          registry/gl.xml registry/egl.xml registry/glx.xml
  COMMENT "gb-gen: generating EGL/GLES/GLX/GL encoders and decoders")
add_custom_target(gb_generated DEPENDS ${GB_GEN_OUT})

# --- server (always the native 64-bit ABI) -----------------------------------
if(NOT GB_PURE32)
  add_executable(gbd src/server/gbd.c src/server/gb_handle.c
                     src/server/gb_server_manual.c src/server/gb_server_x11.c
                     src/server/gb_server_sizes.c ${GB_GEN_SERVER_SRC})
  add_dependencies(gbd gb_generated)
  target_include_directories(gbd PRIVATE src/server ${GB_GEN_DIR})
  target_link_libraries(gbd gb_proto Threads::Threads ${CMAKE_DL_LIBS})
  set_source_files_properties(${GB_GEN_SERVER_SRC} PROPERTIES
                              COMPILE_FLAGS "-Wno-unused-label -Wno-unused-variable")
endif()

# The server the tests talk to.
if(TARGET gbd)
  set(GB_SERVER_FILE $<TARGET_FILE:gbd>)
else()
  set(GB_SERVER_FILE ${GB_SERVER_PATH})
endif()

# --- client library ---------------------------------------------------------------
# ONE library exports EGL + GLES + GLX + desktop GL, so a process using any mix
# of them (eglgears, SDL, Wine, ...) has a single bridge connection. It is
# built as libGL.so.1 in shim{32,64}/ and installed under the other sonames
# applications link against (libEGL.so.1, libGLESv2.so.2, libGLX.so.0,
# libOpenGL.so.0) via symlinks, like a vendor driver does.
set(GB_CLIENT_SRC src/client/gb_client.c src/client/gb_client_arrays.c
                  src/client/gb_client_strings.c src/client/gb_client_map.c
                  src/client/gb_client_debug.c
                  src/client/gb_client_gl.c src/client/gb_client_glx.c
                  ${GB_GEN_CLIENT_SRC})
# Besides the sonames, the unversioned development names: programs that
# dlopen("libEGL.so") (the Khronos CTS does) must find the bridge too.
set(GB_SONAME_ALIASES libEGL.so.1 libGLESv2.so.2 libGLESv1_CM.so.1
                      libGLX.so.0 libOpenGL.so.0
                      libEGL.so libGLESv2.so libGLESv1_CM.so libGLX.so
                      libOpenGL.so)
function(gb_add_bridge name proto_target shimdir)
  add_library(${name} SHARED ${GB_CLIENT_SRC})
  add_dependencies(${name} gb_generated)
  target_include_directories(${name} PRIVATE src/client ${GB_GEN_DIR})
  target_link_libraries(${name} ${proto_target} Threads::Threads
                        ${CMAKE_DL_LIBS})
  set_target_properties(${name} PROPERTIES
    C_VISIBILITY_PRESET hidden OUTPUT_NAME GL VERSION 1.7.0 SOVERSION 1
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${shimdir})
  foreach(alias ${GB_SONAME_ALIASES})
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink libGL.so.1.7.0
              ${CMAKE_BINARY_DIR}/${shimdir}/${alias}
      COMMENT "${shimdir}/${alias} -> libGL.so.1.7.0")
  endforeach()
endfunction()
set_source_files_properties(${GB_GEN_CLIENT_SRC} PROPERTIES
                            COMPILE_FLAGS "-Wno-unused-variable")
if(NOT GB_PURE32)
  gb_add_bridge(gb64_glx gb_proto shim64)
  add_library(gb64_gl ALIAS gb64_glx)
endif()
if(GB_HOST_CAN_M32)
  gb_add_bridge(gb32_glx gb_proto32 shim32)
  gb_m32(gb32_glx)
  add_library(gb32_gl ALIAS gb32_glx)
endif()

# --- tests -------------------------------------------------------------------
enable_testing()

# X11 tests run on a private Xvfb unless GB_TEST_DISPLAY names a real display.
find_program(GB_XVFB_RUN xvfb-run)
set(GB_X_RUN "")
set(GB_X_OK FALSE)
set(GB_X_ENV "")
if(GB_TEST_DISPLAY)
  set(GB_X_OK TRUE)
  set(GB_X_ENV "DISPLAY=${GB_TEST_DISPLAY}")
elseif(GB_XVFB_RUN)
  set(GB_X_OK TRUE)
  set(GB_X_RUN ${GB_XVFB_RUN} -a -s "-screen 0 1024x768x24")
endif()

# Register a test whose executable is a 32-bit binary from this build or from
# GB_PEER32_BUILD; env entries are appended to ENVIRONMENT.
function(gb_add_peer32_test test_name exe32 envlist)
  if(TARGET ${exe32})
    set(exe $<TARGET_FILE:${exe32}>)
  elseif(GB_PEER32_BUILD)
    set(exe ${GB_PEER32_BUILD}/${exe32})
  else()
    return()
  endif()
  add_test(NAME ${test_name} COMMAND ${GB_X_RUN} ${exe})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${envlist}")
endfunction()

# unit / transport -----------------------------------------------------------
if(NOT GB_PURE32)
  add_executable(test_ring tests/unit/test_ring.c)
  target_link_libraries(test_ring gb_proto Threads::Threads)
  add_test(NAME ring64 COMMAND test_ring)
endif()

if(GB_HOST_CAN_M32)
  add_executable(test_ring32 tests/unit/test_ring.c)
  target_link_libraries(test_ring32 gb_proto32 Threads::Threads)
  gb_m32(test_ring32)
  add_test(NAME ring32 COMMAND test_ring32)

  add_executable(test_ring_xabi32 tests/unit/test_ring_xabi.c)
  target_link_libraries(test_ring_xabi32 gb_proto32)
  gb_m32(test_ring_xabi32)
  add_executable(test_chan_xabi32 tests/unit/test_chan_xabi.c)
  target_link_libraries(test_chan_xabi32 gb_proto32 Threads::Threads)
  gb_m32(test_chan_xabi32)
endif()

if(NOT GB_PURE32)
  # Real cross-ABI: a 32-bit and a 64-bit process sharing one memfd ring /
  # channel. The 64-bit side is the driver; the 32-bit peer comes from this
  # build (-m32) or from a pure32 build.
  if(TARGET test_ring_xabi32)
    set(GB_XABI_RING32 $<TARGET_FILE:test_ring_xabi32>)
    set(GB_XABI_CHAN32 $<TARGET_FILE:test_chan_xabi32>)
  elseif(GB_PEER32_BUILD)
    set(GB_XABI_RING32 ${GB_PEER32_BUILD}/test_ring_xabi32)
    set(GB_XABI_CHAN32 ${GB_PEER32_BUILD}/test_chan_xabi32)
  endif()
  add_executable(test_ring_xabi64 tests/unit/test_ring_xabi.c)
  target_link_libraries(test_ring_xabi64 gb_proto)
  add_executable(test_chan_xabi64 tests/unit/test_chan_xabi.c)
  target_link_libraries(test_chan_xabi64 gb_proto Threads::Threads)
  if(GB_XABI_RING32)
    add_test(NAME xabi_32prod_64cons
             COMMAND test_ring_xabi64 ${GB_XABI_RING32} 32prod)
    add_test(NAME xabi_64prod_32cons
             COMMAND test_ring_xabi64 ${GB_XABI_RING32} 32cons)
    add_test(NAME chan_xabi COMMAND test_chan_xabi64 ${GB_XABI_CHAN32})
  endif()
endif()

# --- end-to-end tests --------------------------------------------------------------
# The same application is linked up to three ways so a test bug cannot hide as
# a bridge bug:  *_native (system driver), *_b64 (bridge, both 64-bit),
# *_b32 (bridge client 32-bit + 64-bit gbd: the real thing).
set(GB_TEST_ENV_EGL "GB_SERVER=${GB_SERVER_FILE};EGL_PLATFORM=surfaceless;GB_EGL_PLATFORM=surfaceless")

# EGL/GLES2 offscreen ---------------------------------------------------------
if(NOT GB_PURE32)
  add_executable(smoke_native tests/loopback/gles2_smoke.c)
  target_link_libraries(smoke_native EGL GLESv2)
  add_executable(smoke_b64 tests/loopback/gles2_smoke.c)
  target_link_libraries(smoke_b64 gb64_gl)
  target_compile_definitions(smoke_b64 PRIVATE GB_BRIDGE)
  add_test(NAME smoke_native COMMAND smoke_native)
  set_tests_properties(smoke_native PROPERTIES
                       ENVIRONMENT "EGL_PLATFORM=surfaceless")
  add_test(NAME smoke_b64 COMMAND smoke_b64)
  set_tests_properties(smoke_b64 PROPERTIES ENVIRONMENT "${GB_TEST_ENV_EGL}")
endif()
if(GB_HOST_CAN_M32)
  add_executable(smoke_b32 tests/loopback/gles2_smoke.c)
  gb_m32(smoke_b32)
  target_link_libraries(smoke_b32 gb32_gl)
  target_compile_definitions(smoke_b32 PRIVATE GB_BRIDGE)
endif()
if(NOT GB_PURE32 OR GB_SERVER_PATH)
  set(GB_X_RUN_SAVE "${GB_X_RUN}")
  set(GB_X_RUN "")
  gb_add_peer32_test(smoke_b32 smoke_b32 "${GB_TEST_ENV_EGL}")
  set(GB_X_RUN "${GB_X_RUN_SAVE}")
endif()

# file descriptors across the bridge (dma-buf export/import, native fences) ----------
if(NOT GB_PURE32)
  add_executable(fd_native tests/loopback/fd_smoke.c)
  target_link_libraries(fd_native EGL GLESv2)
  add_executable(fd_b64 tests/loopback/fd_smoke.c)
  target_link_libraries(fd_b64 gb64_gl)
  target_compile_definitions(fd_b64 PRIVATE GB_BRIDGE)
  add_test(NAME fd_native COMMAND fd_native)
  set_tests_properties(fd_native PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless"
                       SKIP_RETURN_CODE 77)
  add_test(NAME fd_b64 COMMAND fd_b64)
  set_tests_properties(fd_b64 PROPERTIES ENVIRONMENT "${GB_TEST_ENV_EGL}"
                       SKIP_RETURN_CODE 77)
endif()
if(GB_HOST_CAN_M32)
  add_executable(fd_b32 tests/loopback/fd_smoke.c)
  gb_m32(fd_b32)
  target_link_libraries(fd_b32 gb32_gl)
  target_compile_definitions(fd_b32 PRIVATE GB_BRIDGE)
endif()
if(NOT GB_PURE32 OR GB_SERVER_PATH)
  set(GB_X_RUN_SAVE "${GB_X_RUN}")
  set(GB_X_RUN "")
  gb_add_peer32_test(fd_b32 fd_b32 "${GB_TEST_ENV_EGL}")
  set(GB_X_RUN "${GB_X_RUN_SAVE}")
endif()

# EGL window (Strategy X) ----------------------------------------------------------
if(NOT GB_PURE32)
  add_executable(x11_native tests/loopback/x11_window.c)
  target_link_libraries(x11_native EGL GLESv2 X11)
  add_executable(x11_b64 tests/loopback/x11_window.c)
  target_link_libraries(x11_b64 gb64_gl X11)
  target_compile_definitions(x11_b64 PRIVATE GB_BRIDGE)
endif()
if(GB_HOST_CAN_M32)
  add_executable(x11_b32 tests/loopback/x11_window.c)
  gb_m32(x11_b32)
  target_link_libraries(x11_b32 gb32_gl ${GB_X11_32})
  target_compile_definitions(x11_b32 PRIVATE GB_BRIDGE)
endif()
if(GB_X_OK)
  # Native reference selects X11 explicitly (a desktop session may export
  # EGL_PLATFORM=wayland). Bridged runs set EGL_PLATFORM=wayland on purpose to
  # prove the bridge requests the X11 platform itself.
  set(GB_X11_ENV_B "GB_SERVER=${GB_SERVER_FILE};EGL_PLATFORM=wayland")
  if(GB_X_ENV)
    set(GB_X11_ENV_B "${GB_X11_ENV_B};${GB_X_ENV}")
  endif()
  if(NOT GB_PURE32)
    add_test(NAME x11_native COMMAND ${GB_X_RUN} $<TARGET_FILE:x11_native>)
    set_tests_properties(x11_native PROPERTIES
                         ENVIRONMENT "EGL_PLATFORM=x11;${GB_X_ENV}")
    add_test(NAME x11_b64 COMMAND ${GB_X_RUN} $<TARGET_FILE:x11_b64>)
    set_tests_properties(x11_b64 PROPERTIES ENVIRONMENT "${GB_X11_ENV_B}")
  endif()
  if(NOT GB_PURE32 OR GB_SERVER_PATH)
    gb_add_peer32_test(x11_b32 x11_b32 "${GB_X11_ENV_B}")
  endif()
endif()

# GLX + desktop GL ----------------------------------------------------------------
if(NOT GB_PURE32)
  add_executable(glx_native tests/loopback/glx_smoke.c)
  target_link_libraries(glx_native GL X11)
  add_executable(glx_b64 tests/loopback/glx_smoke.c)
  target_link_libraries(glx_b64 gb64_glx X11)
endif()
if(GB_HOST_CAN_M32)
  add_executable(glx_b32 tests/loopback/glx_smoke.c)
  gb_m32(glx_b32)
  target_link_libraries(glx_b32 gb32_glx ${GB_X11_32})
endif()
# getter counts / errors / mapping alignment (reference: the system libGL)
if(NOT GB_PURE32)
  add_executable(state_native tests/loopback/state_smoke.c)
  target_link_libraries(state_native GL X11)
  add_executable(state_b64 tests/loopback/state_smoke.c)
  target_link_libraries(state_b64 gb64_glx X11)
endif()
if(GB_HOST_CAN_M32)
  add_executable(state_b32 tests/loopback/state_smoke.c)
  gb_m32(state_b32)
  target_link_libraries(state_b32 gb32_glx ${GB_X11_32})
endif()
if(GB_X_OK)
  set(GB_GLX_ENV "GB_SERVER=${GB_SERVER_FILE}")
  if(GB_X_ENV)
    set(GB_GLX_ENV "${GB_GLX_ENV};${GB_X_ENV}")
  endif()
  if(NOT GB_PURE32)
    add_test(NAME glx_native COMMAND ${GB_X_RUN} $<TARGET_FILE:glx_native>)
    if(GB_X_ENV)
      set_tests_properties(glx_native PROPERTIES ENVIRONMENT "${GB_X_ENV}")
    endif()
    add_test(NAME glx_b64 COMMAND ${GB_X_RUN} $<TARGET_FILE:glx_b64>)
    set_tests_properties(glx_b64 PROPERTIES ENVIRONMENT "${GB_GLX_ENV}")
  endif()
  if(NOT GB_PURE32 OR GB_SERVER_PATH)
    gb_add_peer32_test(glx_b32 glx_b32 "${GB_GLX_ENV}")
  endif()
  if(NOT GB_PURE32)
    add_test(NAME state_native COMMAND ${GB_X_RUN} $<TARGET_FILE:state_native>)
    if(GB_X_ENV)
      set_tests_properties(state_native PROPERTIES ENVIRONMENT "${GB_X_ENV}")
    endif()
    add_test(NAME state_b64 COMMAND ${GB_X_RUN} $<TARGET_FILE:state_b64>)
    set_tests_properties(state_b64 PROPERTIES ENVIRONMENT "${GB_GLX_ENV}")
  endif()
  if(NOT GB_PURE32 OR GB_SERVER_PATH)
    gb_add_peer32_test(state_b32 state_b32 "${GB_GLX_ENV}")
  endif()
endif()

# --- micro-benchmark (not a test): per-call transport overhead ------------------
if(NOT GB_PURE32)
  add_executable(bench_native tests/bench/call_overhead.c)
  target_link_libraries(bench_native EGL GLESv2)
  add_executable(bench_b64 tests/bench/call_overhead.c)
  target_link_libraries(bench_b64 gb64_gl)
endif()
if(GB_HOST_CAN_M32)
  add_executable(bench_b32 tests/bench/call_overhead.c)
  gb_m32(bench_b32)
  target_link_libraries(bench_b32 gb32_gl)
endif()

# --- visual demos (not tests) -------------------------------------------------------
if(NOT GB_PURE32)
  add_executable(demo_native tests/demo/spin_cube.c)
  target_link_libraries(demo_native EGL GLESv2 X11 m)
  add_executable(demo_b64 tests/demo/spin_cube.c)
  target_link_libraries(demo_b64 gb64_gl X11 m)
  add_executable(gears_native tests/demo/gears.c)
  target_link_libraries(gears_native GL X11 m)
  add_executable(gears_b64 tests/demo/gears.c)
  target_link_libraries(gears_b64 gb64_glx X11 m)
endif()
if(GB_HOST_CAN_M32)
  add_executable(demo_b32 tests/demo/spin_cube.c)
  gb_m32(demo_b32)
  target_link_libraries(demo_b32 gb32_gl ${GB_X11_32} m)
  add_executable(gears_b32 tests/demo/gears.c)
  gb_m32(gears_b32)
  target_link_libraries(gears_b32 gb32_glx ${GB_X11_32} m)
endif()

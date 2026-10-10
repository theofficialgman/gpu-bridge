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

# --- 32-bit (client-ABI) builds ----------------------------------------------
# On the dev machine "32-bit" means i386; on the target it means armhf. The
# bridge logic depends on pointer width, not on the ISA, so -m32 is the
# stand-in. Detect whether this host can link -m32 binaries.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-m32")
set(CMAKE_REQUIRED_LINK_OPTIONS "-m32")
check_c_source_compiles("#include <stdio.h>\nint main(void){puts(\"x\");return 0;}"
                        GB_HOST_CAN_M32)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

# --- libgb-proto -------------------------------------------------------------
set(GB_PROTO_SRC src/proto/gb_ring.c src/proto/gb_chan.c src/proto/gb_ctl.c)

add_library(gb_proto STATIC ${GB_PROTO_SRC})
target_include_directories(gb_proto PUBLIC src/proto)

if(GB_HOST_CAN_M32)
  add_library(gb_proto32 STATIC ${GB_PROTO_SRC})
  target_include_directories(gb_proto32 PUBLIC src/proto)
  target_compile_options(gb_proto32 PUBLIC -m32)
  target_link_options(gb_proto32 PUBLIC -m32)
else()
  message(STATUS "gpu-bridge: -m32 not usable on this host; skipping 32-bit "
                 "targets (install gcc-multilib libc6-dev-i386)")
endif()

# --- code generation ---------------------------------------------------------
set(GB_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
set(GB_GEN_OUT
    ${GB_GEN_DIR}/gb_ops.h ${GB_GEN_DIR}/gb_handle_kinds.h
    ${GB_GEN_DIR}/gb_client_gen.c ${GB_GEN_DIR}/gb_client_gen.h
    ${GB_GEN_DIR}/gb_server_gen.c)
add_custom_command(
  OUTPUT ${GB_GEN_OUT}
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gb-gen/gb_gen.py
          ${CMAKE_SOURCE_DIR}/registry ${GB_GEN_DIR}
  DEPENDS tools/gb-gen/gb_gen.py tools/gb-gen/gen_config.py
          registry/gl.xml registry/egl.xml
  COMMENT "gb-gen: generating EGL/GLES encoders and decoders")
add_custom_target(gb_generated DEPENDS ${GB_GEN_OUT})

# --- server (always the native 64-bit ABI) -----------------------------------
add_executable(gbd src/server/gbd.c src/server/gb_handle.c
                   src/server/gb_server_manual.c ${GB_GEN_DIR}/gb_server_gen.c)
add_dependencies(gbd gb_generated)
target_include_directories(gbd PRIVATE src/server ${GB_GEN_DIR})
target_link_libraries(gbd gb_proto Threads::Threads ${CMAKE_DL_LIBS})
set_source_files_properties(${GB_GEN_DIR}/gb_server_gen.c PROPERTIES
                            COMPILE_FLAGS -Wno-unused-label)

# --- client libraries ----------------------------------------------------------
# One library exporting EGL + GLESv2/3 (a GLVND-vendor split comes later).
set(GB_CLIENT_SRC src/client/gb_client.c src/client/gb_client_gl.c
                  ${GB_GEN_DIR}/gb_client_gen.c)
function(gb_add_client name proto_target)
  add_library(${name} SHARED ${GB_CLIENT_SRC})
  add_dependencies(${name} gb_generated)
  target_include_directories(${name} PRIVATE src/client ${GB_GEN_DIR})
  target_link_libraries(${name} ${proto_target} Threads::Threads
                        ${CMAKE_DL_LIBS})
  set_target_properties(${name} PROPERTIES C_VISIBILITY_PRESET hidden)
endfunction()
gb_add_client(gb64_gl gb_proto)
if(GB_HOST_CAN_M32)
  gb_add_client(gb32_gl gb_proto32)
  target_compile_options(gb32_gl PRIVATE -m32)
  target_link_options(gb32_gl PRIVATE -m32)
endif()

# --- tests -------------------------------------------------------------------
enable_testing()

add_executable(test_ring tests/unit/test_ring.c)
target_link_libraries(test_ring gb_proto Threads::Threads)
add_test(NAME ring64 COMMAND test_ring)

if(GB_HOST_CAN_M32)
  add_executable(test_ring32 tests/unit/test_ring.c)
  target_link_libraries(test_ring32 gb_proto32 Threads::Threads)
  add_test(NAME ring32 COMMAND test_ring32)

  # Real cross-ABI: 32-bit and 64-bit processes sharing one memfd ring.
  add_executable(test_ring_xabi32 tests/unit/test_ring_xabi.c)
  target_link_libraries(test_ring_xabi32 gb_proto32)
  add_executable(test_ring_xabi64 tests/unit/test_ring_xabi.c)
  target_link_libraries(test_ring_xabi64 gb_proto)
  add_test(NAME xabi_32prod_64cons
           COMMAND test_ring_xabi64 $<TARGET_FILE:test_ring_xabi32> 32prod)
  add_test(NAME xabi_64prod_32cons
           COMMAND test_ring_xabi64 $<TARGET_FILE:test_ring_xabi32> 32cons)

  # Control protocol + channel + BIG messages + server-death detection.
  add_executable(test_chan_xabi32 tests/unit/test_chan_xabi.c)
  target_link_libraries(test_chan_xabi32 gb_proto32 Threads::Threads)
  add_executable(test_chan_xabi64 tests/unit/test_chan_xabi.c)
  target_link_libraries(test_chan_xabi64 gb_proto Threads::Threads)
  add_test(NAME chan_xabi
           COMMAND test_chan_xabi64 $<TARGET_FILE:test_chan_xabi32>)
endif()

# --- end-to-end loopback tests --------------------------------------------------
# The same app is linked three ways so a test bug cannot hide as a bridge bug:
#   smoke_native : system 64-bit driver (reference)
#   smoke_b64    : bridge client + gbd, both x86_64
#   smoke_b32    : bridge client built -m32 + gbd x86_64  (the real thing)
add_executable(smoke_native tests/loopback/gles2_smoke.c)
target_link_libraries(smoke_native EGL GLESv2)

add_executable(smoke_b64 tests/loopback/gles2_smoke.c)
target_link_libraries(smoke_b64 gb64_gl)
target_compile_definitions(smoke_b64 PRIVATE GB_BRIDGE)

set(GB_TEST_ENV "GB_SERVER=$<TARGET_FILE:gbd>;EGL_PLATFORM=surfaceless;GB_EGL_PLATFORM=surfaceless")
add_test(NAME smoke_native COMMAND smoke_native)
set_tests_properties(smoke_native PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless")
add_test(NAME smoke_b64 COMMAND smoke_b64)
set_tests_properties(smoke_b64 PROPERTIES ENVIRONMENT "${GB_TEST_ENV}")

if(GB_HOST_CAN_M32)
  add_executable(smoke_b32 tests/loopback/gles2_smoke.c)
  target_compile_options(smoke_b32 PRIVATE -m32)
  target_link_options(smoke_b32 PRIVATE -m32)
  target_link_libraries(smoke_b32 gb32_gl)
  target_compile_definitions(smoke_b32 PRIVATE GB_BRIDGE)
  add_test(NAME smoke_b32 COMMAND smoke_b32)
  set_tests_properties(smoke_b32 PROPERTIES ENVIRONMENT "${GB_TEST_ENV}")
endif()

# --- X11 window presentation (Strategy X) -----------------------------------------
# Run under a private headless X server so no window appears on the desktop.
find_program(GB_XVFB_RUN xvfb-run)
add_executable(x11_native tests/loopback/x11_window.c)
target_link_libraries(x11_native EGL GLESv2 X11)
add_executable(x11_b64 tests/loopback/x11_window.c)
target_link_libraries(x11_b64 gb64_gl X11)
target_compile_definitions(x11_b64 PRIVATE GB_BRIDGE)
if(GB_HOST_CAN_M32)
  add_executable(x11_b32 tests/loopback/x11_window.c)
  target_compile_options(x11_b32 PRIVATE -m32)
  target_link_options(x11_b32 PRIVATE -m32)
  # no libX11.so dev symlink for i386 on this host: link the runtime soname
  target_link_libraries(x11_b32 gb32_gl /usr/lib/i386-linux-gnu/libX11.so.6)
  target_compile_definitions(x11_b32 PRIVATE GB_BRIDGE)
endif()
if(GB_XVFB_RUN)
  foreach(v native b64 b32)
    if(TARGET x11_${v})
      add_test(NAME x11_${v}
               COMMAND ${GB_XVFB_RUN} -a -s "-screen 0 1024x768x24"
                       $<TARGET_FILE:x11_${v}>)
    endif()
  endforeach()
  # Native reference: select X11 explicitly (the desktop session may export
  # EGL_PLATFORM=wayland). Bridged: EGL_PLATFORM=wayland is set on purpose to
  # prove the bridge requests the X11 platform itself.
  set_tests_properties(x11_native PROPERTIES ENVIRONMENT "EGL_PLATFORM=x11")
  foreach(v b64 b32)
    if(TARGET x11_${v})
      set_tests_properties(x11_${v} PROPERTIES ENVIRONMENT
        "GB_SERVER=$<TARGET_FILE:gbd>;EGL_PLATFORM=wayland")
    endif()
  endforeach()
endif()

# --- micro-benchmark (not a test): per-call transport overhead ------------------
add_executable(bench_native tests/bench/call_overhead.c)
target_link_libraries(bench_native EGL GLESv2)
add_executable(bench_b64 tests/bench/call_overhead.c)
target_link_libraries(bench_b64 gb64_gl)
if(GB_HOST_CAN_M32)
  add_executable(bench_b32 tests/bench/call_overhead.c)
  target_compile_options(bench_b32 PRIVATE -m32)
  target_link_options(bench_b32 PRIVATE -m32)
  target_link_libraries(bench_b32 gb32_gl)
endif()

# --- visual demo (not a test): spinning cube in an X11 window -------------------------
add_executable(demo_native tests/demo/spin_cube.c)
target_link_libraries(demo_native EGL GLESv2 X11 m)
add_executable(demo_b64 tests/demo/spin_cube.c)
target_link_libraries(demo_b64 gb64_gl X11 m)
if(GB_HOST_CAN_M32)
  add_executable(demo_b32 tests/demo/spin_cube.c)
  target_compile_options(demo_b32 PRIVATE -m32)
  target_link_options(demo_b32 PRIVATE -m32)
  target_link_libraries(demo_b32 gb32_gl /usr/lib/i386-linux-gnu/libX11.so.6 m)
endif()

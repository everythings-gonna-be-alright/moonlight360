# moonlight360: Moonlight game-streaming client prototype for the Xbox 360 (xsdk).
# Built by scripts/build-app.sh inside the moonlight-xsdk-toolchain container.
cmake_minimum_required(VERSION 3.25)
project(moonlight360 C)

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
# FFmpeg with its AltiVec H.264 code, patched for Xenon (scripts/build-ffmpeg.sh): bit-exact with the C
# build and 17-24% faster at 1080p (decbench, 2026-11-23).
set(FFMPEG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../build/prefix-xsdk-vmx")

# Third-party code is not ours to fix warnings in.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error")

# --- Mbed TLS (xsdk port) ---------------------------------------------------------------------------------------
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/mbedtls-xenon" mbedtls EXCLUDE_FROM_ALL)

# --- curl (xsdk port), on mbedtls-xenon's own targets -------------------------------------------------------------
set(MBEDTLS_INCLUDE_DIR "${TP}/mbedtls-xenon/include" CACHE PATH "")
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "")
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "")
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "")
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${TP}/curl-xenon" curl EXCLUDE_FROM_ALL)

# --- expat -------------------------------------------------------------------------------------------------------
set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(EXPAT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EXPAT_DEV_URANDOM OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/expat/expat" expat EXCLUDE_FROM_ALL)
# No getrandom/arc4random on the console: expat seeds its hash salt from the clock instead.
target_compile_definitions(expat PRIVATE XML_POOR_ENTROPY)

# --- Opus --------------------------------------------------------------------------------------------------------
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/opus" opus EXCLUDE_FROM_ALL)

# --- moonlight-common-c (the version moonlight-tv pins) ----------------------------------------------------------
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
add_subdirectory("${TP}/moonlight-tv/core/moonlight-common-c" moonlight-common-c EXCLUDE_FROM_ALL)
target_compile_options(moonlight-common-c PRIVATE -Wno-error)
target_include_directories(enet BEFORE PRIVATE compat/posix)
target_include_directories(moonlight-common-c BEFORE PRIVATE compat/posix)
target_compile_definitions(moonlight-common-c PRIVATE AI_ADDRCONFIG=0
  # Never ask Sunshine to QoS-tag the stream: qWave's 802.1p priority tags on every video/audio frame wedge the
  # 360's network hardware (patches/moonlight-common-c-sdp-options.patch).
  LC_NO_QOS_TAGGING)
# No sendmsg/recvmsg on xsdk: ENet's NO_MSGAPI path uses sendto/recvfrom. XNet is Winsock-like: IP_TTL is 4.
target_compile_definitions(enet PRIVATE NO_MSGAPI=1 AI_ADDRCONFIG=0 IP_TTL=4)

# --- libgamestream (pairing, server info, app list, launch) from moonlight-tv -------------------------------------
set(GS "${TP}/moonlight-tv/core/libgamestream")
add_library(gamestream STATIC
  ${GS}/src/client.c ${GS}/src/http.c ${GS}/src/mkcert.c ${GS}/src/xml.c ${GS}/src/conf.c ${GS}/src/set_error.c
  compat/commons.c)
set_target_properties(gamestream PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(gamestream PUBLIC ${GS}/libgamestream compat)
target_link_libraries(gamestream PUBLIC moonlight-common-c CURL::libcurl expat mbedtls mbedx509 mbedcrypto)
target_include_directories(gamestream PRIVATE "${TP}/moonlight-tv/core/moonlight-common-c/src")
target_compile_definitions(gamestream PRIVATE PATH_MAX=260)

# --- The title ---------------------------------------------------------------------------------------------------
add_executable(moonlight360
  src/main.c src/config.c src/menu.c src/apps.c src/overlay.c src/ui.c src/gfx.c src/perf.c src/gpumc.c src/gpudec.c src/session.c src/video.c src/render.c src/audio.c src/input.c src/placement.c src/posix_stubs.c src/diag.c src/netfix.c src/logbuf.c src/devreturn.c)
set_target_properties(moonlight360 PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(moonlight360 PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_include_directories(moonlight360 PRIVATE src "${FFMPEG_PREFIX}/include"
  "${TP}/moonlight-tv/core/moonlight-common-c/src")
# Every pthread goes through placement.c, which puts it on a hardware thread.
target_link_options(moonlight360 PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=setsockopt -Wl,--wrap=recvfrom -Wl,--wrap=connect
  -Wl,--wrap=xsdk_log_write_bytes -Wl,--wrap=poll)
target_link_libraries(moonlight360 PRIVATE gamestream moonlight-common-c opus
  "${FFMPEG_PREFIX}/lib/libavcodec.a" "${FFMPEG_PREFIX}/lib/libavutil.a")
xsdk_add_shaders(moonlight360 shaders/yuv.vert shaders/yuv.frag shaders/shape.vert shaders/shape.frag
                 shaders/mc.vert shaders/mc_chroma.frag shaders/mc_probe.frag shaders/mc_copy.frag shaders/mc_pg.frag
                 shaders/mc_pb.frag shaders/mc_ph.frag shaders/mc_pj.frag shaders/mc_final.frag)
xsdk_add_xex(moonlight360 NAME "Moonlight 360" VERSION 0.1.0.0)

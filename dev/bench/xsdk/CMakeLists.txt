# Console benchmarks for the decoder (xsdk): decbench (decode speed across setups) and hybench (the hybrid GPU
# decoder against plain decoding). Built by dev/scripts/build-bench.sh; the clips come from dev/make_clips.sh.
cmake_minimum_required(VERSION 3.25)
project(moonlight360_bench C ASM)

set(APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../app")

set(FFMPEG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/prefix-xsdk")

# decbench: 1080p30 ~22 Mbit/s decode across threading, entropy coders and deblocking (decbench.c), with
# FFmpeg in plain C (decbench) and with its patched AltiVec code (decbench_vmx). decbench starts decbench_vmx when done.
set(FFMPEG_VMX_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/prefix-xsdk-vmx")
set(CLIPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data")
set(CLIP_FILES CABAC22=clip1080_cabac_s6 CABAC12=clip1080_cabac_s6_12500 CAVLC22=clip1080_cavlc_s6
               CAVLC12=clip1080_cavlc_s6_12500 CAVLC5=clip1080_cavlc_s6_6000)
set(clip_defs "")
set(clip_deps "")
foreach(entry IN LISTS CLIP_FILES)
  string(REPLACE "=" ";" pair "${entry}")
  list(GET pair 0 key)
  list(GET pair 1 file)
  list(APPEND clip_defs "CLIP_${key}=\"${CLIPS_DIR}/${file}.h264\"")
  list(APPEND clip_deps "${CLIPS_DIR}/${file}.h264")
endforeach()
set_source_files_properties(decclips.S PROPERTIES COMPILE_DEFINITIONS "${clip_defs}" OBJECT_DEPENDS "${clip_deps}")
foreach(flavour IN ITEMS c vmx)
  if(flavour STREQUAL "c")
    set(target decbench)
    set(prefix "${FFMPEG_PREFIX}")
    set(defs DECBENCH_FLAVOUR="plain C" DECBENCH_NEXT="game:\\\\decbench_vmx.xex")
  else()
    set(target decbench_vmx)
    set(prefix "${FFMPEG_VMX_PREFIX}")
    set(defs DECBENCH_FLAVOUR="AltiVec" DECBENCH_VMX=1)
  endif()
  add_executable(${target} decbench.c ${APP_DIR}/src/devreturn.c decclips.S)
  set_target_properties(${target} PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall>)
  target_compile_definitions(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${defs}>)
  target_include_directories(${target} PRIVATE ${APP_DIR}/src "${prefix}/include")
  target_link_options(${target} PRIVATE -Wl,--wrap=pthread_create)
  target_link_libraries(${target} PRIVATE "${prefix}/lib/libavcodec.a" "${prefix}/lib/libavutil.a")
  xsdk_add_xex(${target} NAME "Moonlight 1080p decode bench (${flavour})" VERSION 0.1.0.0)
endforeach()

# hybench: the hybrid decoder (FFmpeg parses, app/src/gpumc.c predicts on the GPU, FFmpeg replays) against plain
# decoding, frame by frame (hybench.c).
add_executable(hybench hybench.c "${APP_DIR}/src/gpumc.c" "${APP_DIR}/src/gfx.c" ../source/mc_ref.c ${APP_DIR}/src/devreturn.c hybclips.S)
set_target_properties(hybench PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(hybench PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall>)
target_compile_definitions(hybench PRIVATE $<$<COMPILE_LANGUAGE:C>:GPUMC_SHADERS="hybench_shaders.h">)
target_include_directories(hybench PRIVATE ../source "${APP_DIR}/src" "${FFMPEG_VMX_PREFIX}/include")
set_source_files_properties(hybclips.S PROPERTIES
  COMPILE_DEFINITIONS "CLIP_CAVLC12=\"${CLIPS_DIR}/clip1080_cavlc_s6_12500.h264\";CLIP_CAVLC22=\"${CLIPS_DIR}/clip1080_cavlc_s6.h264\";CLIP_CABAC22=\"${CLIPS_DIR}/clip1080_cabac_s6.h264\""
  OBJECT_DEPENDS "${CLIPS_DIR}/clip1080_cavlc_s6_12500.h264;${CLIPS_DIR}/clip1080_cavlc_s6.h264;${CLIPS_DIR}/clip1080_cabac_s6.h264")
target_link_options(hybench PRIVATE -Wl,--wrap=pthread_create)
target_link_libraries(hybench PRIVATE "${FFMPEG_VMX_PREFIX}/lib/libavcodec.a" "${FFMPEG_VMX_PREFIX}/lib/libavutil.a")
xsdk_add_shaders(hybench "${APP_DIR}/shaders/shape.vert" "${APP_DIR}/shaders/shape.frag" "${APP_DIR}/shaders/mc.vert" "${APP_DIR}/shaders/mc_chroma.frag"
                 "${APP_DIR}/shaders/mc_probe.frag" "${APP_DIR}/shaders/mc_copy.frag"
                 "${APP_DIR}/shaders/mc_pg.frag" "${APP_DIR}/shaders/mc_pb.frag" "${APP_DIR}/shaders/mc_ph.frag"
                 "${APP_DIR}/shaders/mc_pj.frag" "${APP_DIR}/shaders/mc_final.frag")
xsdk_add_xex(hybench NAME "Moonlight hybrid decode bench" VERSION 0.1.0.0)

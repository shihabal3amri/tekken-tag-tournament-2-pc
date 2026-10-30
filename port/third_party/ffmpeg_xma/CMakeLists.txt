# Minimal static FFmpeg (LGPL-2.1+) for XMA audio: libavutil + the libavcodec subset behind the
# WMA Pro / XMA1 / XMA2 decoders and the XMA parser. Built from the user's local download of the
# official FFmpeg n7.1 source (tools/ffmpeg_dl, not shipped) configured with:
#   configure --toolchain=msvc --target-os=win64 --arch=x86_64 --disable-everything
#     --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swresample
#     --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-asm
#     --disable-debug --disable-autodetect --enable-decoder=wmapro,xma1,xma2 --enable-parser=xma
# FFMPEG_SRC = the source tree, FFMPEG_BUILD = configure's output (config.h, avconfig.h, codec lists).
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED FFMPEG_SRC)
    set(FFMPEG_SRC "C:/ttt2/tools/ffmpeg_dl/FFmpeg-n7.1")
endif()
if(NOT DEFINED FFMPEG_BUILD)
    set(FFMPEG_BUILD "C:/ttt2/tools/ffmpeg_dl/build")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

# configure leaves libavutil/ffversion.h to make; provide it.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ffgen/libavutil/ffversion.h
     "#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n#define FFMPEG_VERSION \"n7.1\"\n#endif\n")

# Build-time changes to the XMA wrapper (the source tree stays as downloaded), all for
# single-stream decoders, which is what a hardware XMA context is: one stream fed
# continuously that never sees an end of stream.
# - Release each packet's samples as soon as the packet is decoded. Upstream holds the last
#   4096 samples back until end of stream so multi-stream files stay aligned; the hold would
#   add ~85 ms of latency and strand the tail of every sound.
# - Output the first frame (after init and after every flush). Upstream discards the first
#   decoded frame (IMDCT warm-up) and makes up for it only when drained at end of stream. The
#   hardware writes 512 samples per channel for every frame it decodes, and TTT2's NUSound
#   ends a one-shot only once it has received all frames x 512 samples (the RIFF's
#   SamplesEncoded): one frame short, every finished one-shot kept its voice until the pool
#   ran out and the engine re-voiced the stage music (measured: 43520 of 44032 samples; the
#   music cut out about two minutes into a fight). Xenia's per-frame XMA codec does not skip
#   either (xmaframes_decode_init).
set(_wmapro_in "${FFMPEG_SRC}/libavcodec/wmaprodec.c")
set(_wmapro_out "${CMAKE_CURRENT_BINARY_DIR}/ffgen/libavcodec/wmaprodec.c")
file(READ "${_wmapro_in}" _wmapro)
set(_wmapro_patched "${_wmapro}")
function(_wmapro_patch name old new)
    string(REPLACE "${old}" "${new}" _patched "${_wmapro_patched}")
    if(_patched STREQUAL _wmapro_patched)
        message(FATAL_ERROR "wmaprodec.c: ${name} not found (FFmpeg version changed?)")
    endif()
    set(_wmapro_patched "${_patched}" PARENT_SCOPE)
endfunction()
_wmapro_patch("XMA sample hold"
    "        if (!eof && avpkt->size)\n            nb_samples -= FFMIN(nb_samples, 4096);"
    "        if (!eof && avpkt->size && s->num_streams > 1)\n            nb_samples -= FFMIN(nb_samples, 4096);")
_wmapro_patch("XMA stream init"
    "        ret = decode_init(&s->xma[i], avctx, i);\n        if (ret < 0)\n            return ret;\n"
    "        ret = decode_init(&s->xma[i], avctx, i);\n        if (ret < 0)\n            return ret;\n        if (s->num_streams == 1)\n            s->xma[i].skip_frame = 0;\n")
_wmapro_patch("XMA stream flush"
    "    for (i = 0; i < s->num_streams; i++)\n        flush(&s->xma[i]);\n"
    "    for (i = 0; i < s->num_streams; i++) {\n        flush(&s->xma[i]);\n        if (s->num_streams == 1)\n            s->xma[i].skip_frame = 0;\n    }\n")
# configure_file leaves an unchanged output untouched, so reconfiguring does not recompile it.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wmaprodec.c.in" "${_wmapro_patched}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/wmaprodec.c.in" "${_wmapro_out}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wmapro_in}")
set_source_files_properties("${_wmapro_out}" PROPERTIES INCLUDE_DIRECTORIES "${FFMPEG_SRC}/libavcodec")

set(_ff_files "")
foreach(f IN LISTS TTT2_FFMPEG_SOURCES)
    if(f STREQUAL "libavcodec/wmaprodec.c")
        list(APPEND _ff_files "${_wmapro_out}")
    else()
        list(APPEND _ff_files "${FFMPEG_SRC}/${f}")
    endif()
endforeach()
add_library(ttt2_ffmpeg STATIC ${_ff_files})
target_include_directories(ttt2_ffmpeg
    PRIVATE ${FFMPEG_BUILD} ${CMAKE_CURRENT_BINARY_DIR}/ffgen ${FFMPEG_SRC}
            ${FFMPEG_SRC}/compat/atomics/win32 ${FFMPEG_SRC}/compat/stdbit
    INTERFACE ${FFMPEG_BUILD} ${CMAKE_CURRENT_BINARY_DIR}/ffgen ${FFMPEG_SRC})
target_compile_definitions(ttt2_ffmpeg PRIVATE
    HAVE_AV_CONFIG_H _ISOC11_SOURCE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE WIN32_LEAN_AND_MEAN
    _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _WIN32_WINNT=0x0600)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(ttt2_ffmpeg PRIVATE /std:c17 /O2 -Wno-everything)
else()
    target_compile_options(ttt2_ffmpeg PRIVATE /std:c17 /O2 /W0 /utf-8)
endif()
set_target_properties(ttt2_ffmpeg PROPERTIES C_STANDARD 17)

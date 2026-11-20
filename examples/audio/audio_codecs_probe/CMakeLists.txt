cmake_minimum_required(VERSION 3.24)
project(audio_codecs_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# audio-codecs on the RT1176: the four decoders the DJ deck loads, and the
# spectrum and tempo engines, run over clips held in flash.
# Bench only: -DPROBE_PROFILE=1 compiles audio-codecs' stage timers in and prints
# PROF lines (microseconds per stage) after each clip and after the analysis.
if(PROBE_PROFILE)
    add_compile_definitions(AUDIO_CODECS_PROFILE=1)
endif()

import_evkb_audio_codecs()

teensy_add_executable(audio_codecs_probe audio_codecs_probe.cpp)
# The clips are linked in as binary data (clips_data.S, .incbin from clips/).
target_sources(audio_codecs_probe.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/clips_data.S)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/clips_data.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp;-I${CMAKE_CURRENT_LIST_DIR}"
    OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/clips/click.flac;${CMAKE_CURRENT_LIST_DIR}/clips/click.mp3;${CMAKE_CURRENT_LIST_DIR}/clips/short.wav;${CMAKE_CURRENT_LIST_DIR}/clips/short.aiff;${CMAKE_CURRENT_LIST_DIR}/clips/short24.wav")
teensy_target_link_libraries(audio_codecs_probe cores)
target_compile_options(audio_codecs_probe.elf PRIVATE -std=gnu++17)
target_link_libraries(audio_codecs_probe.elf AudioCodecs stdc++ m)
target_link_options(audio_codecs_probe.elf PRIVATE -Wl,--print-memory-usage)

# Bench only: -DPROBE_DECODER_IN_SDRAM=1 moves the decoder arena from DTCM to
# SDRAM, to reproduce how slow a decoder is there (see the .cpp).
if(PROBE_DECODER_IN_SDRAM)
    target_compile_definitions(audio_codecs_probe.elf PRIVATE PROBE_DECODER_IN_SDRAM=1)
endif()

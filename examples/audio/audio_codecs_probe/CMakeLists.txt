cmake_minimum_required(VERSION 3.24)
project(audio_codecs_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# audio-codecs on the RT1176: the four decoders the DJ deck loads, and the
# spectrum and tempo engines, run over clips held in flash.
import_evkb_audio_codecs()

teensy_add_executable(audio_codecs_probe audio_codecs_probe.cpp)
teensy_target_link_libraries(audio_codecs_probe cores)
target_compile_options(audio_codecs_probe.elf PRIVATE -std=gnu++17)
target_link_libraries(audio_codecs_probe.elf AudioCodecs stdc++ m)
target_link_options(audio_codecs_probe.elf PRIVATE -Wl,--print-memory-usage)

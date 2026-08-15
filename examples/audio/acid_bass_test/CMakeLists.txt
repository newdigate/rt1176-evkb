cmake_minimum_required(VERSION 3.24)
project(acid_bass_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(acid_bass_test acid_bass_test.cpp)
teensy_target_link_libraries(acid_bass_test cores Audio Wire SPI SdFat SD SerialFlash)
# CMSIS-DSP is a plain STATIC lib (no `.o` suffix on the target name), so it
# links via raw target_link_libraries -- same as audio_h_test.
target_link_libraries(acid_bass_test.elf CMSIS-DSP stdc++ m)

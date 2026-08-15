cmake_minimum_required(VERSION 3.24)
project(step_seq_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(step_seq_test step_seq_test.cpp)
teensy_target_link_libraries(step_seq_test cores Audio Wire SPI SdFat SD SerialFlash)
# CMSIS-DSP is a plain STATIC lib (no `.o` suffix on the target name), so it
# links via raw target_link_libraries -- same as transport_test / acid_bass_test.
target_link_libraries(step_seq_test.elf CMSIS-DSP stdc++ m)

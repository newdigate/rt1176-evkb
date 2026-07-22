cmake_minimum_required(VERSION 3.24)
project(audio_h_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(audio_h_test audio_h_test.cpp)
teensy_target_link_libraries(audio_h_test cores Audio Wire SdFat SD SerialFlash CMSIS-DSP)
target_link_libraries(audio_h_test.elf stdc++ m)

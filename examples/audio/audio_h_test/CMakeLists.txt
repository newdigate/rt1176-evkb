cmake_minimum_required(VERSION 3.24)
project(audio_h_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(audio_h_test audio_h_test.cpp)
teensy_target_link_libraries(audio_h_test cores Audio Wire SPI SdFat SD SerialFlash)
# CMSIS-DSP is a plain STATIC lib (its target name has no `.o` suffix), so it must
# link via the raw target_link_libraries -- exactly like filter_fir_test /
# arm_math_test. The teensy_ wrapper appends `.o` to every name (correct for the
# import_arduino_library targets Audio.o/Wire.o/... but yields -lCMSIS-DSP.o here).
target_link_libraries(audio_h_test.elf CMSIS-DSP stdc++ m)

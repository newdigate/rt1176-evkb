cmake_minimum_required(VERSION 3.24)
project(cm4_audiostream_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# The CM4 image compiles the REAL core AudioStream.cpp against the Arduino-lite
# shim: INCLUDE_DIRS order is load-bearing — the shim dir sits BEFORE the core
# dir so `#include <Arduino.h>` resolves to cm4_shim/Arduino.h while
# `#include "AudioStream.h"` resolves to the real engine header.
teensy_add_cm4_image(cm4_audiostream
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.cpp
            ${CMAKE_CURRENT_LIST_DIR}/cm4/runtime_stubs.c
            ${EVKB_CORES_DIR}/AudioStream.cpp
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/cm4
                 ${EVKB_CORES_DIR}/cm4_shim
                 ${EVKB_CORES_DIR})

teensy_add_executable(cm4_audiostream_test cm4_audiostream_test.cpp)
teensy_target_link_libraries(cm4_audiostream_test cores)
target_link_libraries(cm4_audiostream_test.elf stdc++)
teensy_target_link_cm4_image(cm4_audiostream_test cm4_audiostream)

cmake_minimum_required(VERSION 3.24)
project(cm4_image_test)

# Fallback if the toolchain file isn't passed (see toolchain/rt1170-evkb…).
set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# The imxrt1176 core (includes the Phase-1 Multicore + MessagingUnit).

# Phase 2B: the CM4 secondary image is now a first-class dual target — the
# macro compiles the bare-metal CM4 sources (own startup/linker) and emits
# cm4_image.h (a uint32_t array) into the build dir.
teensy_add_cm4_image(cm4_image
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_executable(cm4_image_test cm4_image_test.cpp)
teensy_target_link_libraries(cm4_image_test cores)
target_link_libraries(cm4_image_test.elf stdc++)

# Embed the CM4 image: adds the dependency + build-dir include path so the CM7
# sketch's `#include "cm4_image.h"` resolves.
teensy_target_link_cm4_image(cm4_image_test cm4_image)

cmake_minimum_required(VERSION 3.24)
project(cm4_image_test)

# Fallback if the toolchain file isn't passed (see toolchain/rt1170-evkb…).
set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# The imxrt1176 core (includes the Phase-1 Multicore + MessagingUnit).
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# Build the bare-metal CM4 sub-image (own startup/linker) -> cm4_image.h in the
# build dir, so the embedded blob always reflects the current cm4/ sources.
# (Phase 2B folds this into teensy-cmake-macros as a first-class dual target.)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cm4_image.h
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/cm4/build_cm4.sh ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
        ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
        ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
        ${CMAKE_CURRENT_LIST_DIR}/cm4/bin2header.py
    COMMENT "Building CM4 image -> cm4_image.h"
    VERBATIM
)
add_custom_target(cm4_image_gen DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/cm4_image.h)

teensy_add_executable(cm4_image_test cm4_image_test.cpp)
teensy_target_link_libraries(cm4_image_test cores)
target_link_libraries(cm4_image_test.elf stdc++)

# The gate .cpp #includes the generated cm4_image.h from the build dir.
add_dependencies(cm4_image_test.elf cm4_image_gen)
target_include_directories(cm4_image_test.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

cmake_minimum_required(VERSION 3.24)
project(string_test)

# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
# Left unguarded this caches 117 and silently builds an RT1176 image into
# build-rt1062/, which then boots the wrong QEMU machine and fails looking like
# a board or model problem rather than a build misconfiguration.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(string_test string_test.cpp)
teensy_target_link_libraries(string_test cores)

target_link_libraries(string_test.elf stdc++)

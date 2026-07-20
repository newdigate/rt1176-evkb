cmake_minimum_required(VERSION 3.24)
project(dualcore_mu_test)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH / COREPATH are supplied by the
# toolchain file (toolchain/rt1170-evkb.toolchain.cmake). Keep a fallback here
# so a bare `cmake -B build .` without the toolchain file still selects 117.
set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
# Use the LOCAL macros checkout (branch imxrt1176) rather than fetching from
# GitHub, so local edits to CMakeLists.include.txt are exercised.

# Compile the whole imxrt1176 core as the `cores` library.

teensy_add_executable(dualcore_mu_test dualcore_mu_test.cpp)
teensy_target_link_libraries(dualcore_mu_test cores)

target_link_libraries(dualcore_mu_test.elf stdc++)

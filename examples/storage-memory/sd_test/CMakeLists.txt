cmake_minimum_required(VERSION 3.24)
# Board + compiler come from the shared toolchain file, included BEFORE
# project() — same mechanism as passing -DCMAKE_TOOLCHAIN_FILE, so a plain
# `cmake -B build` still cross-compiles with no extra flags.
include(${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/rt1170-evkb.toolchain.cmake)
project(sd_test)
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_library(Wire)
import_evkb_library(SPI)
import_evkb_library(SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib
    src/iostream src/SpiDriver)
import_evkb_library(SD src)
teensy_add_executable(sd_test sd_test.cpp)
teensy_target_link_libraries(sd_test cores Wire SPI SdFat SD)
target_link_libraries(sd_test.elf stdc++)
target_link_libraries(sd_test.elf m)

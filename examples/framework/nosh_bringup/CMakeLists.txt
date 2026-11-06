cmake_minimum_required(VERSION 3.24)
project(nosh_bringup)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# Nosh-only: the sketch names NOSH_PIN_* and the 128 MB SEMC window, and the
# source #errors on any other board.  Say so at configure time, where the fix
# is visible, rather than in a compiler error deep in the build.
if(NOT EVKB_BOARD STREQUAL "nosh")
    message(FATAL_ERROR "nosh_bringup builds for the Nosh board only:\n"
        "  cmake -B build-nosh -DEVKB_BOARD=nosh "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/nosh.toolchain.cmake")
endif()

import_evkb_library(Wire)
import_evkb_library(SPI)
# SdFat is a 1.5-format lib: enumerate every src subdir that carries sources (src
# first for the include root). src/SpiDriver is required because SDClass::begin()
# references the SPI-SD path (runtime if, not strippable), even though Nosh has
# no SPI bus and the card sits on SDIO -- the SPI object exists for exactly this
# link-time reason (see SPIIMXRT1176.h, SPIIMXRT1176_NO_BUS).
import_evkb_library(SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib
    src/iostream src/SpiDriver)
import_evkb_library(SD src)
# The SDIO host alone: enumeration of the M.2 module, no IW416 driver.
import_evkb_library(M2Radio sdio)

teensy_add_executable(nosh_bringup nosh_bringup.cpp)
teensy_target_link_libraries(nosh_bringup cores Wire SPI SdFat SD M2Radio)

target_link_libraries(nosh_bringup.elf stdc++)

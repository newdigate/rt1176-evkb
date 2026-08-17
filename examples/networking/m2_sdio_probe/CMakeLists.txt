cmake_minimum_required(VERSION 3.24)
project(m2_sdio_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(M2Radio sdio)

teensy_add_executable(m2_sdio_probe m2_sdio_probe.cpp)
teensy_target_link_libraries(m2_sdio_probe cores M2Radio)

target_link_libraries(m2_sdio_probe.elf stdc++)

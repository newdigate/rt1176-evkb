cmake_minimum_required(VERSION 3.24)
project(camera_preview_synth)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(SPI)
import_evkb_library(ILI9341_t3)
import_evkb_library(PXP)

teensy_add_executable(camera_preview_synth camera_preview_synth.cpp)
teensy_target_link_libraries(camera_preview_synth cores SPI ILI9341_t3 PXP)

target_link_libraries(camera_preview_synth.elf stdc++)

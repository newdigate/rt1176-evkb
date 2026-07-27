cmake_minimum_required(VERSION 3.24)
project(lvgl_ili9341_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(SPI)
import_evkb_library(ILI9341_t3)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_ili9341_test
    lvgl_ili9341_test.cpp
    ${_lvgl_dir}/port/lvgl_ili9341.cpp)
teensy_target_link_libraries(lvgl_ili9341_test cores SPI ILI9341_t3)

# LVGL is a plain CMake static-lib target (like CMSIS-DSP), not a
# teensy_add_library() one: teensy_target_link_libraries() rewrites each name to
# <name>.o and would not propagate LVGL's PUBLIC include dirs. Link it directly,
# exactly as arm_math_test does.
target_link_libraries(lvgl_ili9341_test.elf LVGL stdc++)

cmake_minimum_required(VERSION 3.24)
project(lvgl_rpi_panel_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(RPiDisplay)
import_evkb_library(Wire)   # the RPiDisplay ATtiny driver (Display::begin())
import_evkb_library(PXP)    # Display::fillScreen() paints the framebuffer with the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rpi_panel_test
    lvgl_rpi_panel_test.cpp
    ${_lvgl_dir}/port/lvgl_rpi_panel.cpp)
teensy_target_link_libraries(lvgl_rpi_panel_test cores RPiDisplay Wire PXP)

# LVGL is a plain CMake static-lib target (like CMSIS-DSP), not a
# teensy_add_library() one: teensy_target_link_libraries() rewrites each name to
# <name>.o and would not propagate LVGL's PUBLIC include dirs. Link it directly,
# exactly as arm_math_test and lvgl_ili9341_test do.
target_link_libraries(lvgl_rpi_panel_test.elf LVGL stdc++)

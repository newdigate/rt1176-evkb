cmake_minimum_required(VERSION 3.24)
project(lvgl_rk055_flip_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rk055_flip_test
    lvgl_rk055_flip_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(lvgl_rk055_flip_test cores MipiDisplay PXP)

target_link_libraries(lvgl_rk055_flip_test.elf LVGL stdc++)

# -DFLIP_DEMO_SINGLE=ON builds the SINGLE-buffer variant of the same animation
# (v1 create, tearing visible) for the Task-6 bench before/after.  The QEMU
# gate never runs that variant; it exists purely so the hardware demonstration
# has an honest "before".
option(FLIP_DEMO_SINGLE "single-buffer tearing demo variant" OFF)
if(FLIP_DEMO_SINGLE)
    target_compile_definitions(lvgl_rk055_flip_test.elf PRIVATE FLIP_DEMO_SINGLE=1)
endif()

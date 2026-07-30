cmake_minimum_required(VERSION 3.24)
project(lvgl_rk055_touch_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)              # Display::fillScreen() paints via the PXP
import_evkb_library(TouchPanel gt911) # controller chosen by the importer
import_evkb_library(Wire)             # LPI2C5 = Wire2, the panel/codec/touch bus

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rk055_touch_test
    lvgl_rk055_touch_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp
    ${_lvgl_dir}/port/lvgl_pxp_copy.cpp)
teensy_target_link_libraries(lvgl_rk055_touch_test cores MipiDisplay PXP TouchPanel Wire)

# Plain-target link for LVGL, exactly as the sibling LVGL gates do.
target_link_libraries(lvgl_rk055_touch_test.elf LVGL stdc++)

# GT911::read() is [[nodiscard]] and this tree carries no -Werror: without this
# a dropped poll result warns and the build SUCCEEDS.  Scoped to this target,
# same rationale as rk055_touch_test's identical line.
target_compile_options(lvgl_rk055_touch_test.elf
                       PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-result>)

cmake_minimum_required(VERSION 3.24)
project(lvgl_smoke_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()

teensy_add_executable(lvgl_smoke_test lvgl_smoke_test.cpp)
teensy_target_link_libraries(lvgl_smoke_test cores)

# LVGL is a plain CMake static-lib target (like CMSIS-DSP), not a
# teensy_add_library() one: teensy_target_link_libraries() rewrites each name to
# <name>.o, which LVGL has no target for, and it would not propagate LVGL's
# PUBLIC include dirs. Link it directly, exactly as arm_math_test does.
target_link_libraries(lvgl_smoke_test.elf LVGL stdc++)

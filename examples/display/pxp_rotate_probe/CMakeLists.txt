cmake_minimum_required(VERSION 3.24)
project(pxp_rotate_probe)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# No panel, no LVGL: cores + PXP, exactly like pxp_blit_test.  The two buffers
# are EXTMEM (SDRAM), which the core's startup brings up before setup().
import_evkb_library(PXP)

teensy_add_executable(pxp_rotate_probe pxp_rotate_probe.cpp)
teensy_target_link_libraries(pxp_rotate_probe cores PXP)

target_link_libraries(pxp_rotate_probe.elf stdc++)

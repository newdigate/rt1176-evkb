cmake_minimum_required(VERSION 3.24)
project(vglite_lvgl_test)

# XRGB8888, like synthui_knob_test and lvgl_rk055_panel_test. Directory scope
# on purpose: these must reach the LVGL, MipiDisplay and SynthUI objects the
# imports below create, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ BUILD-TIME PATH SPLIT, not a runtime one, and the reason is a measured
# defect rather than taste.
#
# LVGL registers its draw units in lv_init() (lv_init.c:236 and :300) and does
# so UNCONDITIONALLY when LV_USE_DRAW_VG_LITE is 1. With the backend compiled
# in but the GPU absent -- which is every QEMU run, since vg_lite_init() SPINS
# on absent hardware and must not be called -- the VG_LITE unit still registers
# and still CLAIMS draw tasks. They then go nowhere. Measured: a full-screen
# flush reporting LVGL_FLUSHED=PASS and LVGL_BYTES=3686400 with a framebuffer
# whose checksum is exactly the FNV of all zeros. There is no automatic
# software fallback once the unit is compiled in.
#
#   build/         EVKB_VGLITE=OFF  software. What the QEMU gate runs, and the
#                                   baseline the fps comparison is measured
#                                   against -- same scene, same build system.
#   build-vglite/  EVKB_VGLITE=ON   GPU. Silicon only; its golden lives in
#                                   transcript_hw_evkb.txt.
#
# The two produce DIFFERENT pixels by construction: hardware antialiasing is
# not LVGL's mask arithmetic, and the GPU build also carries LV_USE_FLOAT=1
# (required by LV_USE_MATRIX, which the backend needs), so coordinates round
# differently. Two goldens, never one.
option(EVKB_VGLITE "Build the GPU path: LVGL's VG_LITE draw unit on the GC355" OFF)
if(EVKB_VGLITE)
    import_evkb_lvgl(VGLITE)
else()
    import_evkb_lvgl()
endif()
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(vglite_lvgl_test
    vglite_lvgl_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(vglite_lvgl_test cores MipiDisplay PXP)

# SynthUI, LVGL and VGLite are plain CMake static-lib targets;
# teensy_target_link_libraries() rewrites names to <name>.o and would lose
# their PUBLIC include dirs. LVGL is named directly as well as transitively
# because this target compiles port/lvgl_mipi_panel.cpp itself.
# VGLite only in the GPU build. Linking it into the software build would drag
# the driver in for nothing and blur exactly the distinction this split exists
# to keep.
if(EVKB_VGLITE)
    target_link_libraries(vglite_lvgl_test.elf SynthUI LVGL VGLite stdc++)
else()
    target_link_libraries(vglite_lvgl_test.elf SynthUI LVGL stdc++)
endif()

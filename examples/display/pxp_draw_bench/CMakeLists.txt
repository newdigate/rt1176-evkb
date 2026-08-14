cmake_minimum_required(VERSION 3.24)

# Inlined toolchain: plain `cmake -B build` works; an explicit
# -DCMAKE_TOOLCHAIN_FILE=... still wins if a caller passes one.
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/rt1170-evkb.toolchain.cmake)
endif()

project(pxp_draw_bench)

# v9: ONE CMakeLists, TWO depth builds (the v7 option pattern).  The default
# build is RGB565; `cmake -B build-32 -DDRAW_BENCH_32=ON` builds the XRGB8888
# variant.  Directory-scope add_compile_definitions so the definitions reach
# the LVGL and MipiDisplay objects created by the imports below, not just this
# target (the v7 lesson: the depth must propagate to lv_conf.h and
# panel_config.h everywhere or the link-time depth probes go red).
option(DRAW_BENCH_32 "XRGB8888 build of the draw bench (default RGB565)" OFF)
if(DRAW_BENCH_32)
    add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)
endif()

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# LVGL is needed even though no display binding is created: the SW comparator
# IS LVGL's own lv_draw_sw_blend() (SW_PATH=lvgl), driven through a fabricated
# draw task -- the same renderer code the future draw unit would displace.
import_evkb_lvgl()
# panels/rk055 selects the panel; the RT1176's single MIPI-DSI host means
# exactly one panel directory may ever be on the include path.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)   # the engine under measurement (fill/blit/overlay)

teensy_add_executable(pxp_draw_bench pxp_draw_bench.cpp)
teensy_target_link_libraries(pxp_draw_bench cores MipiDisplay PXP)

# LVGL is a plain CMake static-lib target: teensy_target_link_libraries()
# rewrites names to <name>.o and would not propagate LVGL's PUBLIC include
# dirs.  Link it directly, exactly as the lvgl_rk055 examples do.
target_link_libraries(pxp_draw_bench.elf LVGL stdc++)

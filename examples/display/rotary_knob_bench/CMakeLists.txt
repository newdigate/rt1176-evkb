cmake_minimum_required(VERSION 3.24)
project(rotary_knob_bench)

# XRGB8888 like every RK055 LVGL example. Directory scope so the definitions
# reach the LVGL and MipiDisplay objects, not just this target.
#
# ★ STRIDE_ALIGN 64 IS LOAD-BEARING FOR THE GPU CELLS, not a tuning knob.
# The GC355 has gcFEATURE_VG_16PIXELS_ALIGNED=1 and gcFEATURE_VG_ERROR_CHECK=1
# (VGLite/Series/gc355/0x0_1216/vg_lite_options.h:80,109), so srcbuf_align_check
# (VGLite/vg_lite.c:1854-1861, called from vg_lite_blit at :4533) REJECTS a
# BGRA8888 blit source whose stride is not a multiple of 16 px x 4 B = 64 B.
# The rotor is 150 px wide; at the default STRIDE_ALIGN 1 its stride is 600,
# 600 % 64 = 24, and every bitmap/strip gpu blit would return
# VG_LITE_INVALID_ARGUMENT -- drawing NOTHING while timing beautifully, which is
# the worst way for a benchmark to be wrong. At 64 the stride is 640 and the
# check passes. LVGL derives the canvas/image stride from this same macro, so
# the CPU-side painter and the GPU-side consumer cannot disagree.
# Both are #ifndef-guarded in port/lv_conf.h:182-188; lvgl_mipi_panel.cpp's
# static_assert(PANEL_PITCH_BYTES % LV_DRAW_BUF_STRIDE_ALIGN == 0) still holds
# (2880 % 64 == 0). Same reasoning the VGLITE build of vglite_lvgl_test records.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4
                        LV_DRAW_BUF_STRIDE_ALIGN=64 LV_DRAW_BUF_ALIGN=64)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ ONE ELF, DELIBERATELY -- and it is safe where vglite_lvgl_test's single
# binary was not. That example compiles LVGL's VG_LITE DRAW UNIT, which
# registers unconditionally in lv_init() and claims tasks it cannot execute on
# absent hardware (black screen, every liveness check green) -- hence its
# build-time split. THIS example never enables LV_USE_DRAW_VG_LITE: LVGL is
# always the software renderer, and the GPU is reached only by DIRECT
# vg_lite_* calls guarded by the chip-ID probe (vglite_probe's pattern, one
# binary two outcomes). The bench needs both engines in one boot -- that is
# what makes the A/B controlled.
import_evkb_lvgl()
import_evkb_vglite()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(rotary_knob_bench
    rotary_knob_bench.cpp
    rk_geometry.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(rotary_knob_bench cores MipiDisplay PXP)

# LVGL and VGLite are plain CMake static-lib targets;
# teensy_target_link_libraries() would rewrite the names and lose their PUBLIC
# include dirs. LVGL is named directly because this target compiles
# port/lvgl_mipi_panel.cpp itself.
target_link_libraries(rotary_knob_bench.elf LVGL VGLite stdc++)

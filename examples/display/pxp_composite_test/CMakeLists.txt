cmake_minimum_required(VERSION 3.24)

# Inlined toolchain: plain `cmake -B build` works; an explicit
# -DCMAKE_TOOLCHAIN_FILE=... still wins if a caller passes one.
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/toolchain/rt1170-evkb.toolchain.cmake)
endif()

project(pxp_composite_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# panels/rk055 selects the panel: it provides panel_config.h, and the RT1176's
# single MIPI-DSI host means exactly one panel directory may ever be on the
# include path.  This example runs at the DEFAULT depth (RGB565, 2 B/px).
# NO LVGL: the PS background is CPU-drawn and the checksum is a local FNV-1a.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)   # the compositor under test (Phase 3 overlay API)

teensy_add_executable(pxp_composite_test pxp_composite_test.cpp)
teensy_target_link_libraries(pxp_composite_test cores MipiDisplay PXP)

target_link_libraries(pxp_composite_test.elf stdc++)

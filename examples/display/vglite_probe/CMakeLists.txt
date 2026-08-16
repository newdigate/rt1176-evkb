cmake_minimum_required(VERSION 3.24)
project(vglite_probe)

# XRGB8888 to match the panel binding the LVGL examples use, so the framebuffer
# format the GPU writes is the one the panel scans out.
add_compile_definitions(PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# On a fresh clone this FATAL_ERRORs by design -- VGLite is unpushed, so the
# example cannot configure and its gate reports SKIP rather than a failure.
# See docs/KNOWN-BROKEN-GATES.md.
import_evkb_vglite()
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

teensy_add_executable(vglite_probe vglite_probe.cpp)
teensy_target_link_libraries(vglite_probe cores MipiDisplay PXP)

# VGLite is a plain CMake static-lib target (see import_evkb_vglite for why
# teensy_target_link_libraries cannot link it).
target_link_libraries(vglite_probe.elf VGLite stdc++)

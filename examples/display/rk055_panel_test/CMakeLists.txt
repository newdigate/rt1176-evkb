cmake_minimum_required(VERSION 3.24)
project(rk055_panel_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# panels/rk055 selects the panel: it is the directory that provides
# panel_config.h, and the RT1176's single MIPI-DSI host means exactly one panel
# directory may ever be on the include path.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)   # Display::fillScreen() paints via the PXP 2D engine

teensy_add_executable(rk055_panel_test rk055_panel_test.cpp)
teensy_target_link_libraries(rk055_panel_test cores MipiDisplay PXP)

target_link_libraries(rk055_panel_test.elf stdc++)

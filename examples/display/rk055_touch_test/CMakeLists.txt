cmake_minimum_required(VERSION 3.24)
project(rk055_touch_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# panels/rk055 selects the panel; the RT1176's single MIPI-DSI host means
# exactly one panel directory may ever be on the include path.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)          # Display::fillScreen() paints via the PXP
# gt911 selects the touch controller.  Unlike the panel this is a convenience,
# not a constraint -- touch controllers are I2C devices and could coexist.
import_evkb_library(TouchPanel gt911)
import_evkb_library(Wire)         # LPI2C5 = Wire2, the panel/codec/touch bus

teensy_add_executable(rk055_touch_test rk055_touch_test.cpp)
teensy_target_link_libraries(rk055_touch_test cores MipiDisplay PXP TouchPanel Wire)

target_link_libraries(rk055_touch_test.elf stdc++)

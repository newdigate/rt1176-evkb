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

# GT911::read() is [[nodiscard]], and that alone is worth nothing here: this
# tree carries no -Werror anywhere, so dropping the result of a poll warns and
# the build SUCCEEDS -- exactly the "warnings scrolling past a green build" the
# driver's own pragma comment exists to prevent.  The library's .cpp pragmas
# cannot help, because they end at its translation unit and the caller is in
# this one.  Measured both ways: with this line a dropped result is
# -Werror=unused-result and the build exits 2; without it, a warning and exit 0.
#
# Scoped to this executable target on purpose: it covers the example's own
# source and nothing else.  Deliberately NOT in teensy-cmake-macros or the
# shared flags -- a tree-wide -Werror would make every unrelated example's
# build hostage to an inherited warning.
target_compile_options(rk055_touch_test.elf
                       PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-result>)

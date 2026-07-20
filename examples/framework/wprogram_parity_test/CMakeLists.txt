cmake_minimum_required(VERSION 3.24)
project(wprogram_parity_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# Stock, unmodified Bounce2 (MIT) — the "stock Teensy library compiles" smoke test.
import_evkb_library(Bounce2)

teensy_add_executable(wprogram_parity_test wprogram_parity_test.cpp)
teensy_target_link_libraries(wprogram_parity_test Bounce2 cores)

target_link_libraries(wprogram_parity_test.elf stdc++)

cmake_minimum_required(VERSION 3.24)
project(sdram_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(sdram_test sdram_test.cpp)
teensy_target_link_libraries(sdram_test cores)

target_link_libraries(sdram_test.elf stdc++)

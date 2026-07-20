cmake_minimum_required(VERSION 3.24)
project(string_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(string_test string_test.cpp)
teensy_target_link_libraries(string_test cores)

target_link_libraries(string_test.elf stdc++)

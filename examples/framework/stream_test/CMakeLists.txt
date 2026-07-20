cmake_minimum_required(VERSION 3.24)
project(stream_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(stream_test stream_test.cpp)
teensy_target_link_libraries(stream_test cores)

target_link_libraries(stream_test.elf stdc++)

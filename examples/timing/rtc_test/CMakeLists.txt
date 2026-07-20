cmake_minimum_required(VERSION 3.24)
project(rtc_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(rtc_test rtc_test.cpp)
teensy_target_link_libraries(rtc_test cores)

target_link_libraries(rtc_test.elf stdc++)

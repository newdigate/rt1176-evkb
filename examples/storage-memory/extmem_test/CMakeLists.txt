cmake_minimum_required(VERSION 3.24)
project(extmem_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(extmem_test extmem_test.cpp)
teensy_target_link_libraries(extmem_test cores)

target_link_libraries(extmem_test.elf stdc++)

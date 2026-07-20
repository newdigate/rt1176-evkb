cmake_minimum_required(VERSION 3.24)
project(dac_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(dac_test dac_test.cpp)
teensy_target_link_libraries(dac_test cores)

target_link_libraries(dac_test.elf stdc++)

# loop() uses sinf() for the HW-phase 100Hz sine sweep; re-link libm after the
# objects (same reason -lstdc++ above works) so sinf resolves.
target_link_libraries(dac_test.elf m)

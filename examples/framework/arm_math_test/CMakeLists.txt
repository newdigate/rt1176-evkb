cmake_minimum_required(VERSION 3.24)
project(arm_math_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_cmsis_dsp()

teensy_add_executable(arm_math_test arm_math_test.cpp)
teensy_target_link_libraries(arm_math_test cores)
target_link_libraries(arm_math_test.elf CMSIS-DSP stdc++)

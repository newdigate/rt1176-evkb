cmake_minimum_required(VERSION 3.24)
project(cm4_intr_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


# CM4 image via the Phase-2B first-class dual-target macro.
teensy_add_cm4_image(cm4_intr
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_executable(cm4_intr_test cm4_intr_test.cpp)
teensy_target_link_libraries(cm4_intr_test cores)
target_link_libraries(cm4_intr_test.elf stdc++)
teensy_target_link_cm4_image(cm4_intr_test cm4_intr)

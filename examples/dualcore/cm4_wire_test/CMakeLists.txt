cmake_minimum_required(VERSION 3.24)
project(cm4_wire_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Wire EVKB_WIRE_DIR)


# CM4 image via the Phase-2B first-class dual-target macro (emits cm4_wire.h).
teensy_add_cm4_image(cm4_wire
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            ${EVKB_WIRE_DIR}/lpi2c1176.c
    INCLUDE_DIRS ${EVKB_WIRE_DIR})

teensy_add_executable(cm4_wire_test cm4_wire_test.cpp)
teensy_target_link_libraries(cm4_wire_test cores)
target_link_libraries(cm4_wire_test.elf stdc++)
teensy_target_link_cm4_image(cm4_wire_test cm4_wire)

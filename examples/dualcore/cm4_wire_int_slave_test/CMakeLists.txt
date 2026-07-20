cmake_minimum_required(VERSION 3.24)
project(cm4_wire_int_slave_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Wire EVKB_WIRE_DIR)


# One slave source, two world bindings (spec §4.2): QEMU -> LPI2C2 persona
# (IRQ 33, bridged bus); HW -> LPI2C1 (IRQ 32, Arduino header A4/A5).
teensy_add_cm4_image(cm4_wire_int_slave_q
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            ${EVKB_WIRE_DIR}/lpi2c1176.c
    INCLUDE_DIRS ${EVKB_WIRE_DIR})

teensy_add_cm4_image(cm4_wire_int_slave_h
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            ${EVKB_WIRE_DIR}/lpi2c1176.c
    INCLUDE_DIRS ${EVKB_WIRE_DIR}
    DEFINES WIRE_SLAVE_WORLD_HW=1)

# QEMU-world CM7: boots the _q image AND masters LPI2C1 itself.
teensy_add_executable(cm4_wire_int_slave_test cm4_wire_int_slave_test.cpp
    ${EVKB_WIRE_DIR}/lpi2c1176.c)
teensy_target_link_libraries(cm4_wire_int_slave_test cores)
target_link_libraries(cm4_wire_int_slave_test.elf stdc++)
target_include_directories(cm4_wire_int_slave_test.elf PRIVATE ${EVKB_WIRE_DIR})
teensy_target_link_cm4_image(cm4_wire_int_slave_test cm4_wire_int_slave_q)

# HW-world CM7: boots the _h image, reports only (the external master owns the bus).
teensy_add_executable(cm4_wire_int_slave_test_hw cm4_wire_int_slave_test.cpp
    ${EVKB_WIRE_DIR}/lpi2c1176.c)
teensy_target_link_libraries(cm4_wire_int_slave_test_hw cores)
target_link_libraries(cm4_wire_int_slave_test_hw.elf stdc++)
target_include_directories(cm4_wire_int_slave_test_hw.elf PRIVATE ${EVKB_WIRE_DIR})
target_compile_definitions(cm4_wire_int_slave_test_hw.elf PRIVATE WIRE_SLAVE_WORLD_HW=1)
teensy_target_link_cm4_image(cm4_wire_int_slave_test_hw cm4_wire_int_slave_h)

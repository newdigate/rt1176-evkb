cmake_minimum_required(VERSION 3.24)
project(cm4_sai_irq_probe)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

teensy_add_cm4_image(cm4_sai_probe
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_executable(cm4_sai_irq_probe cm4_sai_irq_probe.cpp)
teensy_target_link_libraries(cm4_sai_irq_probe cores)
target_link_libraries(cm4_sai_irq_probe.elf stdc++)
teensy_target_link_cm4_image(cm4_sai_irq_probe cm4_sai_probe)

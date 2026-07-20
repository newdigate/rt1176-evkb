cmake_minimum_required(VERSION 3.24)
project(cm4_hotswap2_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_cm4_image(cm4_hs2_a
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4_a.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
    DEFINES HS_IDENTITY=0xA1A1A1A1u)
teensy_add_cm4_image(cm4_hs2_b
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4_b.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
    DEFINES HS_IDENTITY=0xB2B2B2B2u)

teensy_add_executable(cm4_hotswap2_test cm4_hotswap2_test.cpp)
teensy_target_link_libraries(cm4_hotswap2_test cores)
target_link_libraries(cm4_hotswap2_test.elf stdc++)
teensy_target_link_cm4_image(cm4_hotswap2_test cm4_hs2_a)
teensy_target_link_cm4_image(cm4_hotswap2_test cm4_hs2_b)

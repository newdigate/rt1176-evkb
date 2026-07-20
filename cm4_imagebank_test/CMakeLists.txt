cmake_minimum_required(VERSION 3.24)
project(cm4_imagebank_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# One ITCM-slot size drives BOTH the linker ORIGIN (per image, via
# teensy_add_cm4_slot_image) and the runtime CM4_SLOT_STAGE() the CM7 hands the
# bank -- single source of truth. 0x1000 = 4 KB -> 32 slots; images are ~700 B.
set(CM4_SLOT_SIZE 0x1000)

set(CM4_SRCS ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
             ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_cm4_slot_image(cm4_ib_a SLOT 0 SLOT_SIZE ${CM4_SLOT_SIZE}
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xA1A1A1A1u)
teensy_add_cm4_slot_image(cm4_ib_b SLOT 1 SLOT_SIZE ${CM4_SLOT_SIZE}
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xB2B2B2B2u)
teensy_add_cm4_slot_image(cm4_ib_c SLOT 2 SLOT_SIZE ${CM4_SLOT_SIZE}
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xC3C3C3C3u)
teensy_add_cm4_slot_image(cm4_ib_d SLOT 0 SLOT_SIZE ${CM4_SLOT_SIZE}  # shares A's slot
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xD4D4D4D4u)

teensy_add_executable(cm4_imagebank_test cm4_imagebank_test.cpp)
target_compile_definitions(cm4_imagebank_test.elf PRIVATE CM4_SLOT_SIZE=${CM4_SLOT_SIZE})
teensy_target_link_libraries(cm4_imagebank_test cores)
target_link_libraries(cm4_imagebank_test.elf stdc++)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_a)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_b)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_c)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_d)

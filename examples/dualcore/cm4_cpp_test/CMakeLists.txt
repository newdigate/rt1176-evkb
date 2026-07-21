cmake_minimum_required(VERSION 3.24)
project(cm4_cpp_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# First C++ CM4 image: main_cm4.cpp goes through the macro's g++ path;
# runtime_stubs.c + startup_cm4.S keep today's byte-identical gcc path.
teensy_add_cm4_image(cm4_cpp
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.cpp
            ${CMAKE_CURRENT_LIST_DIR}/cm4/runtime_stubs.c)

teensy_add_executable(cm4_cpp_test cm4_cpp_test.cpp)
teensy_target_link_libraries(cm4_cpp_test cores)
target_link_libraries(cm4_cpp_test.elf stdc++)
teensy_target_link_cm4_image(cm4_cpp_test cm4_cpp)

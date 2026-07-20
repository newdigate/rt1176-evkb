cmake_minimum_required(VERSION 3.24)
project(usb_mouse_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(usb_mouse_test usb_mouse_test.cpp)
teensy_target_link_libraries(usb_mouse_test cores)
target_link_libraries(usb_mouse_test.elf stdc++)

# Hardware-observation variant (periodic cursor jiggle + CDC echo); not used by the QEMU gate.
teensy_add_executable(usb_mouse_hw usb_mouse_hw.cpp)
teensy_target_link_libraries(usb_mouse_hw cores)
target_link_libraries(usb_mouse_hw.elf stdc++)

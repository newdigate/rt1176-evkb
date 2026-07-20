cmake_minimum_required(VERSION 3.24)
project(usb_joystick_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(usb_joystick_test usb_joystick_test.cpp)
teensy_target_link_libraries(usb_joystick_test cores)
target_link_libraries(usb_joystick_test.elf stdc++)

# Hardware-observation variant (periodic button/axis stepping + CDC echo); not the QEMU gate.
teensy_add_executable(usb_joystick_hw usb_joystick_hw.cpp)
teensy_target_link_libraries(usb_joystick_hw cores)
target_link_libraries(usb_joystick_hw.elf stdc++)

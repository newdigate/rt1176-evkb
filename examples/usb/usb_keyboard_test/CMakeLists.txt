cmake_minimum_required(VERSION 3.24)
project(usb_keyboard_test)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH / COREPATH come from the
# toolchain file (toolchain/rt1170-evkb.toolchain.cmake); fallback keeps 117.
set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)


teensy_add_executable(usb_keyboard_test usb_keyboard_test.cpp)
teensy_target_link_libraries(usb_keyboard_test cores)
target_link_libraries(usb_keyboard_test.elf stdc++)

# Hardware-observation variant (periodic type + CDC echo); not used by the QEMU gate.
teensy_add_executable(usb_keyboard_hw usb_keyboard_hw.cpp)
teensy_target_link_libraries(usb_keyboard_hw cores)
target_link_libraries(usb_keyboard_hw.elf stdc++)

cmake_minimum_required(VERSION 3.24)
project(eeprom_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(EEPROM)

teensy_add_executable(eeprom_test eeprom_test.cpp)
teensy_target_link_libraries(eeprom_test cores EEPROM)

target_link_libraries(eeprom_test.elf stdc++)

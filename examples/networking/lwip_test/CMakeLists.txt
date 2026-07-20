cmake_minimum_required(VERSION 3.24)
project(lwip_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_library(lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
teensy_add_executable(lwip_test lwip_test.cpp udpecho_raw.c tcpecho_raw.c)
teensy_target_link_libraries(lwip_test cores lwip)
target_link_libraries(lwip_test.elf stdc++)

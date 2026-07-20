cmake_minimum_required(VERSION 3.24)
project(native_ethernet_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
# FNET: flat non-recursive globs — every source dir listed explicitly. All files
# are preprocessor-gated, so over-inclusion only costs compile time. third_party
# (mbedTLS) is deliberately NOT compiled (FNET_CFG_TLS=0 for __IMXRT1176__).
import_evkb_library(fnet
    stack
    service service/dhcp service/dns service/autoip service/link service/mdns service/ping
    port port/cpu port/cpu/mimxrt port/netif/fec)
import_evkb_library(nativeethernet src src/utility)
teensy_add_executable(native_ethernet_test native_ethernet_test.cpp)
teensy_target_link_libraries(native_ethernet_test cores fnet nativeethernet)
target_link_libraries(native_ethernet_test.elf stdc++)

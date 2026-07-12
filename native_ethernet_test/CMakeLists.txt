cmake_minimum_required(VERSION 3.24)
project(native_ethernet_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
# FNET: flat non-recursive globs — every source dir listed explicitly. All files
# are preprocessor-gated, so over-inclusion only costs compile time. third_party
# (mbedTLS) is deliberately NOT compiled (FNET_CFG_TLS=0 for __IMXRT1176__).
import_arduino_library(fnet $ENV{HOME}/Development/FNET/src
    stack
    service service/dhcp service/dns service/autoip service/link service/mdns service/ping
    port port/cpu port/cpu/mimxrt port/netif/fec)
import_arduino_library(nativeethernet $ENV{HOME}/Development/NativeEthernet src src/utility)
teensy_add_executable(native_ethernet_test native_ethernet_test.cpp)
teensy_target_link_libraries(native_ethernet_test cores fnet nativeethernet)
target_link_libraries(native_ethernet_test.elf stdc++)

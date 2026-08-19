cmake_minimum_required(VERSION 3.24)
project(m2_rx_demo)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# sdio + iw416 only. No lwip: this example moves raw 802.3 frames and has no
# IP stack, deliberately -- it is the data path under test, not a network app.
import_evkb_library(M2Radio sdio iw416)

# NO firmware-blob option and NO credential options here, unlike m2_sdio_probe
# and m2_lwip_test. Both would be dead weight: this image never downloads
# firmware (the QEMU model's fw-preboot property stands in for that, see the
# .cpp header) and never associates. Keeping them out is also what lets the
# three gates run against the ONE build a fresh clone can make.
teensy_add_executable(m2_rx_demo m2_rx_demo.cpp)
teensy_target_link_libraries(m2_rx_demo cores M2Radio)
target_link_libraries(m2_rx_demo.elf stdc++)

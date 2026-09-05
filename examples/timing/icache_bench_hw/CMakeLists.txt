cmake_minimum_required(VERSION 3.24)
project(icache_bench_hw)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH are supplied by the toolchain
# file (../../../toolchain/rt1170-evkb.toolchain.cmake); fallback for a bare configure.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# Same M2Radio manifest as audio/bt_tone_test.  Only bt/Sbc.cpp.obj is referenced
# from this sketch, so nothing else in the archive links and no IW416 firmware
# blob symbol is needed (nothing in M2Radio references one -- grep'd 2026-09-05).
import_evkb_library(M2Radio sdio iw416 hci bt)

# The reference cell.  Default OFF: Sbc.cpp.obj is routed to FLASH below so the
# bench measures the flash-resident encoder with the I-cache off and on.  ON
# leaves Sbc in ITCM (the core script's default .text placement) for the
# ITCM number -- build it in a SECOND directory (build-sbc-itcm), never
# reconfigure build/ with it, so both ELFs stay side by side.
option(ICACHE_BENCH_SBC_ITCM "Leave Sbc.cpp in ITCM (the reference cell) instead of routing it to FLASH" OFF)

teensy_add_executable(icache_bench_hw icache_bench_hw.cpp)
teensy_target_link_libraries(icache_bench_hw cores M2Radio)
target_link_libraries(icache_bench_hw.elf stdc++)
target_link_libraries(icache_bench_hw.elf m)

if(NOT ICACHE_BENCH_SBC_ITCM)
    # Route libM2Radio's Sbc.cpp.obj to FLASH (XIP): the SAME derived-linker-script
    # mechanism display/acid_box uses for its M2_BT_OUT build -- the core's
    # imxrt1176.ld is read at configure time, the one input pattern is appended
    # beside the libLVGL_flash.a rule, and ONLY this target links the copy, so it
    # never drifts from the core script and touches no other example.
    get_target_property(_icb_lf icache_bench_hw.elf LINK_FLAGS)
    string(REGEX MATCH "-T([^ ]*imxrt1176[.]ld)" _icb_ignore "${_icb_lf}")
    set(_icb_coreld "${CMAKE_MATCH_1}")
    if(_icb_coreld AND EXISTS "${_icb_coreld}")
        file(READ "${_icb_coreld}" _icb_ld)
        string(REPLACE "*libLVGL_flash.a:(.text*)"
                       "*libLVGL_flash.a:(.text*)\n\t\t*libM2Radio*.a:Sbc.cpp.obj(.text* .fastrun)"
                       _icb_ld "${_icb_ld}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/icache_bench_hw.ld" "${_icb_ld}")
        string(REPLACE "${_icb_coreld}"
                       "${CMAKE_CURRENT_BINARY_DIR}/icache_bench_hw.ld" _icb_lf "${_icb_lf}")
        set_target_properties(icache_bench_hw.elf PROPERTIES LINK_FLAGS "${_icb_lf}")
        message(STATUS "icache_bench_hw: Sbc.cpp.obj routed to FLASH via ${CMAKE_CURRENT_BINARY_DIR}/icache_bench_hw.ld")
    else()
        message(FATAL_ERROR "icache_bench_hw: could not find the core imxrt1176.ld in LINK_FLAGS to derive the Sbc-to-FLASH linker script (the flash cell IS the bench)")
    endif()
endif()

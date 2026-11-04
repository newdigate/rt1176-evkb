# Toolchain file for the Nosh board (i.MX RT1176, Cortex-M7; a cut-down
# MIMXRT1170-EVKB: 128 MB SDRAM @ 132 MHz, microSD on USDHC2, no Ethernet).
# Same chip and core directory as rt1170-evkb.toolchain.cmake; the board is
# selected through TEENSY_BOARD, which the macros turn into -DARDUINO_NOSH and
# imxrt1176_nosh.ld. Pair it with -DEVKB_BOARD=nosh (evkb.cmake checks both).
# Board identity + compiler only — COREPATH is owned by evkb.cmake, which
# resolves the core under TEENSY_LIB_ROOT (local-first, pinned fallback).
set(TEENSY_VERSION 117 CACHE STRING "RT1176 / Nosh" FORCE)
set(TEENSY_BOARD NOSH CACHE STRING "Nosh board (TEENSY_VERSION 117 family)" FORCE)
set(CPU_CORE_SPEED 996000000 CACHE STRING "RT1176 M7 core clock" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "") # for linking stdc++ (nano)
if(DEFINED ENV{ARM_TOOLCHAIN_BIN})
    set(COMPILERPATH "$ENV{ARM_TOOLCHAIN_BIN}/")   # portable override
elseif(IS_DIRECTORY "/Applications/ARM_10/bin")
    set(COMPILERPATH "/Applications/ARM_10/bin/")
elseif(IS_DIRECTORY "/Applications/ARM/bin")
    set(COMPILERPATH "/Applications/ARM/bin/")
else()
    find_program(ARM_GCC_PATH arm-none-eabi-gcc)
    if(ARM_GCC_PATH)
        get_filename_component(ARM_GCC_DIR "${ARM_GCC_PATH}" DIRECTORY)
        set(COMPILERPATH "${ARM_GCC_DIR}/")
    else()
        set(COMPILERPATH "/Applications/ARM_10/bin/")
    endif()
endif()
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

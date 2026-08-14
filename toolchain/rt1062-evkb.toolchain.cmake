# Toolchain file for the NXP MIMXRT1060-EVKB (i.MX RT1062, Cortex-M7).
# Selects TEENSY_VERSION 42, which teensy-cmake-macros maps to the teensy4
# core and imxrt1060_evkb.ld. Board identity + compiler only — COREPATH is
# owned by evkb.cmake (resolved under TEENSY_LIB_ROOT, local-first).
set(TEENSY_VERSION 42 CACHE STRING "RT1062 / MIMXRT1060-EVKB" FORCE)
set(CPU_CORE_SPEED 600000000 CACHE STRING "RT1062 M7 core clock" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "")
if(DEFINED ENV{ARM_TOOLCHAIN_BIN})
    set(COMPILERPATH "$ENV{ARM_TOOLCHAIN_BIN}/")
else()
    set(COMPILERPATH "/Applications/ARM_10/bin/")
endif()
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

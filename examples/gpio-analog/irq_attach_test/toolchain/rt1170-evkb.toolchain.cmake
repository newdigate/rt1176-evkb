# Toolchain file for the NXP MIMXRT1170-EVKB (i.MX RT1176, Cortex-M7).
# Mirrors the working rt1060-evkb toolchain but selects the imxrt1176 core.
set(TEENSY_VERSION 117 CACHE STRING "RT1176 / MIMXRT1170-EVKB" FORCE)
set(CPU_CORE_SPEED 996000000 CACHE STRING "RT1176 M7 core clock" FORCE)

# Point the macros at the LOCAL imxrt1176 core (not the fetched teensy4 fork).
# COREPATH must end with a trailing slash: the macros build LINKER_FILE as
# "${COREPATH}imxrt1176.ld".
# NB: cache type STRING, not PATH -- CMake normalises PATH cache entries and
# strips the trailing slash, but the macros build LINKER_FILE by raw string
# concat ("${COREPATH}imxrt1176.ld"), so the trailing slash must survive.
get_filename_component(_evkb_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(COREPATH "${_evkb_root}/cores/imxrt1176/" CACHE STRING "imxrt1176 core path" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "") # for linking stdc++ (nano)
set(COMPILERPATH "/Applications/ARM_10/bin/")
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

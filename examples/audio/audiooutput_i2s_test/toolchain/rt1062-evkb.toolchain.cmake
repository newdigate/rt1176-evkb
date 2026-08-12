# Toolchain file for the NXP MIMXRT1060-EVKB (i.MX RT1062, Cortex-M7).
# Mirrors rt1170-evkb.toolchain.cmake but selects TEENSY_VERSION 42, which
# teensy-cmake-macros already maps to the teensy4 core and imxrt1060_evkb.ld
# (CMakeLists.include.txt:86-89) -- no macro change is needed for this board.
set(TEENSY_VERSION 42 CACHE STRING "RT1062 / MIMXRT1060-EVKB" FORCE)
set(CPU_CORE_SPEED 600000000 CACHE STRING "RT1062 M7 core clock" FORCE)

# Point the macros at the LOCAL teensy4 core. COREPATH must end with a trailing
# slash: the macros build LINKER_FILE as "${COREPATH}imxrt1060_evkb.ld".
# NB: cache type STRING, not PATH -- CMake normalises PATH cache entries and
# strips the trailing slash, but the macros concatenate raw strings.
get_filename_component(_evkb_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(COREPATH "${_evkb_root}/cores/teensy4/" CACHE STRING "teensy4 core path" FORCE)

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

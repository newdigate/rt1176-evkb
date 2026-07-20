cmake_minimum_required(VERSION 3.24)

# --- Toolchain bootstrap (before project()) — inlined (like SdFat/tests/*) so a
# plain `cmake -B build -S .` cross-compiles for the RT1176 M7 with no extra
# flags. Mirrors evkb/*/toolchain/rt1170-evkb.toolchain.cmake. ---
set(TEENSY_VERSION 117 CACHE STRING "RT1176 / MIMXRT1170-EVKB" FORCE)
set(CPU_CORE_SPEED 996000000 CACHE STRING "RT1176 M7 core clock" FORCE)
set(EVKB ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(COREPATH "${EVKB}/cores/imxrt1176/" CACHE STRING "imxrt1176 core path" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "")
set(COMPILERPATH "/Applications/ARM_10/bin/")
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
# -----------------------------------------------------------------------------

project(sd_wav_play_test)

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${EVKB}/cores/imxrt1176)
import_arduino_library(Wire  $ENV{HOME}/Development/Wire)
import_arduino_library(SPI   $ENV{HOME}/Development/SPI)
# SdFat is a 1.5-format lib: enumerate every src subdir that carries sources (src
# first for the include root). src/SpiDriver is required because SDClass::begin()
# references the SPI-SD path (runtime if, not strippable), even though the gate
# uses only SDIO. (Same as SdFat/tests/sd_fs_test.)
import_arduino_library(SdFat $ENV{HOME}/Development/SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib
    src/iostream src/SpiDriver)
import_arduino_library(SD    $ENV{HOME}/Development/PaulS_SD src)

teensy_add_executable(sd_wav_play_test sd_wav_play_test.cpp)
teensy_target_link_libraries(sd_wav_play_test cores Wire SPI SdFat SD)

# Audio nodes are cherry-picked (the evkb gates do NOT glob the Audio fork).
# memcpy_audio.S needs the LANGUAGE C + assembler-with-cpp trick (as in
# audiooutput_i2s_test) since it's added directly, not via import_arduino_library.
# spi_interrupt.cpp is required (first gate to need it): play_sd_wav.cpp includes
# spi_interrupt.h, and because newdigate/SPI defines SPI_HAS_NOTUSINGINTERRUPT the
# header takes its extern-AudioUsingSPICount branch -- that counter is defined in
# spi_interrupt.cpp (upstream it ships in the Audio lib). Prior audio gates never
# pulled in AudioPlaySdWav, the only node that touches the SPI-interrupt path.
target_sources(sd_wav_play_test.elf PRIVATE
    sd_wav_play_test.cpp
    $ENV{HOME}/Development/Audio/play_sd_wav.cpp
    $ENV{HOME}/Development/Audio/output_i2s.cpp
    $ENV{HOME}/Development/Audio/control_wm8962.cpp
    $ENV{HOME}/Development/Audio/spi_interrupt.cpp
    $ENV{HOME}/Development/Audio/memcpy_audio.S
)
set_source_files_properties($ENV{HOME}/Development/Audio/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
)
target_include_directories(sd_wav_play_test.elf PRIVATE $ENV{HOME}/Development/Audio)
target_link_libraries(sd_wav_play_test.elf stdc++)
target_link_libraries(sd_wav_play_test.elf m)

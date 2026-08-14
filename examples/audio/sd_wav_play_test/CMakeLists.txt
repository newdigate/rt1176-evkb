cmake_minimum_required(VERSION 3.24)

# Board + compiler come from the shared toolchain file, included BEFORE
# project() — same mechanism as passing -DCMAKE_TOOLCHAIN_FILE, so a plain
# `cmake -B build` still cross-compiles with no extra flags.
include(${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/rt1170-evkb.toolchain.cmake)

project(sd_wav_play_test)

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Audio EVKB_AUDIO_DIR)

import_evkb_library(Wire)
import_evkb_library(SPI)
# SdFat is a 1.5-format lib: enumerate every src subdir that carries sources (src
# first for the include root). src/SpiDriver is required because SDClass::begin()
# references the SPI-SD path (runtime if, not strippable), even though the gate
# uses only SDIO. (Same as SdFat/tests/sd_fs_test.)
import_evkb_library(SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib
    src/iostream src/SpiDriver)
import_evkb_library(SD src)

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
    ${EVKB_AUDIO_DIR}/play_sd_wav.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/control_wm8962.cpp
    ${EVKB_AUDIO_DIR}/spi_interrupt.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S
)
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
)
target_include_directories(sd_wav_play_test.elf PRIVATE ${EVKB_AUDIO_DIR})
target_link_libraries(sd_wav_play_test.elf stdc++)
target_link_libraries(sd_wav_play_test.elf m)

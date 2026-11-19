cmake_minimum_required(VERSION 3.24)
project(dj_deck)

# The landscape (rotated present) pipeline is 32 bpp only.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

if(NOT EVKB_BOARD STREQUAL "nosh")
    message(FATAL_ERROR "dj_deck builds for the Nosh board only:\n"
        "  cmake -B build-nosh -DEVKB_BOARD=nosh -DTEENSY_BOARD=NOSH "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/nosh.toolchain.cmake")
endif()

# SdFat in full, BEFORE the Audio import (which pulls it in for its headers
# only): the browser and the loader really use the card.
import_evkb_library(Wire)
import_evkb_library(SPI)
import_evkb_library(SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib
    src/iostream src/SpiDriver)
import_evkb_library(SD src)
import_evkb_audio_full()
import_evkb_lvgl()
import_evkb_synthui(VGLITE)
import_evkb_audio_codecs()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)
import_evkb_library(TouchPanel gt911)
evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(dj_deck
    dj_deck.cpp
    board_io.cpp
    loader.cpp
    peaks.cpp
    track_source.cpp
    ui_deck.cpp
    ui_browser.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_pxp_copy.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp)
target_compile_options(dj_deck.elf PRIVATE -std=gnu++17)
target_link_options(dj_deck.elf PRIVATE -Wl,--print-memory-usage)
teensy_target_link_libraries(dj_deck
    cores Audio Wire SPI SdFat SD SerialFlash MipiDisplay PXP TouchPanel)
target_link_libraries(dj_deck.elf SynthUI AudioCodecs LVGL VGLite CMSIS-DSP stdc++ m)

# ---- what runs from ITCM and what from flash ---------------------------------
# Everything linked here is about 300 KB of code and ITCM is 256 KB.  The core's
# linker script has one hook for sending an archive's code to flash (XIP, behind
# the I-cache); a derived script widens it, as examples/display/acid_box does.
# Kept in ITCM, because they run per sample, per pixel column or per frame:
# LVGL, the decoders and analysis engines (AudioCodecs), the audio player and
# I2S output, the waveform widget, and this example's decode path.
# Sent to flash, because they run per touch, per file or once: the SD stack,
# I2C, the display and touch drivers, the other SynthUI widgets, the rest of
# the Audio library, the two screens and the loader's bookkeeping.
get_target_property(_dj_lf dj_deck.elf LINK_FLAGS)
string(REGEX MATCH "-T([^ ]*imxrt1176[^ ]*[.]ld)" _dj_ignore "${_dj_lf}")
set(_dj_coreld "${CMAKE_MATCH_1}")
if(NOT _dj_coreld OR NOT EXISTS "${_dj_coreld}")
    message(FATAL_ERROR "dj_deck: cannot find the core linker script in LINK_FLAGS (${_dj_lf})")
endif()
file(READ "${_dj_coreld}" _dj_ld)
set(_dj_routing "*libLVGL_flash.a:(.text*)
		*libSdFat*.a:(.text*)
		*libSD.*a:(.text*)
		*libSPI*.a:(.text*)
		*libWire*.a:(.text*)
		*libSerialFlash*.a:(.text*)
		*libUSBHost_t36*.a:(.text*)
		*libEEPROM*.a:(.text*)
		*libMipiDisplay*.a:(.text*)
		*libTouchPanel*.a:(.text*)
		*libVGLite*.a:(.text*)
		*libCMSIS-DSP*.a:(.text*)
		*libSynthUI*.a:(EXCLUDE_FILE(*synthui_waveform.cpp.obj) .text*)
		*libAudio.*a:(EXCLUDE_FILE(*play_buffer.cpp.o* *output_i2s.cpp.o* *memcpy_audio.S.o*) .text*)
		*ui_deck.cpp.obj(.text*)
		*ui_browser.cpp.obj(.text*)
		*board_io.cpp.obj(.text*)
		*dj_deck.cpp.obj(.text*)
		*lvgl_gt911_indev.cpp.obj(.text*)
		*loader.cpp.obj(.text*)
		*tempo_generator.cpp.obj(.text*)
		*tempo_refiner.cpp.obj(.text*)
		*tempo_reader.cpp.obj(.text*)
		*preview_reader.cpp.obj(.text*)
		*spectrum_reader.cpp.obj(.text*)
		*md5.cpp.obj(.text*)
		*metadata.cpp.obj(.text*)
		*lv_theme_default.c.obj(.text*)
		/* Constant data goes to DTCM by default, and the two fonts are 80 KB of it.  Glyphs are read
		   a few dozen at a time, ten times a second: from flash is fine, and DTCM is needed for the
		   decoder arena and the stack. */
		*lv_font_montserrat_*.c.obj(.rodata*)")
string(FIND "${_dj_ld}" "*libLVGL_flash.a:(.text*)" _dj_found)
if(_dj_found EQUAL -1)
    message(FATAL_ERROR "dj_deck: the flash-routing hook '*libLVGL_flash.a:(.text*)' is gone from ${_dj_coreld}; re-anchor this rule")
endif()
string(REPLACE "*libLVGL_flash.a:(.text*)" "${_dj_routing}" _dj_ld "${_dj_ld}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dj_deck.ld" "${_dj_ld}")
set_property(TARGET dj_deck.elf APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/dj_deck.ld")
string(REPLACE "${_dj_coreld}" "${CMAKE_CURRENT_BINARY_DIR}/dj_deck.ld" _dj_lf "${_dj_lf}")
set_target_properties(dj_deck.elf PROPERTIES LINK_FLAGS "${_dj_lf}")

# Bench options: -DDJ_FPS=1 prints the frame rate once a second while a track loads;
# -DDJ_LOADER_STEP_US=<n> sets the loader's time slice per loop (default 16000).
if(DJ_FPS)
    target_compile_definitions(dj_deck.elf PRIVATE DJ_FPS=1)
endif()
if(DJ_LOADER_STEP_US)
    target_compile_definitions(dj_deck.elf PRIVATE DJ_LOADER_STEP_US=${DJ_LOADER_STEP_US})
endif()
# -DDJ_BENCH=1: load the audio_codecs_probe clips from the card with no touch input
# (see bench_step() in dj_deck.cpp).  The clips are linked into flash.
if(DJ_BENCH)
    set(_probe ${CMAKE_CURRENT_LIST_DIR}/../audio_codecs_probe)
    target_sources(dj_deck.elf PRIVATE ${_probe}/clips_data.S)
    set_source_files_properties(${_probe}/clips_data.S PROPERTIES LANGUAGE C
        COMPILE_OPTIONS "-x;assembler-with-cpp;-I${_probe}")
    target_compile_definitions(dj_deck.elf PRIVATE DJ_BENCH=1 DJ_FPS=1)
endif()

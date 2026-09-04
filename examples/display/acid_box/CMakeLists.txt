cmake_minimum_required(VERSION 3.24)
project(acid_box)

# XRGB8888 exactly like synthui_step_test / synthui_knob_test.  Directory scope
# on purpose: the definitions must reach the LVGL, MipiDisplay AND SynthUI
# objects created by the imports below, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ FIRST EXAMPLE IN THIS TREE TO LINK THE AUDIO LIBRARY AND LVGL TOGETHER.
# Every display example so far was display-only and every audio example was
# audio-only, so the combined include path, the combined symbol set and the
# combined RAM footprint had never been exercised.  Proving that link is half
# of what this slice of the capstone is for; the other half is the note pump.
import_evkb_audio_full()

import_evkb_lvgl()
# VGLITE flavor: compiles SynthUI's rotary-knob GPU compositor and links the
# VGLite driver.  LVGL stays the SOFTWARE renderer (one ELF, both engines --
# the rotary_knob_bench pattern): the GC355 is reached only by direct vg_lite
# calls behind the chip-ID probe, so this same image is safe in QEMU.
import_evkb_synthui(VGLITE)
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)              # Display::fillScreen() paints via the PXP
import_evkb_library(TouchPanel gt911) # controller chosen by the importer

# --- Bluetooth A2DP audio output (opt-in) -----------------------------------
# OFF by default: nothing below compiles in and the ELF is byte-identical, so
# the golden run_qemu.sh gate is untouched.  ON links the M.2 BT stack and adds
# an AudioOutputBluetooth sink alongside the I2S/WM8962 path (see
# docs/superpowers/specs/2026-09-04-acid-box-bluetooth-output-design.md).
option(M2_BT_OUT "Stream the synth audio to a Bluetooth A2DP sink (bench)" OFF)
if(M2_BT_OUT)
    add_definitions(-DM2_BT_OUT=1)
    import_evkb_library(M2Radio sdio iw416 hci bt)

    # The BT bench knobs the headset path needs (same as audio/bt_tone_test).
    option(M2_BT_UART_DNLD "Run the BT-only V3 UART firmware download" ON)
    if(M2_BT_UART_DNLD)
    else()
        add_definitions(-DM2_BT_NO_UART_DNLD=1)
    endif()
    option(M2_BT_WAKE_PULSE "Pulse the boot-sleep wake line before the BT firmware download" ON)
    if(M2_BT_WAKE_PULSE)
        add_definitions(-DM2_BT_WAKE_PULSE=1)
    endif()
    option(M2_BT_RTS_FLOW "Hardware RXRTSE flow control on LPUART2 (Serial2.attachRts)" ON)
    set(M2_BT_RTS_WATER "1" CACHE STRING "MODIR RTSWATER watermark for M2_BT_RTS_FLOW (0..3)")
    if(M2_BT_RTS_FLOW)
        add_definitions(-DM2_BT_RTS_FLOW=1 -DM2_BT_RTS_WATER=${M2_BT_RTS_WATER})
    endif()
    set(M2_BT_TARGET_NAME "" CACHE STRING "Prefer the A2DP sink whose inquiry name contains this substring")
    if(NOT M2_BT_TARGET_NAME STREQUAL "")
        add_definitions(-DM2_BT_TARGET_NAME="${M2_BT_TARGET_NAME}")
    endif()
    option(M2_BT_LEGACY_PIN "Write_Simple_Pairing_Mode=0: legacy PIN pairing (1234)" OFF)
    if(M2_BT_LEGACY_PIN)
        add_definitions(-DM2_BT_LEGACY_PIN=1)
    endif()
    option(M2_BT_CONNECT_RETRY "Retry A2dpSource::connect() from loop() until it succeeds" ON)
    if(M2_BT_CONNECT_RETRY)
        add_definitions(-DM2_BT_CONNECT_RETRY=1)
    endif()
    option(M2_BT_FAST_BAUD "Switch HCI to M2_BT_FAST_BAUD_RATE after identity" ON)
    set(M2_BT_FAST_BAUD_RATE "3000000" CACHE STRING "Rate for M2_BT_FAST_BAUD")
    if(M2_BT_FAST_BAUD)
        add_definitions(-DM2_BT_FAST_BAUD=${M2_BT_FAST_BAUD_RATE})
    endif()

    # The BT-only UART firmware image (NOT vendored), same rule + synthetic fallback
    # as bt_tone_test/m2_hci_probe.
    set(M2RADIO_IW416_BT_FW "" CACHE FILEPATH "IW416 BT-only UART firmware .bin.inc from an NXP SDK")
    if(M2RADIO_IW416_BT_FW AND EXISTS "${M2RADIO_IW416_BT_FW}")
        message(STATUS "IW416 BT firmware: ${M2RADIO_IW416_BT_FW}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_BT_FW}\"\n};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
        set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
        add_definitions(-DHAVE_IW416_BT_FW=1)
    else()
        message(STATUS "IW416 BT firmware: not supplied -- using a 1 KB SYNTHETIC image")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n"
"#define R16(n) (uint8_t)(n),(uint8_t)(n+1),(uint8_t)(n+2),(uint8_t)(n+3),(uint8_t)(n+4),(uint8_t)(n+5),(uint8_t)(n+6),(uint8_t)(n+7),(uint8_t)(n+8),(uint8_t)(n+9),(uint8_t)(n+10),(uint8_t)(n+11),(uint8_t)(n+12),(uint8_t)(n+13),(uint8_t)(n+14),(uint8_t)(n+15)\n"
"#define R256(n) R16(n),R16(n+16),R16(n+32),R16(n+48),R16(n+64),R16(n+80),R16(n+96),R16(n+112),R16(n+128),R16(n+144),R16(n+160),R16(n+176),R16(n+192),R16(n+208),R16(n+224),R16(n+240)\n"
"R256(0), R256(1), R256(2), R256(3)\n"
"};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
        set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
        add_definitions(-DHAVE_IW416_BT_FW=1 -DBT_FW_IS_SYNTHETIC=1)
    endif()
else()
    set(M2_BT_FW_SRC "")
endif()

evkb_library_dir(LVGL _lvgl_dir)

# The BT-audio bridge is shared with audio/bt_tone_test (one copy, not a fork).
if(M2_BT_OUT)
    set(M2_BT_OUT_SRCS
        ${CMAKE_CURRENT_LIST_DIR}/../../audio/bt_tone_test/AudioOutputBluetooth.cpp
        ${M2_BT_FW_SRC})
    include_directories(${CMAKE_CURRENT_LIST_DIR}/../../audio/bt_tone_test)
else()
    set(M2_BT_OUT_SRCS "")
endif()

teensy_add_executable(acid_box
    acid_box.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp
    ${M2_BT_OUT_SRCS})
# Audio's own dependency set (Wire/SPI/SdFat/SD/SerialFlash) is named exactly as
# acid_bass_test names it -- import_evkb_audio_full() creates those targets but
# does not link them.  Wire covers Wire2 = LPI2C5, shared by the codec and the
# touch controller, so TouchPanel needs no bus import of its own.
teensy_target_link_libraries(acid_box
    cores Audio Wire SPI SdFat SD SerialFlash MipiDisplay PXP TouchPanel)
if(M2_BT_OUT)
    teensy_target_link_libraries(acid_box M2Radio)
    target_include_directories(acid_box.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

# SynthUI, LVGL and CMSIS-DSP are plain CMake static-lib targets (no `.o`
# suffix on the target name), so they link via raw target_link_libraries on the
# `.elf` target rather than the teensy_ wrapper -- see lvgl_rk055_panel_test for
# why.  LVGL is named directly, not only pulled in transitively via SynthUI's
# PUBLIC edge, because this target also compiles port/lvgl_mipi_panel.cpp itself.
target_link_libraries(acid_box.elf SynthUI LVGL VGLite CMSIS-DSP stdc++ m)

# GT911::read() is [[nodiscard]] and this tree carries no -Werror: without this
# a dropped poll result in the indev port warns and the build SUCCEEDS.  Scoped
# to this target, same rationale as lvgl_rk055_touch_test's identical line.
target_compile_options(acid_box.elf
                       PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-result>)

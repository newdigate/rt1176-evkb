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
import_evkb_synthui()
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(acid_box
    acid_box.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
# Audio's own dependency set (Wire/SPI/SdFat/SD/SerialFlash) is named exactly as
# acid_bass_test names it -- import_evkb_audio_full() creates those targets but
# does not link them.
teensy_target_link_libraries(acid_box
    cores Audio Wire SPI SdFat SD SerialFlash MipiDisplay PXP)

# SynthUI, LVGL and CMSIS-DSP are plain CMake static-lib targets (no `.o`
# suffix on the target name), so they link via raw target_link_libraries on the
# `.elf` target rather than the teensy_ wrapper -- see lvgl_rk055_panel_test for
# why.  LVGL is named directly, not only pulled in transitively via SynthUI's
# PUBLIC edge, because this target also compiles port/lvgl_mipi_panel.cpp itself.
target_link_libraries(acid_box.elf SynthUI LVGL CMSIS-DSP stdc++ m)

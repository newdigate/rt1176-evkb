cmake_minimum_required(VERSION 3.24)
project(cm4_audio_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# Plan-2 Task 3 capstone: the CM4 image compiles the SAI I/O node sources, so
# this firmware claims audio for the CM4 (spec §4 -- exactly one core owns audio
# per firmware; i2s_int_test claims CM7, this claims CM4).
evkb_claim_audio_owner(CM4)

evkb_library_dir(Audio EVKB_AUDIO_DIR)
evkb_library_dir(Wire  EVKB_WIRE_DIR)

# CMSIS-DSP amalgams for the CM4 image world (analyze_fft256 -> arm_cfft_radix4_q15).
# -Wl,--gc-sections at the image link keeps unused kernels (and unused FFT/window
# tables) out of the 128 K ITCM.
evkb_cmsis_dsp_cm4_sources(_cm4_dsp_srcs _cm4_dsp_incs)

# ★★AUDIO-PLL OWNERSHIP (default ON = the HW-verified working pipeline). With
# AUDIO_CM4_PLL_FALLBACK=ON (the default) the CM7 pre-arms the 44.1 kHz ANATOP
# Audio PLL before Multicore.begin() and the CM4 image is built
# -DSAI1176_PLL_EXTERNAL so it SKIPS the ANATOP AI writes -- ownership of
# everything else (clock root/LPCG/pads/SAI/codec/graph/FFT) stays on the CM4.
# This is the config HW-verified on the EVKB (2026-07-22: audible 1 kHz on J101,
# rx_overflows=0, AUDIO_CM4=PASS -- see transcript_hw_evkb.txt). A gate's default
# should demonstrate the capability on the target, so this working pre-arm
# pipeline is the default.
#
# Pass -DAUDIO_CM4_PLL_FALLBACK=OFF for the pure-CM4-drives-PLL PROBE: the CM4
# owns the WHOLE pipeline including the ANATOP Audio-PLL AI-write handshake. That
# probe PASSES in QEMU (the model fakes the AI handshake) but HANGS on real
# silicon -- the CM4 cannot drive the ANATOP AI-write handshake (EVKB 2026-07-22
# finding; see Audio/sai1176.c SAI1176_PLL_EXTERNAL and the report).
option(AUDIO_CM4_PLL_FALLBACK "ON (default, HW-verified): CM7 pre-arms the Audio PLL, CM4 skips ANATOP AI writes. OFF: pure-CM4-drives-PLL probe (QEMU-only; hangs on silicon)" ON)
set(_cm4_defines EVKB_CM4_WORLD=1)
if(AUDIO_CM4_PLL_FALLBACK)
    list(APPEND _cm4_defines SAI1176_PLL_EXTERNAL=1)
endif()

# The CM4-owned audio pipeline image. SOURCES: the cm4 scaffold + the WM8962
# codec driver (over Wire/lpi2c1176.c) + the real core AudioStream engine + the
# Audio fork's shared SAI core, interrupt-driven I/O nodes, sine/peak/fft256
# nodes and their data tables + CMSIS-DSP amalgams.
# INCLUDE_DIRS order is load-bearing: cm4_shim (Arduino-lite) BEFORE the core
# dir so <Arduino.h> resolves to the shim while "AudioStream.h" resolves to the
# real engine; the CMSIS shim dir (inside _cm4_dsp_incs) owns <arm_math.h>.
teensy_add_cm4_image(cm4_audio
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.cpp
            ${CMAKE_CURRENT_LIST_DIR}/cm4/runtime_stubs.c
            ${CMAKE_CURRENT_LIST_DIR}/cm4/codec_wm8962.c
            ${EVKB_CORES_DIR}/AudioStream.cpp
            ${EVKB_AUDIO_DIR}/sai1176.c
            ${EVKB_AUDIO_DIR}/output_i2s_int.cpp
            ${EVKB_AUDIO_DIR}/input_i2s_int.cpp
            ${EVKB_AUDIO_DIR}/synth_sine.cpp
            ${EVKB_AUDIO_DIR}/analyze_peak.cpp
            ${EVKB_AUDIO_DIR}/analyze_fft256.cpp
            ${EVKB_AUDIO_DIR}/data_waveforms.c
            ${EVKB_AUDIO_DIR}/data_windows.c
            ${EVKB_AUDIO_DIR}/utility/sqrt_integer.c
            ${EVKB_WIRE_DIR}/lpi2c1176.c
            ${_cm4_dsp_srcs}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/cm4
                 ${EVKB_CORES_DIR}/cm4_shim
                 ${EVKB_AUDIO_DIR}
                 ${EVKB_AUDIO_DIR}/utility
                 ${EVKB_CORES_DIR}
                 ${EVKB_WIRE_DIR}
                 ${_cm4_dsp_incs}
    # The interrupt-driven SAI nodes gate their bodies on
    # `#if defined(__IMXRT1176__) || defined(EVKB_CM4_WORLD)` BEFORE they include
    # <Arduino.h>, so the CM4 world macro (which the shim also sets) must be on
    # the command line here -- the mirror of how __IMXRT1176__ marks the CM7 world.
    # SAI1176_PLL_EXTERNAL is appended only when AUDIO_CM4_PLL_FALLBACK=ON.
    DEFINES ${_cm4_defines})

teensy_add_executable(cm4_audio_test cm4_audio_test.cpp)
teensy_target_link_libraries(cm4_audio_test cores)
target_link_libraries(cm4_audio_test.elf stdc++)
teensy_target_link_cm4_image(cm4_audio_test cm4_audio)

# Pre-arm path (default ON): the CM7 pre-arms the Audio PLL before booting the
# CM4 (needs sai1176.c + its header). Only the pure-CM4-PLL probe (=OFF) omits it.
if(AUDIO_CM4_PLL_FALLBACK)
    target_compile_definitions(cm4_audio_test.elf PRIVATE AUDIO_CM4_PLL_FALLBACK)
    target_sources(cm4_audio_test.elf PRIVATE ${EVKB_AUDIO_DIR}/sai1176.c)
    target_include_directories(cm4_audio_test.elf PRIVATE ${EVKB_AUDIO_DIR})
endif()

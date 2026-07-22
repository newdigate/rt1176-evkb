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

# ★★HW PROBE FALLBACK (default OFF). The default build is the pure capstone: the
# CM4 owns the WHOLE pipeline including the ANATOP Audio-PLL bring-up. That build
# is QEMU-verified; on the EVKB it fires the documented ANATOP-PLL-from-CM4 probe
# (silence + sai_isr_count==0 -- the CM4 cannot drive the AI-write handshake; see
# Audio/sai1176.c SAI1176_PLL_EXTERNAL and the report). Configure with
# -DAUDIO_CM4_PLL_FALLBACK=ON to engage the plan's fallback: the CM7 pre-arms the
# 44.1 kHz Audio PLL before Multicore.begin() and the CM4 image skips the AI
# writes -- ownership of everything else (clock root/LPCG/pads/SAI/codec/graph)
# stays on the CM4.
option(AUDIO_CM4_PLL_FALLBACK "CM7 pre-arms the Audio PLL; CM4 skips ANATOP AI writes" OFF)
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

# Fallback engaged: the CM7 pre-arms the Audio PLL (needs sai1176.c + its header).
if(AUDIO_CM4_PLL_FALLBACK)
    target_compile_definitions(cm4_audio_test.elf PRIVATE AUDIO_CM4_PLL_FALLBACK)
    target_sources(cm4_audio_test.elf PRIVATE ${EVKB_AUDIO_DIR}/sai1176.c)
    target_include_directories(cm4_audio_test.elf PRIVATE ${EVKB_AUDIO_DIR})
endif()

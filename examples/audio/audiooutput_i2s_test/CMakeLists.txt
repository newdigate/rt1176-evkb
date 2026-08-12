cmake_minimum_required(VERSION 3.24)
project(audiooutput_i2s_test)
# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
# Left unguarded this caches 117 and silently builds an RT1176 image into
# build-rt1062/, which boots the wrong QEMU machine and fails looking like a
# board or model problem rather than a build misconfiguration.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)
teensy_add_executable(audiooutput_i2s_test audiooutput_i2s_test.cpp)
teensy_target_link_libraries(audiooutput_i2s_test cores Wire)

# Task 3 gate: AudioSynthWaveformSine -> AudioOutputI2S (SAI1 TX DMA) + a synth
# peak sanity, with the board's codec driver (WM8962 or WM8960) bringing up the
# playback route.
# output_i2s.cpp's isr() calls into memcpy_tointerleave{LR,L,R} (memcpy_audio.S,
# guarded __ARM_ARCH_7EM__ -- defined for -mcpu=cortex-m7 too, since M4 and M7
# are both ARMv7E-M) which import_arduino_library's own .S handling never sees
# here because these Audio sources are added directly, not via that helper's
# ROOT glob. Mirror its trick (LANGUAGE C + assembler-with-cpp) so the plain C
# compiler assembles it without needing enable_language(ASM) project-wide.
target_sources(audiooutput_i2s_test.elf PRIVATE
    audiooutput_i2s_test.cpp
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S
)
# The two boards carry different codecs -- WM8962 on the MIMXRT1170-EVKB, WM8960
# on the MIMXRT1060-EVKB -- so exactly one driver is compiled. EVKB_BOARD is set
# by evkb.cmake (line 59) and is a normal CMake cache variable here.
if(EVKB_BOARD STREQUAL "rt1062")
    # output_i2s.cpp's __IMXRT1062__ path drives PLL4 through set_audioClock()
    # (utility/imxrt_hw.cpp -- self-guarded to 1052/1062, an empty TU elsewhere);
    # the __IMXRT1176__ path programs its clocks inline and never references it.
    target_sources(audiooutput_i2s_test.elf PRIVATE
        ${EVKB_AUDIO_DIR}/control_wm8960.cpp
        ${EVKB_AUDIO_DIR}/utility/imxrt_hw.cpp
    )
else()
    target_sources(audiooutput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR}/control_wm8962.cpp)
endif()
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
)
target_include_directories(audiooutput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR})

target_link_libraries(audiooutput_i2s_test.elf stdc++)
target_link_libraries(audiooutput_i2s_test.elf m)

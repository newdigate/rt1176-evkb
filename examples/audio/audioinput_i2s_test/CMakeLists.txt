cmake_minimum_required(VERSION 3.24)
project(audioinput_i2s_test)
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
teensy_add_executable(audioinput_i2s_test audioinput_i2s_test.cpp)
teensy_target_link_libraries(audioinput_i2s_test cores Wire)

# Task 4 integration gate: pull in the fork's audio nodes directly (they live
# outside the core's glob, in ~/Development/Audio). config_i2s (output_i2s.cpp)
# is linked in even though this gate has no AudioOutputI2S instance, because
# AudioInputI2S::begin() calls AudioOutputI2S::config_i2s() to bring up SAI1 TX
# (RX runs synchronous to TX -- see input_i2s.cpp).
target_sources(audioinput_i2s_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/input_i2s.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp)
# The two boards carry different codecs -- WM8962 on the MIMXRT1170-EVKB, WM8960
# on the MIMXRT1060-EVKB -- so exactly one driver is compiled. EVKB_BOARD is set
# by evkb.cmake (line 59) and is a normal CMake cache variable here.
if(EVKB_BOARD STREQUAL "rt1062")
    # output_i2s.cpp's __IMXRT1062__ path drives PLL4 through set_audioClock()
    # (utility/imxrt_hw.cpp -- self-guarded to 1052/1062, an empty TU elsewhere);
    # the __IMXRT1176__ path programs its clocks inline and never references it.
    # input_i2s.cpp calls AudioOutputI2S::config_i2s(), so this applies here too.
    target_sources(audioinput_i2s_test.elf PRIVATE
        ${EVKB_AUDIO_DIR}/control_wm8960.cpp
        ${EVKB_AUDIO_DIR}/utility/imxrt_hw.cpp
    )
else()
    target_sources(audioinput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR}/control_wm8962.cpp)
endif()
target_include_directories(audioinput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR})

target_link_libraries(audioinput_i2s_test.elf stdc++)
target_link_libraries(audioinput_i2s_test.elf m)

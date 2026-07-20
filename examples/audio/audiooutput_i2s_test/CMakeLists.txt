cmake_minimum_required(VERSION 3.24)
project(audiooutput_i2s_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)
import_arduino_library(Wire $ENV{HOME}/Development/Wire)
teensy_add_executable(audiooutput_i2s_test audiooutput_i2s_test.cpp)
teensy_target_link_libraries(audiooutput_i2s_test cores Wire)

# Task 3 gate: AudioSynthWaveformSine -> AudioOutputI2S (SAI1 TX DMA) + a synth
# peak sanity, with AudioControlWM8962 bringing up the codec's playback route.
# output_i2s.cpp's isr() calls into memcpy_tointerleave{LR,L,R} (memcpy_audio.S,
# guarded __ARM_ARCH_7EM__ -- defined for -mcpu=cortex-m7 too, since M4 and M7
# are both ARMv7E-M) which import_arduino_library's own .S handling never sees
# here because these Audio sources are added directly, not via that helper's
# ROOT glob. Mirror its trick (LANGUAGE C + assembler-with-cpp) so the plain C
# compiler assembles it without needing enable_language(ASM) project-wide.
target_sources(audiooutput_i2s_test.elf PRIVATE
    audiooutput_i2s_test.cpp
    $ENV{HOME}/Development/Audio/synth_sine.cpp
    $ENV{HOME}/Development/Audio/data_waveforms.c
    $ENV{HOME}/Development/Audio/analyze_peak.cpp
    $ENV{HOME}/Development/Audio/output_i2s.cpp
    $ENV{HOME}/Development/Audio/control_wm8962.cpp
    $ENV{HOME}/Development/Audio/memcpy_audio.S
)
set_source_files_properties($ENV{HOME}/Development/Audio/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
)
target_include_directories(audiooutput_i2s_test.elf PRIVATE $ENV{HOME}/Development/Audio)

target_link_libraries(audiooutput_i2s_test.elf stdc++)
target_link_libraries(audiooutput_i2s_test.elf m)

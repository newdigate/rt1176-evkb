cmake_minimum_required(VERSION 3.24)
project(audiooutput_i2s_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)
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
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/control_wm8962.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S
)
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
)
target_include_directories(audiooutput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR})

target_link_libraries(audiooutput_i2s_test.elf stdc++)
target_link_libraries(audiooutput_i2s_test.elf m)

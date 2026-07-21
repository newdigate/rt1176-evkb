cmake_minimum_required(VERSION 3.24)
project(filter_fir_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_cmsis_dsp()
evkb_library_dir(Audio EVKB_AUDIO_DIR)

teensy_add_executable(filter_fir_test filter_fir_test.cpp)

# Audio nodes are cherry-picked (evkb gates do NOT glob the Audio fork).
# synth_sine needs data_waveforms (AudioWaveformSine); analyze_fft256 needs
# data_windows (Hanning) + utility/sqrt_integer.
target_sources(filter_fir_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/filter_fir.cpp
    ${EVKB_AUDIO_DIR}/analyze_fft256.cpp
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/data_windows.c
    ${EVKB_AUDIO_DIR}/utility/sqrt_integer.c)
target_include_directories(filter_fir_test.elf PRIVATE
    ${EVKB_AUDIO_DIR} ${EVKB_AUDIO_DIR}/utility)

teensy_target_link_libraries(filter_fir_test cores)
target_link_libraries(filter_fir_test.elf CMSIS-DSP stdc++)

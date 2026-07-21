cmake_minimum_required(VERSION 3.24)
project(guard_sweep_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Audio EVKB_AUDIO_DIR)

teensy_add_executable(guard_sweep_test guard_sweep_test.cpp)

# Cherry-picked (no glob, per gate convention). Deliberately NO CMSIS-DSP
# import: none of these nodes needs arm_math once the stale includes are
# stripped -- this gate is also the proof of that.
target_sources(guard_sweep_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/synth_karplusstrong.cpp
    ${EVKB_AUDIO_DIR}/synth_simple_drum.cpp
    ${EVKB_AUDIO_DIR}/synth_wavetable.cpp
    ${EVKB_AUDIO_DIR}/effect_delay.cpp
    ${EVKB_AUDIO_DIR}/play_queue.cpp
    ${EVKB_AUDIO_DIR}/record_queue.cpp
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c)
target_include_directories(guard_sweep_test.elf PRIVATE
    ${EVKB_AUDIO_DIR} ${EVKB_AUDIO_DIR}/utility)

teensy_target_link_libraries(guard_sweep_test cores)
target_link_libraries(guard_sweep_test.elf stdc++)
# libm must be re-linked AFTER the objects (same rationale as i2s_audio_test):
# sinf/lrintf (wt_table fill) + powf (synth_wavetable.h noteToFreq) live here.
target_link_libraries(guard_sweep_test.elf m)

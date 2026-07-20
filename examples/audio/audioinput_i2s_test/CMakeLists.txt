cmake_minimum_required(VERSION 3.24)
project(audioinput_i2s_test)
set(TEENSY_VERSION 117 CACHE STRING "")
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
    ${EVKB_AUDIO_DIR}/control_wm8962.cpp
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp)
target_include_directories(audioinput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR})

target_link_libraries(audioinput_i2s_test.elf stdc++)
target_link_libraries(audioinput_i2s_test.elf m)

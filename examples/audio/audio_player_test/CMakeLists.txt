cmake_minimum_required(VERSION 3.24)
project(audio_player_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# AudioPlayer (Audio/play_buffer.*) -> record queues + I2S out.  The Audio
# sources are listed one by one, as the sibling audio examples do.
evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)

teensy_add_executable(audio_player_test audio_player_test.cpp)
teensy_target_link_libraries(audio_player_test cores Wire)
target_sources(audio_player_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/play_buffer.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/record_queue.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S)
if(EVKB_BOARD STREQUAL "nosh")
    target_sources(audio_player_test.elf PRIVATE ${EVKB_AUDIO_DIR}/control_sgtl5000.cpp)
else()
    target_sources(audio_player_test.elf PRIVATE ${EVKB_AUDIO_DIR}/control_wm8962.cpp)
endif()
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp")
target_include_directories(audio_player_test.elf PRIVATE ${EVKB_AUDIO_DIR})
target_link_libraries(audio_player_test.elf stdc++)
target_link_libraries(audio_player_test.elf m)

cmake_minimum_required(VERSION 3.24)
project(cm4_fft_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# CMSIS-DSP into the CM4 image world: same pinned amalgams + collision shim as
# the CM7's import_evkb_cmsis_dsp(), compiled per-source by the image macro.
# -Wl,--gc-sections at the image link keeps unused kernels out of ITCM.
evkb_cmsis_dsp_cm4_sources(_cm4_dsp_srcs _cm4_dsp_incs)

teensy_add_cm4_image(cm4_fft
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.cpp
            ${CMAKE_CURRENT_LIST_DIR}/cm4/runtime_stubs.c
            ${_cm4_dsp_srcs}
    INCLUDE_DIRS ${_cm4_dsp_incs})

teensy_add_executable(cm4_fft_test cm4_fft_test.cpp)
teensy_target_link_libraries(cm4_fft_test cores)
target_link_libraries(cm4_fft_test.elf stdc++)
teensy_target_link_cm4_image(cm4_fft_test cm4_fft)

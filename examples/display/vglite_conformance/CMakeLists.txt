cmake_minimum_required(VERSION 3.24)
project(vglite_conformance)

set(TEENSY_VERSION 117 CACHE STRING "")

# Opt-in matrix including the cases that can HANG the front end (spec
# section 6). NEVER configured into build/ -- use a separate build-danger dir.
#
# Placed above teensy_add_executable() for LOCALITY ONLY -- it is the first
# thing that distinguishes this build from the default one, so it reads first.
# It is NOT a correctness requirement, and this comment exists to stop the
# placement being mistaken for one.
#
# MEASURED (CMake 4.4.2, 2026-08-30): both orderings produce IDENTICAL flags.
# Configured -DVGC_DANGEROUS=ON with this block above the target and again
# with it below, then diffed the generated
# CMakeFiles/vglite_conformance.elf.dir/flags.make: no difference, and
# -DVGC_DANGEROUS=1 present in CXX_DEFINES in BOTH (checked, so "identical"
# is not two arms that both dropped it). The directory COMPILE_DEFINITIONS
# property is resolved at GENERATE time for every target in the directory,
# not captured at target-creation time.
#
# The after-ordering is a SHIPPED pattern here, not a hypothetical:
# examples/display/synthui_fader_test/CMakeLists.txt:39 puts
# add_compile_definitions(FD_EYEBALL_HOLD=...) below its
# teensy_add_executable() at :23, and that define is genuinely consumed at
# synthui_fader_test.cpp:61 in the NEW-23 diagnostic build. Nothing here
# forbids it.
if(VGC_DANGEROUS)
    add_compile_definitions(VGC_DANGEROUS=1)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# VGLite only. NO MipiDisplay, NO PXP, NO LVGL: spec section 4 keeps the panel
# out of this example entirely (no scanout, no flip). The core's startup brings
# up the SEMC SDRAM before setup(), so EXTMEM is live without Display.begin(),
# and dropping the panel removes a whole subsystem from the failure surface --
# and the 12s bring-up margin vglite_probe's gate has to allow for.
import_evkb_vglite()

teensy_add_executable(vglite_conformance
    vglite_conformance.cpp
    vgc_cases_path.cpp
    vgc_dangerous.cpp)
teensy_target_link_libraries(vglite_conformance cores)

# VGLite is a plain CMake static-lib target (see import_evkb_vglite for why
# teensy_target_link_libraries cannot link it).
target_link_libraries(vglite_conformance.elf VGLite stdc++)

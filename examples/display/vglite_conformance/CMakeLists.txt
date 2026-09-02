cmake_minimum_required(VERSION 3.24)
project(vglite_conformance)

set(TEENSY_VERSION 117 CACHE STRING "")

# Opt-in matrix including the cases that can HANG the front end (spec
# section 6). NEVER configured into build/ -- use a separate build-danger dir.
#
# Above teensy_add_executable() for LOCALITY only -- it is what distinguishes
# this build from the default one, so it reads first. Placement is NOT a
# correctness requirement: MEASURED (CMake 4.4.2, 2026-08-30) by configuring
# -DVGC_DANGEROUS=ON both above and below the target and diffing the generated
# CMakeFiles/vglite_conformance.elf.dir/flags.make -- identical, with
# -DVGC_DANGEROUS=1 present in CXX_DEFINES in BOTH arms (checked, so
# "identical" is not two arms that each dropped it). The directory
# COMPILE_DEFINITIONS property is resolved at generate time.
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
    vgc_arena.cpp
    vgc_cases_path.cpp
    vgc_cases_color.cpp
    vgc_cases_grad.cpp
    vgc_cases_blit.cpp
    vgc_dangerous.cpp)
teensy_target_link_libraries(vglite_conformance cores)

# VGLite is a plain CMake static-lib target (see import_evkb_vglite for why
# teensy_target_link_libraries cannot link it).
target_link_libraries(vglite_conformance.elf VGLite stdc++)

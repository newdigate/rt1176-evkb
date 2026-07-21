# evkb.cmake — one-line bootstrap for every example: include AFTER project().
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
#
# It (1) pulls in teensy-cmake-macros (the local sibling checkout if present,
# else GitHub at the pinned ref), (2) defines the pinned library manifest and
# the import_evkb_library()/evkb_library_dir() helpers, (3) imports the
# imxrt1176 core and points COREPATH at wherever it resolved to.
#
# Local-first: a developer's ~/Development/<lib> checkout always wins (working
# tree, uncommitted edits included) — the silicon-truth loop is unchanged. A
# fresh clone with no sibling checkouts fetches everything from GitHub at the
# pinned refs below (CPM; set the CPM_SOURCE_CACHE env var, e.g. ~/.cache/CPM,
# to clone each repo once across all build dirs).
#
# -DEVKB_FORCE_FETCH=ON ignores every local checkout — the "fresh user"
# simulation used to test this file.
#
# Updating a pin: push the library, paste its new SHA here, commit.

if(DEFINED EVKB_CMAKE_INCLUDED)
    return()
endif()
set(EVKB_CMAKE_INCLUDED 1)

set(EVKB_ROOT ${CMAKE_CURRENT_LIST_DIR})
option(EVKB_FORCE_FETCH "Ignore local sibling checkouts; fetch at the pinned refs" OFF)

# --- teensy-cmake-macros (the build system itself) ---------------------------
include(FetchContent)
if(EXISTS ${EVKB_ROOT}/teensy-cmake-macros/CMakeLists.include.txt AND NOT EVKB_FORCE_FETCH)
    FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB_ROOT}/teensy-cmake-macros)
else()
    FetchContent_Declare(teensy_cmake_macros
        GIT_REPOSITORY https://github.com/newdigate/teensy-cmake-macros
        GIT_TAG        e948da4d43cf76e3a0d8813cd85e6da314a0a569)
endif()
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# --- pinned library manifest: name -> (local checkout, repo, ref, sub-path) --
# The name is what examples pass to import_evkb_library()/evkb_library_dir()
# (it matches the existing import_arduino_library call sites). LOCAL is the
# full local path (including any sub-path); PATH is the sub-path inside the
# fetched repo ("." for the repo root).
macro(_evkb_lib NAME LOCAL URL REF PATH)
    set(EVKB_LIB_${NAME}_LOCAL "${LOCAL}")
    set(EVKB_LIB_${NAME}_URL   "${URL}")
    set(EVKB_LIB_${NAME}_REF   "${REF}")
    set(EVKB_LIB_${NAME}_PATH  "${PATH}")
endmacro()

set(_dev "$ENV{HOME}/Development")
_evkb_lib(cores          ${EVKB_ROOT}/cores/imxrt1176 https://github.com/newdigate/teensy-cores    7f44a4ed9857b2cc4284d6b9472575952e76237f imxrt1176)
_evkb_lib(Wire           ${_dev}/Wire                 https://github.com/newdigate/Wire            193e949c51c8da316df6ed619e019e308acaca45 .)
_evkb_lib(SPI            ${_dev}/SPI                  https://github.com/newdigate/SPI             eefd8798c74a727a09f38d34d79e1ab55c0110b3 .)
_evkb_lib(Audio          ${_dev}/Audio                https://github.com/newdigate/Audio           460d0c174ac5ee4718133942abb094f0caca05dd .)
_evkb_lib(SdFat          ${_dev}/SdFat                https://github.com/newdigate/SdFat           681bfcf83d05beb943e3d905f15d8181bf9072c7 .)
_evkb_lib(SD             ${_dev}/PaulS_SD             https://github.com/newdigate/SD              e28c549918ea34ffb2942fd84deffc7c76a89880 .)
_evkb_lib(ethernet       ${_dev}/Ethernet             https://github.com/newdigate/Ethernet        eebbfebc699a1500864236db21d17abf3cf7535a .)
_evkb_lib(nativeethernet ${_dev}/NativeEthernet       https://github.com/newdigate/NativeEthernet  7f5d881d5da80540177caea760d895780478b128 .)
_evkb_lib(fnet           ${_dev}/FNET/src             https://github.com/newdigate/FNET            a50373d50e57778595eb388b7bfeaad79080a077 src)
_evkb_lib(lwip           ${_dev}/lwip                 https://github.com/newdigate/lwip            03dddc67f73113e2beb3807e290a368d5cb7cfe0 .)
_evkb_lib(USBHost_t36    ${_dev}/USBHost_t36          https://github.com/newdigate/USBHost_t36     77c23a6a1c692c987e9c27e4caa1a3ea402ca92f .)
_evkb_lib(EEPROM         ${_dev}/EEPROM               https://github.com/PaulStoffregen/EEPROM     9790da76d62bc633563f763c3dc1526539ed0a6b .)
_evkb_lib(Bounce2        ${_dev}/Bounce2/src          https://github.com/PaulStoffregen/Bounce2    eb5ab9fad8a15539743315786beb8236e96c8b9a src)
# ARM upstream (not Arduino-layout; consumed via import_evkb_cmsis_dsp below).
# CMSIS-Core is a headers-only dependency of CMSIS-DSP (cmsis_compiler.h et al).
_evkb_lib(CMSIS-DSP  ${_dev}/CMSIS-DSP https://github.com/ARM-software/CMSIS-DSP 4b4fa8ff218ca5ac20bad71b653a37d93815f24b .) # v1.17.1
_evkb_lib(CMSIS-Core ${_dev}/CMSIS_6   https://github.com/ARM-software/CMSIS_6   45dab712ad84f8cbbf2b7bfc089c19088507df6f .) # v6.3.0

# --- helpers -----------------------------------------------------------------
# evkb_library_dir(NAME OUT_VAR): resolve NAME's source directory (local-first,
# pinned-git fallback) without importing — for cherry-picked sources.
macro(evkb_library_dir NAME OUT_VAR)
    if(NOT DEFINED EVKB_LIB_${NAME}_URL)
        message(FATAL_ERROR "evkb_library_dir(${NAME}): not in the evkb.cmake manifest")
    endif()
    if(NOT DEFINED _evkb_resolved_${NAME})    # memoized: an example may both
        if(EVKB_FORCE_FETCH)                  # import a lib AND reference its dir
            set(_evkb_local "${EVKB_ROOT}/.force-fetch-no-local")
        else()
            set(_evkb_local "${EVKB_LIB_${NAME}_LOCAL}")
        endif()
        resolve_arduino_library_auto(${NAME} "${_evkb_local}"
            "${EVKB_LIB_${NAME}_URL}" "${EVKB_LIB_${NAME}_REF}" "${EVKB_LIB_${NAME}_PATH}"
            _evkb_resolved_${NAME})
    endif()
    set(${OUT_VAR} "${_evkb_resolved_${NAME}}")
endmacro()

# import_evkb_library(NAME [subdirs...]): the drop-in replacement for
# import_arduino_library(NAME <local path> [subdirs...]).
macro(import_evkb_library NAME)
    evkb_library_dir(${NAME} _evkb_import_dir)
    import_arduino_library(${NAME} "${_evkb_import_dir}" ${ARGN})
endmacro()

# --- CMSIS-DSP shared helpers (CM7 target + CM4 image world) -----------------
# The amalgam source list and the generated collision shim live EXACTLY once,
# consumed by both import_evkb_cmsis_dsp() (CM7 static lib) and
# evkb_cmsis_dsp_cm4_sources() (CM4 image world).

# _evkb_cmsis_dsp_amalgams(<dspdir> <outvar>): the per-group amalgamation
# sources — Source/<G>/<G>.c #includes the individual files, so globbing
# Source/*/*.c would double-define every symbol.
macro(_evkb_cmsis_dsp_amalgams DSPDIR OUTVAR)
    set(${OUTVAR} "")
    # Pin bump: diff `ls Source/` against this list (a NEW upstream group
    # would otherwise be silently omitted); F16 amalgams are absent only
    # for Controller/Quaternion/Window at v1.17.1.
    foreach(_g BasicMathFunctions BayesFunctions CommonTables
               ComplexMathFunctions ControllerFunctions DistanceFunctions
               FastMathFunctions FilteringFunctions InterpolationFunctions
               MatrixFunctions QuaternionMathFunctions SVMFunctions
               StatisticsFunctions SupportFunctions TransformFunctions
               WindowFunctions)
        list(APPEND ${OUTVAR} "${DSPDIR}/Source/${_g}/${_g}.c")
        if(EXISTS "${DSPDIR}/Source/${_g}/${_g}F16.c")
            list(APPEND ${OUTVAR} "${DSPDIR}/Source/${_g}/${_g}F16.c")
        endif()
    endforeach()
endmacro()

# _evkb_cmsis_dsp_shim(<dspdir> <outdirvar>): include-order armor — a generated
# wrapper shadows <arm_math.h> so a TU may include Arduino.h and arm_math.h in
# either order. It papers over two core-vs-CMSIS clashes: (1) pins_arduino.h's
# "#define A0 16" (and A1/A2) vs the A0/A1/A2 struct members in
# dsp/controller_functions.h; (2) core_pins.h's __disable_irq()/__enable_irq()
# static inlines vs the identical definitions in CMSIS-Core's cmsis_gcc.h
# (renamed away — nothing in the DSP code calls them after the parse). The shim
# is world-agnostic: cm4_shim/Arduino.h has the same __disable_irq/__enable_irq
# inline collision that the CM7 core does.
macro(_evkb_cmsis_dsp_shim DSPDIR OUTDIRVAR)
    # file(CONFIGURE @ONLY) substitutes real variables, not macro parameters.
    set(_evkb_cmsis_shim_dspdir "${DSPDIR}")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/evkb-cmsis-shim/arm_math.h"
         CONTENT [[
/* Generated by evkb.cmake (_evkb_cmsis_dsp_shim) - do not edit. */
#ifndef EVKB_ARM_MATH_SHIM_H
#define EVKB_ARM_MATH_SHIM_H
#pragma push_macro("A0")
#pragma push_macro("A1")
#pragma push_macro("A2")
#undef A0
#undef A1
#undef A2
#define __disable_irq __evkb_cmsis_disable_irq
#define __enable_irq  __evkb_cmsis_enable_irq
#include "@_evkb_cmsis_shim_dspdir@/Include/arm_math.h"
#undef __disable_irq
#undef __enable_irq
#pragma pop_macro("A2")
#pragma pop_macro("A1")
#pragma pop_macro("A0")
#endif
]]       @ONLY NEWLINE_STYLE UNIX)
    set(${OUTDIRVAR} "${CMAKE_BINARY_DIR}/evkb-cmsis-shim")
endmacro()

# import_evkb_cmsis_dsp(): CMSIS-DSP as a static lib target named CMSIS-DSP.
# Not Arduino-layout, so it bypasses import_arduino_library. CMSIS-Core's
# include dir is PUBLIC because <arm_math.h> reaches cmsis_compiler.h from every
# consumer TU; it is deliberately NOT added to cores or any other target.
macro(import_evkb_cmsis_dsp)
    if(NOT TARGET CMSIS-DSP)
        evkb_library_dir(CMSIS-DSP _evkb_cmsisdsp_dir)
        evkb_library_dir(CMSIS-Core _evkb_cmsiscore_dir)
        _evkb_cmsis_dsp_amalgams("${_evkb_cmsisdsp_dir}" _evkb_cmsisdsp_srcs)
        add_library(CMSIS-DSP STATIC ${_evkb_cmsisdsp_srcs})
        _evkb_cmsis_dsp_shim("${_evkb_cmsisdsp_dir}" _evkb_cmsis_shim_dir)
        target_include_directories(CMSIS-DSP
            PUBLIC  "${_evkb_cmsis_shim_dir}"
                    "${_evkb_cmsisdsp_dir}/Include"
                    "${_evkb_cmsiscore_dir}/CMSIS/Core/Include"
            PRIVATE "${_evkb_cmsisdsp_dir}/PrivateInclude")
        target_link_libraries(CMSIS-DSP PRIVATE teensy_flags)
        # CMSIS-DSP calls libm (expf/logf/sqrtf in the Bayes/Distance/SVM
        # groups). PUBLIC so -lm lands AFTER the consumer's objects on the link
        # line — the toolchain's own -lm sits before them and resolves nothing
        # (same reason the audio examples each re-link m).
        target_link_libraries(CMSIS-DSP PUBLIC m)
    endif()
endmacro()

# CMSIS-DSP for CM4 images: the image world compiles per-source (no CMake
# targets), so expose the amalgam source list + include dirs for
# teensy_add_cm4_image(SOURCES ... INCLUDE_DIRS ...). Reuses the same pinned
# fetch and the same generated collision-shim dir as the CM7 target (the shim
# is world-agnostic: cm4_shim/Arduino.h has the same __disable_irq/__enable_irq
# inline collision with cmsis_gcc.h that the CM7 core does).
macro(evkb_cmsis_dsp_cm4_sources OUT_SRCS OUT_INCS)
    evkb_library_dir(CMSIS-DSP _evkb_cmsisdsp_dir)
    evkb_library_dir(CMSIS-Core _evkb_cmsiscore_dir)
    _evkb_cmsis_dsp_amalgams("${_evkb_cmsisdsp_dir}" ${OUT_SRCS})
    _evkb_cmsis_dsp_shim("${_evkb_cmsisdsp_dir}" _evkb_cmsis_shim_dir)
    set(${OUT_INCS}
        "${_evkb_cmsis_shim_dir}"
        "${_evkb_cmsisdsp_dir}/Include"
        "${_evkb_cmsisdsp_dir}/PrivateInclude"
        "${_evkb_cmsiscore_dir}/CMSIS/Core/Include")
endmacro()

# Build-time audio-ownership exclusivity (spec §4): a firmware claims audio
# for exactly one core. Called by convention from gate CMakeLists that compile
# SAI I/O node sources.
macro(evkb_claim_audio_owner CORE)
    get_property(_owner GLOBAL PROPERTY EVKB_AUDIO_OWNER)
    if(_owner AND NOT _owner STREQUAL "${CORE}")
        message(FATAL_ERROR "audio owner already claimed by ${_owner}; cannot also claim ${CORE} (one core owns audio per firmware)")
    endif()
    set_property(GLOBAL PROPERTY EVKB_AUDIO_OWNER "${CORE}")
endmacro()

# --- the imxrt1176 core (every example needs it) -----------------------------
evkb_library_dir(cores EVKB_CORES_DIR)
# The toolchain file guessed COREPATH from its own location; re-point it at the
# resolved core so a fresh clone (fetched core) links the right imxrt1176.ld.
# MUST happen BEFORE the first import_arduino_library call — that call runs
# teensy_set_dynamic_properties once, baking COREPATH into the link flags.
# Trailing slash required (LINKER_FILE is "${COREPATH}imxrt1176.ld").
set(COREPATH "${EVKB_CORES_DIR}/" CACHE STRING "imxrt1176 core path" FORCE)
import_arduino_library(cores "${EVKB_CORES_DIR}")

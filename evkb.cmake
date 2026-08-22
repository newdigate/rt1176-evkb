# evkb.cmake — one-line bootstrap for every example: include AFTER project().
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
#
# It (1) pulls in teensy-cmake-macros (a ${TEENSY_LIB_ROOT}/teensy-cmake-macros
# checkout if present, else GitHub at the pinned ref), (2) declares the pinned
# library manifest behind import_evkb_library()/evkb_library_dir(), (3) imports
# the board's core and points COREPATH at wherever it resolved to.
#
# Local-first: a developer's ${TEENSY_LIB_ROOT}/<lib> checkout always wins
# (working tree, uncommitted edits included) — TEENSY_LIB_ROOT defaults to
# ~/Development (env var to override). A fresh clone with no sibling checkouts
# fetches everything from GitHub at the pinned refs below. Set the
# CPM_SOURCE_CACHE env var (e.g. ~/.cache/CPM) to clone each repo once across
# all build dirs — covers every declared library INCLUDING the core; the one
# exception is teensy-cmake-macros itself, fetched with plain FetchContent
# (~½ MB; deliberately not CPM, so the single CPM pin stays in the macros —
# see docs/superpowers/specs/2026-08-14-shared-cores-macros-resolution-design.md).
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

# --- board axis (before the macros: the COREPATH pre-set needs the subdir) ---
# Which board this build targets. Drives the core subdir below and the QEMU
# machine in tools/gate-lib.sh. Default rt1176 so every existing example
# builds exactly as before without being edited.
set(EVKB_BOARD "rt1176" CACHE STRING "Target board: rt1176 (MIMXRT1170-EVKB) or rt1062 (MIMXRT1060-EVKB)")
set_property(CACHE EVKB_BOARD PROPERTY STRINGS rt1176 rt1062)
if(EVKB_BOARD STREQUAL "rt1176")
    set(EVKB_CORE_SUBDIR imxrt1176)
    set(_evkb_expected_tv 117)
elseif(EVKB_BOARD STREQUAL "rt1062")
    set(EVKB_CORE_SUBDIR teensy4)
    set(_evkb_expected_tv 42)
else()
    message(FATAL_ERROR "EVKB_BOARD must be rt1176 or rt1062, got '${EVKB_BOARD}'")
endif()
# The toolchain file supplies TEENSY_VERSION; the board flag and the toolchain
# must agree. A mismatch used to configure CLEAN and die deep in the core
# compile — wrong -D__IMXRT1176__, a linker script that does not exist in the
# selected core (e.g. teensy4/imxrt1176.ld) — looking like a board or model
# problem rather than a build misconfiguration. Name it at configure time.
if(NOT DEFINED TEENSY_VERSION OR NOT TEENSY_VERSION EQUAL _evkb_expected_tv)
    message(FATAL_ERROR "EVKB_BOARD=${EVKB_BOARD} requires TEENSY_VERSION ${_evkb_expected_tv} "
        "(rt1176 -> 117 via ../../../toolchain/rt1170-evkb.toolchain.cmake, "
        "rt1062 -> 42 via ../../../toolchain/rt1062-evkb.toolchain.cmake); got '${TEENSY_VERSION}'")
endif()

# --- TEENSY_LIB_ROOT: where sibling checkouts live ---------------------------
# Set BEFORE the macros load so both computations agree by construction.
# evkb's default wins by construction; keep in sync with the macros' own
# block (CMakeLists.include.txt, TEENSY_LIB_ROOT).
if(NOT DEFINED TEENSY_LIB_ROOT)
    if(DEFINED ENV{TEENSY_LIB_ROOT})
        set(TEENSY_LIB_ROOT "$ENV{TEENSY_LIB_ROOT}" CACHE PATH "root for sibling library checkouts")
    else()
        set(TEENSY_LIB_ROOT "$ENV{HOME}/Development" CACHE PATH "root for sibling library checkouts")
    endif()
endif()
# Map evkb's knob onto the generic flag the resolver honors. Respect a
# directly-passed -DTEENSY_FORCE_FETCH=ON too — the macros' README documents
# that flag, so it must not be silently clobbered here.
if(EVKB_FORCE_FETCH OR TEENSY_FORCE_FETCH)
    set(TEENSY_FORCE_FETCH ON)
else()
    set(TEENSY_FORCE_FETCH OFF)
endif()

# --- COREPATH pre-set: suppress the macros' own core resolution --------------
# The macros resolve teensy-cores themselves when COREPATH is undefined; evkb
# resolves the core through its own manifest (pinned below) instead. Economy,
# not safety: the real value is FORCEd after resolution, before the first
# import bakes it into link flags.
if(EXISTS "${TEENSY_LIB_ROOT}/teensy-cores/${EVKB_CORE_SUBDIR}" AND NOT EVKB_FORCE_FETCH)
    set(COREPATH "${TEENSY_LIB_ROOT}/teensy-cores/${EVKB_CORE_SUBDIR}/")
else()
    set(COREPATH "${CMAKE_BINARY_DIR}/evkb-corepath-pending/")   # placeholder; FORCEd below
endif()

# --- teensy-cmake-macros (the build system itself) ---------------------------
# Plain FetchContent, deliberately NOT CPM: the macros own the single CPM pin,
# and this bootstrap must not duplicate it (dual-pin drift — see the spec).
include(FetchContent)
if(EXISTS ${TEENSY_LIB_ROOT}/teensy-cmake-macros/CMakeLists.include.txt AND NOT EVKB_FORCE_FETCH)
    message(STATUS "teensy-cmake-macros: local ${TEENSY_LIB_ROOT}/teensy-cmake-macros")
    FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${TEENSY_LIB_ROOT}/teensy-cmake-macros)
else()
    message(STATUS "teensy-cmake-macros: fetching at the pinned ref")
    FetchContent_Declare(teensy_cmake_macros
        GIT_REPOSITORY https://github.com/newdigate/teensy-cmake-macros
        GIT_TAG        917062ee01a2f38ac1973f51e0600d01f6a88726)
endif()
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# --- pinned library manifest -------------------------------------------------
# teensy_declare_library(NAME <subdir under TEENSY_LIB_ROOT> URL REF <sub-path
# inside the fetched repo>). NAME is what examples pass to
# import_evkb_library()/evkb_library_dir() (matches the existing call sites).
teensy_declare_library(cores          teensy-cores/${EVKB_CORE_SUBDIR} https://github.com/newdigate/teensy-cores    fcd22b0c58c4d9c8099b999e5eeb28b57593e441 ${EVKB_CORE_SUBDIR}) # on cores MASTER since 2026-08-20 (the m2-serial2 branch was fast-forwarded into it); it carries imxrt1176 Serial2 on LPUART2, the M.2 Bluetooth HCI UART
teensy_declare_library(Wire           Wire                 https://github.com/newdigate/Wire            19babd18b83bc2f9ddbd16f6afefcbb42558530d .)
teensy_declare_library(SPI            SPI                  https://github.com/newdigate/SPI             eefd8798c74a727a09f38d34d79e1ab55c0110b3 .)
teensy_declare_library(PXP            PXP                  https://github.com/newdigate/PXP             5658e34885ff3a5cb5516a178ba60743e62a7517 .)
teensy_declare_library(ILI9341_t3     ILI9341_t3           https://github.com/newdigate/ILI9341_t3      e69e657f360e997e93fc7736a23eba8b09d1a043 .)
teensy_declare_library(MipiDisplay    MipiDisplay          https://github.com/newdigate/MipiDisplay     4cdb46c1d96cd7d42c05003481644cf81a8c030f .) # panel chosen by the importer: import_evkb_library(MipiDisplay soc panels/<name>)
teensy_declare_library(TouchPanel     TouchPanel           https://github.com/newdigate/TouchPanel      d20499c707290985379cb407689eca7f2c14fd08 .) # controller chosen by the importer: import_evkb_library(TouchPanel gt911)
teensy_declare_library(Audio          Audio                https://github.com/newdigate/Audio           ca1d10fe77afed819d8c2cf01f2a7c714830765f .)
teensy_declare_library(SdFat          SdFat                https://github.com/newdigate/SdFat           681bfcf83d05beb943e3d905f15d8181bf9072c7 .)
teensy_declare_library(M2Radio        M2Radio              https://github.com/newdigate/M2Radio         300d32bbd9bb47593c6a31c31742e6b51af95ba2 .) # subdir chosen by the importer: import_evkb_library(M2Radio sdio iw416 [lwip]). W17: dual-BSS RX tag + sendDataFrameBss + uAP netif + WPA2 TLVs -- m2_uap_lwip will NOT BUILD against anything older (verified: -DEVKB_FORCE_FETCH=ON against the previous pin fails on iw416NetifInitUap/PollDual/rxFramesByBss).
teensy_declare_library(SD             PaulS_SD             https://github.com/newdigate/SD              e28c549918ea34ffb2942fd84deffc7c76a89880 .)
teensy_declare_library(SerialFlash    SerialFlash          https://github.com/newdigate/SerialFlash     2b6f24168c1ca97af1138c4a5b10255b39c4ad0b .)
teensy_declare_library(ethernet       Ethernet             https://github.com/newdigate/Ethernet        eebbfebc699a1500864236db21d17abf3cf7535a .)
teensy_declare_library(nativeethernet NativeEthernet       https://github.com/newdigate/NativeEthernet  7f5d881d5da80540177caea760d895780478b128 .)
teensy_declare_library(fnet           FNET/src             https://github.com/newdigate/FNET            41deb998e16195cf317a2a0f3f889d33451a57ab src)
# lwip: the M.2 branch's pin, NOT master's -- master's 03dddc67 is an ANCESTOR
# of this one, and what sits between them is the W11 lwipopts bump
# (TCP_WND / TCP_SND_BUF = 8*MSS) that the M.2 throughput work depends on.
teensy_declare_library(lwip           lwip                 https://github.com/newdigate/lwip            c6b25488403d08fb5886d77e193d089c87b604c0 .)
teensy_declare_library(USBHost_t36    USBHost_t36          https://github.com/newdigate/USBHost_t36     928bfefc2c9eebcb8e01bb4fd136b2cb6d5017f8 .)
teensy_declare_library(LVGL           LVGL                 https://github.com/newdigate/LVGL            afb9789ed430614b74eb6ae946da6062485964cd .) # NOT Arduino-layout: use import_evkb_lvgl(), not import_evkb_library()
teensy_declare_library(SynthUI        SynthUI              https://github.com/newdigate/SynthUI         f6309669b634eee3050254c709a2d7517141b6a1 .) # NOT Arduino-layout: use import_evkb_synthui(). Pushed 2026-08-17; the pin then moved to a REWRITTEN history (reference/rebirth/ dropped before going public), so every SHA before e132012 is unreachable.
teensy_declare_library(VGLite         VGLite               https://github.com/newdigate/VGLite          3119701c6f4f3dcf1de6d03ca1138c83e6005652 .) # NOT Arduino-layout: use import_evkb_vglite(). NXP's VGLite vendored verbatim plus this tree's bare-metal port. Since the v7 (SDK 26.06 LTS) re-vendor it is MIT THROUGHOUT -- the Apache-2.0 vg_lite_flat.{c,h} pair no longer exists upstream; NOTICE records the history.
teensy_declare_library(EEPROM         EEPROM               https://github.com/newdigate/EEPROM          477c4296040d2061c90779f2841cdb953b5aca81 .)
teensy_declare_library(Bounce2        Bounce2/src          https://github.com/PaulStoffregen/Bounce2    eb5ab9fad8a15539743315786beb8236e96c8b9a src)
# ARM upstream (not Arduino-layout; consumed via import_evkb_cmsis_dsp below).
# CMSIS-Core is a headers-only dependency of CMSIS-DSP (cmsis_compiler.h et al).
teensy_declare_library(CMSIS-DSP  CMSIS-DSP https://github.com/ARM-software/CMSIS-DSP 4b4fa8ff218ca5ac20bad71b653a37d93815f24b .) # v1.17.1
teensy_declare_library(CMSIS-Core CMSIS_6   https://github.com/ARM-software/CMSIS_6   45dab712ad84f8cbbf2b7bfc089c19088507df6f .) # v6.3.0

# --- helpers: thin aliases over the macros' declared-library API -------------
# Kept so no example CMakeLists.txt changes (≈90 call sites). Memoization,
# local-first and force-fetch all live in the macros now.
macro(evkb_library_dir NAME OUT_VAR)
    teensy_resolve_library(${NAME} ${OUT_VAR})
endmacro()

macro(import_evkb_library NAME)
    teensy_import_library(${NAME} ${ARGN})
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

# --- LVGL --------------------------------------------------------------------
# LVGL is 449 .c files across ~145 directories; import_arduino_library() globs
# only one level deep, so LVGL gets a dedicated target the same way CMSIS-DSP
# does. The port's display bindings are deliberately NOT compiled here — each
# example compiles the one binding it needs, so an ILI9341 sketch never pulls
# in MipiDisplay.
#
# The glob is *.c ONLY and must stay that way: src/libs/thorvg/ is 47 .cpp
# files needing a config.h that is absent from a fresh clone.
# import_evkb_lvgl([VGLITE])
#
# VGLITE opts this example into LVGL's VG_LITE draw unit on the GC355. It is
# OPT-IN, not the default, because every existing display gate has recorded
# SOFTWARE goldens and the GPU path will not reproduce them -- hardware
# antialiasing is not LVGL's mask arithmetic. Two golden sets, never one.
macro(import_evkb_lvgl)
    # ★ ARGN must be copied into a real variable before IN_LIST. In a MACRO
    # (not a function) ARGN is not a variable at all -- it is substituted
    # textually -- so `if("VGLITE" IN_LIST ARGN)` never matches and the opt-in
    # silently does nothing. That failure is invisible: the build SUCCEEDS,
    # LVGL's 19 vg_lite objects compile to zero symbols each because
    # LV_USE_DRAW_VG_LITE stayed 0, and the ELF simply has no GPU backend in
    # it. Caught here only by counting symbols in those objects.
    set(_evkb_lvgl_args ${ARGN})
    set(_evkb_lvgl_vglite OFF)
    if("VGLITE" IN_LIST _evkb_lvgl_args)
        set(_evkb_lvgl_vglite ON)
    endif()
    if(NOT TARGET LVGL)
        evkb_library_dir(LVGL _evkb_lvgl_dir)
        file(GLOB_RECURSE _evkb_lvgl_srcs CONFIGURE_DEPENDS
             "${_evkb_lvgl_dir}/lvgl/src/*.c")
        add_library(LVGL STATIC
            ${_evkb_lvgl_srcs}
            "${_evkb_lvgl_dir}/port/lvgl_rt1176.cpp")
        target_include_directories(LVGL PUBLIC
            "${_evkb_lvgl_dir}/lvgl"
            "${_evkb_lvgl_dir}/port")
        target_compile_definitions(LVGL PUBLIC LV_CONF_INCLUDE_SIMPLE=1)
        target_link_libraries(LVGL PRIVATE teensy_flags)
        # LVGL's software renderer calls libm (sqrtf/sinf/cosf in the vector,
        # gradient and transform paths). PUBLIC for the same reason CMSIS-DSP
        # does it above: -lm must land AFTER the consumer's objects on the link
        # line — the toolchain's own -lm sits before them and resolves nothing.
        # Inert today (those paths are behind disabled config), but it turns a
        # confusing undefined-reference in Task 3/4 into a non-event.
        target_link_libraries(LVGL PUBLIC m)

        if(_evkb_lvgl_vglite)
            import_evkb_vglite()
            evkb_library_dir(VGLite _evkb_vglite_dir)

            # ★ NO COMPAT SHIM, AND NO IMAGE-DECODER GUARD -- both retired with
            # the HEADER_VERSION 7 re-vendor, and it matters WHY rather than
            # just that they went.
            #
            # The shim existed because LVGL 9.4 referenced 47 names the old
            # driver did not define. v7 defines all 47, measured by
            # tools/vglite-lvgl-names.py, which is kept precisely to prove that
            # and to catch the next LVGL bump reopening a gap.
            #
            # The decoder guard existed because two SHIMMED names --
            # VG_LITE_BGR888 and VG_LITE_BGRA5658 -- were live format
            # selections, and handing a faked format to this GPU hangs it
            # silently. v7 has both formats for real, so the hazard is gone with
            # its cause. A check kept past its reason reads as protection and
            # is worse than none.
            # LV_USE_MATRIX is REQUIRED by the backend (lv_matrix_t is declared
            # only under it, and the backend's headers use it by value), and it
            # in turn requires LV_USE_FLOAT.
            #
            # ★ LV_USE_FLOAT CHANGES PIXELS -- lv_value_precise_t becomes float,
            # so angles and coordinates round differently. That is exactly why
            # this is opt-in: every software gate keeps goldens recorded with it
            # at 0, and the GPU example gets its own set. Flipping it globally
            # would move goldens tree-wide and each would need re-confirming on
            # glass.
            # ★ 64-byte draw-buffer stride, because the GC355 reports
            # gcFEATURE_BIT_VG_16PIXELS_ALIGN=1 (measured on silicon) and 16 px
            # x 4 B at LV_COLOR_DEPTH 32 is 64. LVGL allocates glyph and layer
            # buffers with these; the software defaults (stride 1, align 4) are
            # fine for a CPU renderer and feed the GPU misaligned rows, which
            # renders vector paths correctly while smearing every small blit.
            # ★ LV_USE_VECTOR_GRAPHIC is REQUIRED for gradients, not an extra:
            # lv_draw_vg_lite_fill.c compiles its gradient branch (and
            # lv_vg_lite_grad.c compiles at all) only under it. Without it
            # every gradient fill hits LV_LOG_WARN and is SKIPPED -- the build
            # succeeds, solids render, and each gradient surface is simply
            # absent, which presents as wrong COLOURS, not missing shapes:
            # the knob face became its semi-transparent cap composited on the
            # screen background. Found by a primitive-isolation scene
            # (GRADPROBE) after two sessions of chasing blend/premultiply
            # theories. ThorVG stays OFF; this enables only LVGL's own
            # vector API, which the backend maps to the GPU.
            # ★ The EXT linear-gradient path is DISABLED against this driver,
            # by contract mismatch measured on silicon: the GC355 v7
            # vg_lite_update_linear_grad() consumes grad->matrix (NXP's own
            # example assigns it immediately before the call), but LVGL 9.4
            # zeroes the item and only assigns the matrix at DRAW time -- so
            # update sees a zero matrix, the transformed endpoints have
            # length 0, and it returns INVALID_ARGUMENT. LVGL then trips its
            # own create-failure assert (grad_item_pool_free, item->ctx) and
            # the firmware halts: the symptom is a nearly-empty frame, not a
            # bad gradient. LVGL's sequence fits the newer GC555-style API;
            # this knob exists in LVGL precisely for that split. The basic
            # 256-entry-ramp path (vg_lite_draw_grad) is the one this driver
            # and LVGL agree on.
            # ★ VLC_OP_CLOSE is removed from emitted paths ("Workaround for
            # NXP", LVGL's words). Measured on the GC355: a rect carrying
            # BOTH a gradient fill and a border -- the knob-face recipe --
            # rendered its interior flooded with border colour plus banding
            # and rightward streaks, while the identical borderless gradient
            # beside it was pixel-perfect and every API call reported
            # success. With CLOSE removed the same tile renders correctly.
            # Isolated by the GRADPROBE scene, one primitive per tile.
            # ★ Stroke cache 128, not the default 32: the 16-knob scene draws
            # more distinct strokes per frame than 32, and at that boundary
            # the SECOND animated refresh LIVELOCKED -- CPU cycling
            # draw-task/heap code forever, GPU idle, LVGL log empty, every
            # status green. At 128 the same scene ran 64/64 refreshes. The
            # golden frame is unaffected (first frame never evicts).
            target_compile_definitions(LVGL PUBLIC
                LV_USE_DRAW_VG_LITE=1
                LV_USE_MATRIX=1
                LV_USE_FLOAT=1
                LV_USE_VECTOR_GRAPHIC=1
                LV_VG_LITE_DISABLE_LINEAR_GRADIENT_EXT=1
                LV_VG_LITE_DISABLE_VLC_OP_CLOSE=1
                LV_VG_LITE_STROKE_CACHE_CNT=128
                LV_DRAW_BUF_STRIDE_ALIGN=64
                LV_DRAW_BUF_ALIGN=64)
            # ★ The GPU-mode LVGL runs XIP from flash, not ITCM. Enabling
            # LV_USE_VECTOR_GRAPHIC overflowed the 256K ITCM by 25K; on this
            # path LVGL executes per draw-call (the GPU does the pixels), so
            # flash residency is the cheap place to give that back.
            # imxrt1176.ld routes `*libLVGL_flash.a:(.text*)` to .text.progmem
            # BY THIS NAME -- rename here and there together. Software builds
            # keep the default archive name and their ITCM placement, so the
            # fps baseline is not quietly slowed to make the GPU build fit.
            set_target_properties(LVGL PROPERTIES OUTPUT_NAME "LVGL_flash")
            target_link_libraries(LVGL PUBLIC VGLite)
        endif()
    endif()
endmacro()

# --- VGLite ------------------------------------------------------------------
# Vivante VGLite (MIT) for the GC355, plus this tree's bare-metal port. A plain
# STATIC target for the same reason LVGL is: consumers need the PUBLIC include
# dirs, which teensy_target_link_libraries() would not propagate (it rewrites
# each name to <name>.o).
#
# VG_DRIVER_SINGLE_THREAD is PUBLIC and load-bearing: it is honoured by the
# vendored core and by inc/vg_lite_hal.h, and it is what reduces the port layer
# to the functions port/baremetal/ actually implements. Drop it and the link
# fails on the multi-threaded entry points.
macro(import_evkb_vglite)
    if(NOT TARGET VGLite)
        # Pushed 2026-08-17, so this resolves like any other library: local
        # checkout first, pinned fetch otherwise. The LOCAL-ONLY FATAL_ERROR
        # guard that stood here until then is gone.
        evkb_library_dir(VGLite _evkb_vglite_dir)
        file(GLOB _evkb_vglite_srcs CONFIGURE_DEPENDS
             "${_evkb_vglite_dir}/VGLite/*.c"
             "${_evkb_vglite_dir}/VGLiteKernel/*.c"
             "${_evkb_vglite_dir}/port/baremetal/*.c")
        add_library(VGLite STATIC ${_evkb_vglite_srcs})
        target_include_directories(VGLite PUBLIC
             "${_evkb_vglite_dir}/inc"
             "${_evkb_vglite_dir}/VGLite"
             "${_evkb_vglite_dir}/VGLiteKernel"
             "${_evkb_vglite_dir}/port/baremetal")
        # ★ HEADER_VERSION 7 selects the SILICON's capabilities at COMPILE time
        # from Series/<chip>/<rev>/vg_lite_options.h, dispatched through
        # VG_LITE_OPTIONS. The vendored VGLite/vg_lite_options.h is only a
        # template (`<Series/GCID_REV_CID/vg_lite_options.h>`), so the real path
        # must be supplied here or the driver's gcFEATURE_VG_* constants do not
        # exist -- 70 compile errors, all "did you mean gcFEATURE_BIT_VG_...".
        #
        # ★ THE REVISION IS NOT COSMETIC. vg_lite_init() compares CHIPID,
        # REVISION, CID and ECOID against the real registers and returns
        # VG_LITE_NOT_SUPPORT on ANY mismatch. The two gc355 headers differ
        # ONLY in REVISION (0x1215 vs 0x1216) -- every feature flag is
        # identical -- so a wrong pick costs nothing but a clean, self-
        # diagnosing init failure that prints both sides. vglite_probe reads
        # the registers before init precisely so the board can settle it.
        set(EVKB_VGLITE_SERIES "gc355/0x0_1216" CACHE STRING
            "VGLite Series options dir: <chip>/<rev> under VGLite/Series")
        # The dispatch header wants GCID_REV_CID and builds the include path
        # itself; it #errors by name if unset, which is how this was found.
        target_compile_definitions(VGLite PUBLIC
            VG_DRIVER_SINGLE_THREAD=1
            GCID_REV_CID=${EVKB_VGLITE_SERIES})
        target_link_libraries(VGLite PRIVATE teensy_flags)
        target_link_libraries(VGLite PUBLIC m)
    endif()
endmacro()

# --- SynthUI -----------------------------------------------------------------
# Plain STATIC target like LVGL/CMSIS-DSP: the widgets #include <lvgl.h>, whose
# include dirs only propagate through a real target_link_libraries edge --
# teensy_target_link_libraries() would rewrite the name to SynthUI.o and lose
# them.  PUBLIC LVGL so consumers get lvgl.h and -lm transitively.
macro(import_evkb_synthui)
    if(NOT TARGET SynthUI)
        # Pushed 2026-08-17, so this resolves like any other library: local
        # checkout first, pinned fetch otherwise. The LOCAL-ONLY FATAL_ERROR
        # guard that stood here until then is gone.
        # ★ The pinned SHA moved by more than a commit at that push: the repo's
        # history was rewritten to drop reference/rebirth/ (rights unclear)
        # before it went public, so every SynthUI SHA before e132012 is
        # unreachable. Don't "restore" an older pin from git history here.
        import_evkb_lvgl()
        evkb_library_dir(SynthUI _evkb_synthui_dir)
        file(GLOB _evkb_synthui_src CONFIGURE_DEPENDS
             "${_evkb_synthui_dir}/src/*.cpp")
        add_library(SynthUI STATIC ${_evkb_synthui_src})
        target_include_directories(SynthUI PUBLIC "${_evkb_synthui_dir}/src")
        # m explicit, not just borrowed from LVGL's PUBLIC m above (that link's
        # own comment calls it "inert today", i.e. removable): Task 5's draw
        # port calls cosf/sinf/lroundf directly, so SynthUI needs its own claim
        # on libm regardless of what LVGL keeps.
        target_link_libraries(SynthUI PUBLIC LVGL m)
        target_link_libraries(SynthUI PRIVATE teensy_flags)
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

# import_evkb_audio_full(): build the ENTIRE Audio fork as one `Audio` library
# target for the CM7, with every dependency's headers on the global compile
# path so `#include <Audio.h>` resolves and every node .cpp compiles. Unlike the
# cherry-picking gates, this globs all 87 sources (import_arduino_library) — so
# every node must compile clean on RT1176 (empty-on-1176 nodes -> empty objects;
# --gc-sections drops the unused ones at link).
macro(import_evkb_audio_full)
    if(NOT TARGET Audio)
        # 1. CMSIS-DSP: build the lib AND put the shim+Include dirs on teensy_flags
        #    so the Audio nodes that #include <arm_math.h> compile. The shim dir
        #    MUST be first (intercepts the core's A0-A2/IRQ-inline collisions).
        import_evkb_cmsis_dsp()
        evkb_cmsis_dsp_cm4_sources(_evkb_audio_cmsis_srcs _evkb_audio_cmsis_incs)
        # (evkb_cmsis_dsp_cm4_sources returns the SAME include dir list the CM4
        #  path uses: shim, DSP/Include, DSP/PrivateInclude, Core/Include — the
        #  shim is first. We reuse only the include list, not the sources.)
        teensy_set_dynamic_properties()   # ensure teensy_flags exists
        foreach(_inc ${_evkb_audio_cmsis_incs})
            target_include_directories(teensy_flags INTERFACE "${_inc}")
        endforeach()
        # 2. The peripheral-lib deps Audio's headers reach (Wire/SD/SdFat/
        #    SerialFlash/USBHost_t36/EEPROM). import_evkb_library adds each lib
        #    root to teensy_flags (global) and creates its target; the linker
        #    RESCAN group resolves cross-refs regardless of order. EEPROM is
        #    needed because USBHost_t36's bluetooth.cpp #includes <EEPROM.h> —
        #    every other example that imports USBHost_t36 pairs it with EEPROM
        #    for the same reason (see e.g. usb_host_hid_test/CMakeLists.txt).
        import_evkb_library(Wire)
        import_evkb_library(SPI)
        import_evkb_library(SdFat src)
        import_evkb_library(SD src)
        import_evkb_library(SerialFlash)
        import_evkb_library(USBHost_t36)
        import_evkb_library(EEPROM)
        # 3. The whole Audio fork (root globs 87 .cpp/6 .c; utility/ adds 2).
        import_evkb_library(Audio utility)
    endif()
endmacro()

# --- the core (every example needs it) ---------------------------------------
evkb_library_dir(cores EVKB_CORES_DIR)
# A core directory that exists but carries no core is otherwise diagnosed
# hundreds of lines later as "Arduino.h: No such file or directory".
if(NOT EXISTS "${EVKB_CORES_DIR}/core_pins.h")
    message(FATAL_ERROR "resolved core at '${EVKB_CORES_DIR}' has no core_pins.h — stale or empty ${EVKB_BOARD} core checkout?")
endif()
# Re-point COREPATH at the resolved core (replacing the pre-include value set
# above). MUST happen BEFORE the first import_arduino_library call — that call
# runs teensy_set_dynamic_properties once, baking COREPATH into the link flags.
# Trailing slash required: the macros build LINKER_FILE as
# "${COREPATH}imxrt1176.ld" (117) or "${COREPATH}imxrt1060_evkb.ld" (42).
set(COREPATH "${EVKB_CORES_DIR}/" CACHE STRING "resolved core path" FORCE)
# CMP0126 (NEW here): the cache FORCE above does not remove the pre-include
# NORMAL COREPATH variable, which would otherwise shadow the cache for every
# later read — on the fetch path that put the placeholder on the link line.
set(COREPATH "${EVKB_CORES_DIR}/")
import_arduino_library(cores "${EVKB_CORES_DIR}")

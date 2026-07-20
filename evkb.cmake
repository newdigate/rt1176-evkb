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
        GIT_TAG        b92b235e0717f403265a9b98f4406c1749573319)
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
_evkb_lib(cores          ${EVKB_ROOT}/cores/imxrt1176 https://github.com/newdigate/teensy-cores    483b279c003d10452d79200d250111d3dacdf39a imxrt1176)
_evkb_lib(Wire           ${_dev}/Wire                 https://github.com/newdigate/Wire            193e949c51c8da316df6ed619e019e308acaca45 .)
_evkb_lib(SPI            ${_dev}/SPI                  https://github.com/newdigate/SPI             eefd8798c74a727a09f38d34d79e1ab55c0110b3 .)
_evkb_lib(Audio          ${_dev}/Audio                https://github.com/newdigate/Audio           0d9501ea9a73b0233efd21c8a26aad045918e897 .)
_evkb_lib(SdFat          ${_dev}/SdFat                https://github.com/newdigate/SdFat           681bfcf83d05beb943e3d905f15d8181bf9072c7 .)
_evkb_lib(SD             ${_dev}/PaulS_SD             https://github.com/newdigate/SD              e28c549918ea34ffb2942fd84deffc7c76a89880 .)
_evkb_lib(ethernet       ${_dev}/Ethernet             https://github.com/newdigate/Ethernet        eebbfebc699a1500864236db21d17abf3cf7535a .)
_evkb_lib(nativeethernet ${_dev}/NativeEthernet       https://github.com/newdigate/NativeEthernet  7f5d881d5da80540177caea760d895780478b128 .)
_evkb_lib(fnet           ${_dev}/FNET/src             https://github.com/newdigate/FNET            a50373d50e57778595eb388b7bfeaad79080a077 src)
_evkb_lib(lwip           ${_dev}/lwip                 https://github.com/newdigate/lwip            03dddc67f73113e2beb3807e290a368d5cb7cfe0 .)
_evkb_lib(USBHost_t36    ${_dev}/USBHost_t36          https://github.com/newdigate/USBHost_t36     77c23a6a1c692c987e9c27e4caa1a3ea402ca92f .)
_evkb_lib(EEPROM         ${_dev}/EEPROM               https://github.com/PaulStoffregen/EEPROM     9790da76d62bc633563f763c3dc1526539ed0a6b .)
_evkb_lib(Bounce2        ${_dev}/Bounce2/src          https://github.com/PaulStoffregen/Bounce2    eb5ab9fad8a15539743315786beb8236e96c8b9a src)

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

# --- the imxrt1176 core (every example needs it) -----------------------------
evkb_library_dir(cores EVKB_CORES_DIR)
# The toolchain file guessed COREPATH from its own location; re-point it at the
# resolved core so a fresh clone (fetched core) links the right imxrt1176.ld.
# MUST happen BEFORE the first import_arduino_library call — that call runs
# teensy_set_dynamic_properties once, baking COREPATH into the link flags.
# Trailing slash required (LINKER_FILE is "${COREPATH}imxrt1176.ld").
set(COREPATH "${EVKB_CORES_DIR}/" CACHE STRING "imxrt1176 core path" FORCE)
import_arduino_library(cores "${EVKB_CORES_DIR}")

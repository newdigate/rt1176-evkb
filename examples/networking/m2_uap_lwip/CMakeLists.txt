cmake_minimum_required(VERSION 3.24)
project(m2_uap_lwip)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ Static ARP entries, and this must come BEFORE the lwip import so it reaches
# lwip's own sources and not just ours.  A DHCP server has to reply to a client
# that does not yet hold the address being offered, so ordinary ARP resolution
# cannot work -- the standard answer is to install the mapping directly from the
# chaddr being replied to.  lwip defaults this off (opt.h) and the shared port's
# lwipopts.h does not set it, so the #ifndef default is what we are overriding.
#
# Scoped to THIS example: add_definitions is directory-scoped and every example
# configures its own lwip in its own build directory, so m2_lwip_test and the
# rest are untouched.
add_definitions(-DETHARP_SUPPORT_STATIC_ENTRIES=1)

# lwip exactly as lwip_test imports it (port/ carries lwipopts.h + arch/;
# port/ethernetif.c also compiles but is inert here -- nothing calls it).
import_evkb_library(lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
# M2Radio: sdio + iw416 as the probe, PLUS the lwip netif glue subdir.
import_evkb_library(M2Radio sdio iw416 lwip)

# --- IW416 firmware blob (NOT vendored) -- same rules as m2_sdio_probe ------
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- NO CREDENTIALS HERE, and that is deliberate ---------------------------
# m2_lwip_test needs an SSID + PSK because it JOINS someone else's network.
# This example HOSTS an open one, so there is nothing to supply and nothing that
# could reach a commit.  When WPA2 lands, the passphrase follows the same
# configure-time pattern as the firmware blob above -- never a literal in
# source.  This repo has leaked live Wi-Fi passwords into pushed history once.
set(M2_UAP_SSID "RT1176-UAP-TEST" CACHE STRING "SSID this example hosts")
set(M2_UAP_CHANNEL "6" CACHE STRING "2.4 GHz channel to host on")
add_definitions(-DM2_UAP_SSID="${M2_UAP_SSID}" -DM2_UAP_CHANNEL=${M2_UAP_CHANNEL})

# --- WPA2 passphrase: CONFIGURE TIME ONLY, never a tracked file -------------
# Supply with -DM2_UAP_PSK="a throwaway passphrase".  Omit it and the AP comes
# up OPEN, which is what every gate and every earlier W17 run used.
#
#   cmake -B build-hw ... -DM2_UAP_PSK="somethingthrowaway"
#
# The passphrase is written to uap_creds.h in the BUILD DIRECTORY, which is
# gitignored -- the same scratchpad -> cache -> generated-header path the rest
# of this repo uses (tools/esp32c6-benchap/make-creds.sh documents why).  This
# project leaked live Wi-Fi passwords into PUSHED history once; the rule exists
# because redacting at HEAD does not undo that.
#
# ★ Use a FRESH THROWAWAY passphrase.  Never a real network's, and never one
# reused anywhere else: it is compiled into an .elf that lives in a build
# directory and is trivially recoverable with `strings`.
set(M2_UAP_PSK "" CACHE STRING "WPA2 passphrase (8..63 chars); empty = open network")
if(M2_UAP_PSK)
    string(LENGTH "${M2_UAP_PSK}" _psklen)
    if(_psklen LESS 8 OR _psklen GREATER 63)
        message(FATAL_ERROR
            "M2_UAP_PSK must be 8..63 characters (got ${_psklen}). "
            "A shorter one is not a weak passphrase -- WPA2 rejects it, and the "
            "AP would silently come up OPEN while the log still said WPA2.")
    endif()
    # Refuse to generate into anything git would offer to commit.  The build
    # directory is normally ignored; if someone configures in-tree, stop.
    execute_process(COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" check-ignore -q
                            "${CMAKE_CURRENT_BINARY_DIR}/uap_creds.h"
                    RESULT_VARIABLE _ignored)
    if(NOT _ignored EQUAL 0)
        message(FATAL_ERROR
            "${CMAKE_CURRENT_BINARY_DIR}/uap_creds.h is NOT gitignored -- refusing "
            "to write a credential there.  Configure into a build/ directory.")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uap_creds.h"
"// Generated at configure time from -DM2_UAP_PSK.  NEVER committed.\n"
"#pragma once\n"
"#define M2_UAP_PSK_STR \"${M2_UAP_PSK}\"\n")
    add_definitions(-DHAVE_UAP_PSK=1)
    message(STATUS "uAP security: WPA2-PSK (passphrase ${_psklen} chars, not logged)")
else()
    message(STATUS "uAP security: OPEN (no -DM2_UAP_PSK given)")
endif()

# W17: measure AP->client ARP loss per request, with the destination as the one
# variable (broadcast is DTIM-buffered and sent at a basic rate; unicast is not).
option(ARP_LOSS_PROBE   "Run the AP->client ARP loss probe once a client joins" OFF)
option(ARP_PROBE_UNICAST "Send the probe unicast rather than broadcast" OFF)
if(ARP_LOSS_PROBE)
    add_definitions(-DARP_LOSS_PROBE=1)
    message(STATUS "uAP: ARP loss probe ENABLED")
endif()
if(ARP_PROBE_UNICAST)
    add_definitions(-DARP_PROBE_UNICAST=1)
endif()

teensy_add_executable(m2_uap_lwip m2_uap_lwip.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_uap_lwip cores M2Radio lwip)
target_include_directories(m2_uap_lwip.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(m2_uap_lwip.elf stdc++)

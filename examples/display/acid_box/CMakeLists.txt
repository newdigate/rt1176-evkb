cmake_minimum_required(VERSION 3.24)
project(acid_box)

# XRGB8888 exactly like synthui_step_test / synthui_knob_test.  Directory scope
# on purpose: the definitions must reach the LVGL, MipiDisplay AND SynthUI
# objects created by the imports below, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ FIRST EXAMPLE IN THIS TREE TO LINK THE AUDIO LIBRARY AND LVGL TOGETHER.
# Every display example so far was display-only and every audio example was
# audio-only, so the combined include path, the combined symbol set and the
# combined RAM footprint had never been exercised.  Proving that link is half
# of what this slice of the capstone is for; the other half is the note pump.
import_evkb_audio_full()

import_evkb_lvgl()
# VGLITE flavor: compiles SynthUI's rotary-knob GPU compositor and links the
# VGLite driver.  LVGL stays the SOFTWARE renderer (one ELF, both engines --
# the rotary_knob_bench pattern): the GC355 is reached only by direct vg_lite
# calls behind the chip-ID probe, so this same image is safe in QEMU.
import_evkb_synthui(VGLITE)
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)              # Display::fillScreen() paints via the PXP
import_evkb_library(TouchPanel gt911) # controller chosen by the importer

# --- Loop / frame / touch instrumentation (opt-in, bench) ------------------
# OFF by default: nothing compiles in and the loadable image is byte-identical
# (Task 4 of the NEW-33 plan diffs the objcopy -O binary; the ELF's DWARF moves
# with every edited line), so the gate, both goldens and the vacuity fixture
# are untouched.  ON prints loopstat/framestat/touchstat once a second and makes
# the "ACID BOX" title a tap-toggle for a synthetic knob wiggle -- see
# docs/superpowers/specs/2026-09-04-acid-box-bt-ui-responsiveness-design.md §1.
# Independent of M2_BT_OUT on purpose: the same instrument measures both builds.
option(ACIDBOX_LOOPSTAT "Per-slot loop timing, frame and touch latency lines (bench)" OFF)
if(ACIDBOX_LOOPSTAT)
    add_definitions(-DACIDBOX_LOOPSTAT=1)
endif()

# --- Bluetooth A2DP audio output (opt-in) -----------------------------------
# OFF by default: nothing below compiles in and the ELF is byte-identical, so
# the golden run_qemu.sh gate is untouched.  ON links the M.2 BT stack and adds
# an AudioOutputBluetooth sink alongside the I2S/WM8962 path (see
# docs/superpowers/specs/2026-09-04-acid-box-bluetooth-output-design.md).
option(M2_BT_OUT "Stream the synth audio to a Bluetooth A2DP sink (bench)" OFF)
if(M2_BT_OUT)
    add_definitions(-DM2_BT_OUT=1)
    import_evkb_library(M2Radio sdio iw416 hci bt)

    # The BT bench knobs the headset path needs (same as audio/bt_tone_test).
    option(M2_BT_UART_DNLD "Run the BT-only V3 UART firmware download" ON)
    if(M2_BT_UART_DNLD)
    else()
        add_definitions(-DM2_BT_NO_UART_DNLD=1)
    endif()
    option(M2_BT_WAKE_PULSE "Pulse the boot-sleep wake line before the BT firmware download" ON)
    if(M2_BT_WAKE_PULSE)
        add_definitions(-DM2_BT_WAKE_PULSE=1)
    endif()
    option(M2_BT_FORGET_BONDS "Bench: wipe the bonded-device store at boot (NEW-34 control arm)" OFF)
    if(M2_BT_FORGET_BONDS)
        add_definitions(-DM2_BT_FORGET_BONDS=1)
    endif()
    option(M2_BT_RTS_FLOW "Hardware RXRTSE flow control on LPUART2 (Serial2.attachRts)" ON)
    set(M2_BT_RTS_WATER "1" CACHE STRING "MODIR RTSWATER watermark for M2_BT_RTS_FLOW (0..3)")
    if(M2_BT_RTS_FLOW)
        add_definitions(-DM2_BT_RTS_FLOW=1 -DM2_BT_RTS_WATER=${M2_BT_RTS_WATER})
    endif()
    set(M2_BT_TARGET_NAME "" CACHE STRING "Prefer the A2DP sink whose inquiry name contains this substring")
    if(NOT M2_BT_TARGET_NAME STREQUAL "")
        add_definitions(-DM2_BT_TARGET_NAME="${M2_BT_TARGET_NAME}")
    endif()
    option(M2_BT_LEGACY_PIN "Write_Simple_Pairing_Mode=0: legacy PIN pairing (1234)" OFF)
    if(M2_BT_LEGACY_PIN)
        add_definitions(-DM2_BT_LEGACY_PIN=1)
    endif()
    # NEW-34 piece 2: the reconnect retry cadence (ms between lost-peer page attempts).  Default in the
    # library is 10 s.  Same knob as audio/bt_tone_test; Task 15's silicon bench matrix runs against
    # "bt_tone_test (or acid_box -DM2_BT_OUT=ON)" so both examples must honour it.
    set(M2_BT_RETRY_MS "" CACHE STRING "BtSession retry cadence in ms (empty = library default 10000)")
    if(NOT M2_BT_RETRY_MS STREQUAL "")
        add_definitions(-DM2_BT_RETRY_MS=${M2_BT_RETRY_MS})
    endif()
    # NEW-34 piece 2 bench knob: link supervision timeout in ms (0/empty = controller default ~20 s).
    # Task 15's "Supervision knob" bench step (-DM2_BT_SUPERVISION_MS=5000) is run against this example too.
    set(M2_BT_SUPERVISION_MS "" CACHE STRING "Write_Link_Supervision_Timeout in ms (empty = controller default)")
    if(NOT M2_BT_SUPERVISION_MS STREQUAL "")
        add_definitions(-DM2_BT_SUPERVISION_MS=${M2_BT_SUPERVISION_MS})
    endif()
    # (No M2_BT_CONNECT_RETRY knob: acid_box's loop() ALWAYS defers + retries the
    # connect -- a bench build has no reason to want a one-shot setup() connect --
    # so the switch would be inert.  bt_tone_test keeps it because it gates that mode.)
    option(M2_BT_FAST_BAUD "Switch HCI to M2_BT_FAST_BAUD_RATE after identity" ON)
    set(M2_BT_FAST_BAUD_RATE "3000000" CACHE STRING "Rate for M2_BT_FAST_BAUD")
    if(M2_BT_FAST_BAUD)
        add_definitions(-DM2_BT_FAST_BAUD=${M2_BT_FAST_BAUD_RATE})
    endif()

    # The BT-only UART firmware image (NOT vendored), same rule + synthetic fallback
    # as bt_tone_test/m2_hci_probe.
    set(M2RADIO_IW416_BT_FW "" CACHE FILEPATH "IW416 BT-only UART firmware .bin.inc from an NXP SDK")
    if(M2RADIO_IW416_BT_FW AND EXISTS "${M2RADIO_IW416_BT_FW}")
        message(STATUS "IW416 BT firmware: ${M2RADIO_IW416_BT_FW}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_BT_FW}\"\n};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
        set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
        add_definitions(-DHAVE_IW416_BT_FW=1)
    else()
        message(STATUS "IW416 BT firmware: not supplied -- using a 1 KB SYNTHETIC image")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n"
"#define R16(n) (uint8_t)(n),(uint8_t)(n+1),(uint8_t)(n+2),(uint8_t)(n+3),(uint8_t)(n+4),(uint8_t)(n+5),(uint8_t)(n+6),(uint8_t)(n+7),(uint8_t)(n+8),(uint8_t)(n+9),(uint8_t)(n+10),(uint8_t)(n+11),(uint8_t)(n+12),(uint8_t)(n+13),(uint8_t)(n+14),(uint8_t)(n+15)\n"
"#define R256(n) R16(n),R16(n+16),R16(n+32),R16(n+48),R16(n+64),R16(n+80),R16(n+96),R16(n+112),R16(n+128),R16(n+144),R16(n+160),R16(n+176),R16(n+192),R16(n+208),R16(n+224),R16(n+240)\n"
"R256(0), R256(1), R256(2), R256(3)\n"
"};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
        set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
        add_definitions(-DHAVE_IW416_BT_FW=1 -DBT_FW_IS_SYNTHETIC=1)
    endif()
else()
    set(M2_BT_FW_SRC "")
endif()

evkb_library_dir(LVGL _lvgl_dir)

# The BT-audio bridge is shared with audio/bt_tone_test (one copy, not a fork).
if(M2_BT_OUT)
    set(M2_BT_OUT_SRCS
        ${CMAKE_CURRENT_LIST_DIR}/../../audio/bt_tone_test/AudioOutputBluetooth.cpp
        ${M2_BT_FW_SRC})
    include_directories(${CMAKE_CURRENT_LIST_DIR}/../../audio/bt_tone_test)
else()
    set(M2_BT_OUT_SRCS "")
endif()

teensy_add_executable(acid_box
    acid_box.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_pxp_copy.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp
    ${M2_BT_OUT_SRCS})
# ITCM margin on EVERY link, not only M2_BT_OUT.  The landscape rework (spec
# 2026-09-14-acid-box-landscape-design.md) took the DEFAULT build's ITCM headroom
# from 4,276 B to 836 B in one commit -- the port's rotated present compiles into
# this executable (no archive routing applies to it) -- and nothing printed it:
# the NEW-45 floor ASSERT below exists only for M2_BT_OUT links.  A linker flag
# that only reports; the loadable image is unchanged.
# ★ MEASURED 2026-09-15, after moving the witnesses, build_ui/mkknob/mkbtn and
# create_rotated to flash: headroom default 2,756 B, ACIDBOX_LOOPSTAT (BT off)
# 2,612 B, build-bt 11,396 B, build-bench 11,220 B (both were 13,968/13,776
# before the landscape present; the ~2.5 KB spent is rot_flush_cb + rot_present,
# which stay in ITCM because they run every frame).  The NON-BT builds are now
# the tight ones -- the BT builds route more archives to flash -- and they carry
# no floor: read this line in their build output.
# ★ RE-MEASURED 2026-09-15, after the equality guard went incremental: -32 B on
# all four -- default 2,724 B (2,708 B after the close-out null guard), LOOPSTAT 2,580 B, build-bt 11,364 B, build-bench
# 11,188 B.  The guard's functions are all in flash; nm-diffed against the
# previous default ELF, ITCM gained two long-branch veneers (+16 B) and loop()
# grew 20 B (the inlined per-pass active test and the step-8 arm), net of
# alignment.
# ★ RE-MEASURED 2026-09-16, NEW-25 Task 4: two more SynthUI widgets landed with
# the editor -- led_button and lamp -- and overflowed the DEFAULT build's ITCM:
# led_button 2952 + lamp 1424 B, against a 646 B saving elsewhere (the step
# lane reusing led_button geometry rather than adding its own), net +3730 B
# on a 2708 B headroom -- the 988 B `region ITCM overflowed` this task started
# from.  Fixed by moving the libSynthUI routing rule (excluding only the
# rotary knob and its GC355 compositor) from M2_BT_OUT-only to EVERY
# configuration -- see the ITCM relief block below.
# ★ RE-MEASURED 2026-09-16 AT MERGE: all FOUR SynthUI widgets are linked now --
# `nm` shows `synthui_seven_segment_create` and `synthui_panel_button_create`
# in flash beside led_button and lamp, referenced by the editor's step readout
# and its REWIND/FORWARD panel buttons.  They land in FLASH under the same
# whole-archive routing rule, so their ITCM cost is ~0, already paid for.
# Headroom: default 2,884 B (259,260 B used), LOOPSTAT 2,756 B (259,388 B
# used), build-bt 11,604 B (250,540 B used), build-bench 11,412 B (250,732 B
# used).  QEMU golden ACIDBOX_UI_SUM=0xBB2AEE59 -- the re-record for the
# editor's own frame happened in a later task; routing code to flash did not
# move a pixel by itself.
# ★ SUPERSEDED 2026-09-17 by NEW-54 (top bar and SAW/SQR onto SynthUI widgets):
# golden 0xBB2AEE59 -> 0x18B7B637, and the headroom above reads 2,900 / 2,756 /
# 11,604 / 11,428 B.  The four numbers are annotated rather than replaced
# because the paragraph is a dated merge measurement -- but note that THREE of
# them did not move, which makes the one that did (the default build, +16 B)
# easy to miss.  That +16 is one quantum of an instrument quantised to 16 bytes
# (.text.itcm ends with ALIGN(16); see CLAUDE.md's 2026-09-17 block), so it is
# indistinguishable from no change at all.
# ★ RE-MEASURED 2026-09-19, NEW-55 Task 6 (the sync-copy handler installed from
# lvgl_mipi_panel_create_db): headroom 2,804 / 2,660 / 11,524 / 11,332 B
# (259,340 / 259,484 / 250,620 / 250,812 B used).  All four fell 64-96 B against
# the line above, and NONE of it is Task 6's -- MEASURED by a controlled A/B, not
# argued: with this task's LVGL-port and CMakeLists changes reverted in place,
# `build` relinked to the IDENTICAL 259,340 B, and `nm` finds zero
# lvgl_pxp_copy_* / lvgl_mipi_panel_sync_* / _create_db symbols in any of the
# four ELFs.  acid_box calls create_rotated(), never create_db(), so
# --gc-sections drops the handler and its installer entirely; adding
# port/lvgl_pxp_copy.cpp to the source list above costs this example 0 B.
# The 64-96 B belongs to the PXP pin bump earlier on this branch (PXP 354eda7,
# PXPOp::alphaOut): libPXP is NOT in the flash-routing rules below, so its new
# OUT_CTRL[ALPHA_OUTPUT] code in PXPOp::_program() lands in ITCM -- and acid_box
# links PXP for the rotated present.  Worth writing down because the obvious
# reading ("the new sync-copy source file cost us ITCM") is wrong, and the four
# deltas differ (64/96/80/96) only because the metric is 16-B quantised.
# ★ One 2,048 B floor (the NEW-45 ASSERT below) guards all four builds, but
# their headroom differs by about 4x -- in practice it guards the non-BT
# builds, which is the right call: they are the tight ones, while the BT
# builds carry ~9.5 KB of unguarded room above the floor.
# ★ NEXT LEVER IF A LATER TASK OVERFLOWS AGAIN (review note, 2026-09-16):
# synthui_rotary_knob_gpu.cpp.obj is ~3188 B of .text sitting in ITCM and
# NEVER EXECUTES in the default/QEMU build -- the GC355 path is silicon-only,
# gated behind the chip-ID probe.  Routing it to flash would roughly double
# the default build's headroom.  NOT done here: on silicon it IS per-frame
# (the GPU compositor runs under a drag), so unlike led_button/lamp this one
# must be BENCHED before moving, not assumed safe from its QEMU inertness.
target_link_options(acid_box.elf PRIVATE -Wl,--print-memory-usage)
# Audio's own dependency set (Wire/SPI/SdFat/SD/SerialFlash) is named exactly as
# acid_bass_test names it -- import_evkb_audio_full() creates those targets but
# does not link them.  Wire covers Wire2 = LPI2C5, shared by the codec and the
# touch controller, so TouchPanel needs no bus import of its own.
teensy_target_link_libraries(acid_box
    cores Audio Wire SPI SdFat SD SerialFlash MipiDisplay PXP TouchPanel)
if(M2_BT_OUT)
    teensy_target_link_libraries(acid_box M2Radio)
    target_include_directories(acid_box.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ITCM relief, on EVERY configuration since the NEW-25 SynthUI-editor consumer
# rework (docs/superpowers/plans/2026-09-16-acid-box-synthui-editor.md, Task 4,
# pulled forward from Task order because Task 1 proved the tree does not link
# without it): the editor links two more SynthUI widgets today (led_button,
# lamp -- see the widget-count note below), and the DEFAULT build overflowed
# ITCM by 988 B before this change -- not just M2_BT_OUT.
# Derive a linker script from the core imxrt1176.ld that routes named archives'
# .text to FLASH (XIP) -- the SAME mechanism the core already uses for
# libLVGL_flash.a in VGLite mode -- and point ONLY this target at it.
# This whole derivation used to live entirely inside `if(M2_BT_OUT)`, because
# only the BT build needed it.  The SynthUI routing rule and the NEW-45
# headroom ASSERT below now both apply UNCONDITIONALLY (the ASSERT moved
# out of M2_BT_OUT on 2026-09-16 -- see its own comment for why); only the
# BT-only archive rules (M2Radio, VGLite, MipiDisplay/Wire/TouchPanel) stay
# gated on M2_BT_OUT, because only the BT link carries those archives.
# TWO RULES FOR EVERY ROUTING LINE BELOW (NEW-45, spec 2026-09-11-acid-box-itcm-headroom-design.md §5):
#  1. Route a WHOLE ARCHIVE with EXCLUDE_FILE for named hot exceptions -- never an inclusion list of
#     objects.  An exclusion rule makes a newly added library file default to FLASH; an inclusion list
#     makes it default to ITCM, which is exactly how Avrcp.cpp overflowed ITCM for a day on 2026-09-07.
#  2. Do NOT capture `.fastrun` in a new rule, so FASTRUN keeps meaning "ITCM" and a library can pin one
#     hot function back without a linker-script change.  The libM2Radio line below predates this rule and
#     still captures .fastrun; it is inert only because M2Radio marks nothing FASTRUN.  If a
#     latency-sensitive M2Radio routine is ever marked FASTRUN, drop `.fastrun` from that line.
get_target_property(_acidbt_lf acid_box.elf LINK_FLAGS)
string(REGEX MATCH "-T([^ ]*imxrt1176[.]ld)" _acidbt_ignore "${_acidbt_lf}")
set(_acidbt_coreld "${CMAKE_MATCH_1}")

# NEW-45: make the ITCM margin legible instead of only fatal.  EVERY
# configuration now, not only M2_BT_OUT -- corrected 2026-09-16.  It used to
# be BT-only on the theory that only the M2Radio-sized library growth needed
# catching; this very commit is the counter-example, since the 988 B overflow
# that started Task 4 was in the DEFAULT build, from SynthUI, not from
# M2Radio.  Measured margins bear that out directly: 3,156 B (default) / 3,012
# B (LOOPSTAT) against 11,812 / 11,652 B (build-bt / build-bench) -- the
# builds that used to carry the guard have 3.7x the margin of the ones that
# didn't.  The non-BT builds are now the TIGHT ones, and are guarded for
# exactly that reason.
set(ACIDBOX_ITCM_MIN_HEADROOM "2048" CACHE STRING "Fail the link when ITCM headroom drops below this many bytes.  2048 because measured headroom after the NEW-45 VGLite routing is ~9.8 KB on the BT builds and ~3 KB on the non-BT builds: low enough that neither nags, high enough that a growth the size of the one that opened NEW-45 (+412 B from a library pin bump) or the one that opened Task 4 (+3730 B from two SynthUI widgets, 988 B over the old non-BT headroom) reports a named headroom failure instead of a raw 'region ITCM overflowed' link error")

if(_acidbt_coreld AND EXISTS "${_acidbt_coreld}")
    file(READ "${_acidbt_coreld}" _acidbt_ld)

    # Two halves:
    #   * --print-memory-usage prints region used/size/percent on EVERY link, so a shrinking margin shows up
    #     in ordinary build output rather than only at the cliff;
    #   * an ASSERT so the next NEW-41-sized (or Task-4-sized) library growth reports "ITCM headroom below
    #     2048" instead of "region ITCM overflowed by 140 bytes".  ld cannot interpolate a number into an
    #     ASSERT message, so the literal is injected here at configure time along with the expression.
    # WHAT THE REGION ACTUALLY HOLDS, measured rather than assumed: .text.itcm is 252336 B at VMA 0, so it
    # ends at 252336, and .ARM.exidx is 8 B at VMA 252340 -- 4 B of alignment padding sits between them.  The
    # region's true top is therefore 252348, and 252348 is the number --print-memory-usage reports.  The
    # ASSERT sums SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) = 252344, so it does NOT see that padding and is
    # still 4 B OPTIMISTIC.  It is not exact and is not claimed to be: a .text.itcm-only assert would have
    # been 12 B optimistic, this one is 4, and 4 B cannot decide anything against a 2048 B floor.  Recorded
    # here rather than hidden, because a guard whose slack is undocumented is a guard nobody can reason about.
    # The exact expression is ADDR(.ARM.exidx) + SIZEOF(.ARM.exidx) - ORIGIN(ITCM), and it is DECLINED
    # deliberately: this linker script already measures the region as SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx),
    # in _itcm_block_count EIGHTEEN LINES ABOVE this assert (line 146 against 165 in the generated
    # acid_box_itcm.ld).  One convention that is 4 B loose beats two that disagree -- inventing a third way to
    # measure ITCM in the same file is how the two get out of step later.
    string(REPLACE
        "ASSERT(__text_csf_end <= 0x30FC0000, \"Image overlaps the EEPROM flash region (top 256K)\")"
        "ASSERT(__text_csf_end <= 0x30FC0000, \"Image overlaps the EEPROM flash region (top 256K)\")
	ASSERT(SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) <= LENGTH(ITCM) - ${ACIDBOX_ITCM_MIN_HEADROOM}, \"NEW-45: ITCM headroom below ${ACIDBOX_ITCM_MIN_HEADROOM} bytes -- see docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md; see the NEXT LEVER note in examples/display/acid_box/CMakeLists.txt\")"
        _acidbt_ld "${_acidbt_ld}")
    # string(REPLACE) is a SILENT NO-OP when its anchor does not match, and the anchor above is a specific
    # line of the CORE imxrt1176.ld -- which lives in ANOTHER REPOSITORY (teensy-cores, resolved via
    # TEENSY_LIB_ROOT or fetched at the SHA evkb.cmake pins).  Reword that line upstream and the headroom
    # ASSERT quietly stops being injected while every build keeps passing with no guard at all: precisely
    # the silent-failure class this whole feature exists to close, relocated one level up.  So check it.
    # ASYMMETRY, which is why only this one is checked: if the OTHER string(REPLACE) below (anchored on
    # *libLVGL_flash.a:(.text*)) ever fails to match, every routing rule vanishes, ITCM overflows and the
    # link fails loudly on its own -- that failure is self-announcing.  This one's failure is silence.
    string(FIND "${_acidbt_ld}" "NEW-45: ITCM headroom" _acidbt_found)
    if(_acidbt_found EQUAL -1)
        message(FATAL_ERROR
            "acid_box: the NEW-45 ITCM headroom ASSERT was NOT injected into the derived linker script. "
            "The EEPROM-assert line this rule anchors on has changed in the core script "
            "(${_acidbt_coreld}), so string(REPLACE) matched nothing and did nothing.  Left unchecked "
            "this build would link UNGUARDED and the next library growth would report a raw 'region ITCM "
            "overflowed by N bytes' instead of a named headroom failure.  Re-anchor the string(REPLACE) "
            "in examples/display/acid_box/CMakeLists.txt against that script's current ASSERT text.")
    endif()

    # SynthUI routing -- applies to EVERY configuration; anchored the same place
    # the BT-only rules anchor (right after the core's own libLVGL_flash.a rule).
    set(_acidbt_routing "*libLVGL_flash.a:(.text*)
	/* NEW-25 consumer rework: the editor links FOUR SynthUI widgets --
	   led_button, lamp, seven_segment and panel_button, the last two
	   referenced by the step readout and the REWIND/FORWARD panel buttons --
	   and ITCM is the binding constraint -- the default build had 2,708 B of
	   headroom before led_button+lamp (net +3730 B) overflowed it by 988 B.
	   These widgets are called per DRAW CALL at UI rate, the same shape as
	   MipiDisplay/Wire/TouchPanel, which measured fine from flash on
	   2026-09-11 (NEW-45 arm b) with the I-cache covering them.  Routing the
	   WHOLE archive means seven_segment/panel_button land in flash too, the
	   moment something references them -- their ITCM cost is paid now, at 0.
	   EXCLUDED and kept in ITCM: the rotary knob and its GC355 compositor,
	   which run per FRAME under a drag (wedge-delta + pre-flip compose).
	   Whole archive with an exclusion, per NEW-45 rule 1, so a NEW SynthUI
	   file defaults to FLASH rather than to ITCM.  No `.fastrun`, per rule 2. */
	*libSynthUI*.a:(EXCLUDE_FILE(*synthui_rotary_knob.cpp.obj *synthui_rotary_knob_gpu.cpp.obj) .text*)")

    if(M2_BT_OUT)
        set(_acidbt_routing "${_acidbt_routing}
	*acid_box.cpp.obj(.text.setup .text._GLOBAL__sub* .text.startup._GLOBAL__sub*)
	/* Every libM2Radio object EXCEPT Sbc.cpp (the per-audio-block-hot SBC encoder: ITCM by design, NEW-9)
	   goes to flash.  Until 2026-09-08 this was a hand-maintained list of eighteen per-object lines, and
	   Avrcp.cpp (added to the library 2026-09-07) was missing from it: the M2_BT_OUT bench builds overflowed
	   ITCM by 316-652 B for a day before anyone rebuilt them.  A wildcard cannot drift when the library grows;
	   the acceptance for the swap was an IDENTICAL ITCM symbol set and .text.itcm size in all four bench builds. */
	*libM2Radio*.a:(EXCLUDE_FILE(*Sbc.cpp.obj) .text* EXCLUDE_FILE(*Sbc.cpp.obj) .fastrun)
	/* NEW-45 phase 0.  libVGLite is the largest non-LVGL ITCM consumer (9984 B, of which vg_lite.c is
	   5640) and is coarse-grained BY CONSTRUCTION: on this build the GC355 does the pixels and VGLite is
	   called per DRAW CALL -- the same shape as the SdpServer/L2cap service chain, which measured
	   0.46 us/iter from flash after NEW-36 enabled the I-cache.  Note vg_lite_hal.c/vg_lite_os.c carry
	   the GPU2D handler (attached at vg_lite_hal.c:248), so this puts an ISR in flash: the claim that
	   this is a first-use miss, not a steady-state penalty, is PROVISIONAL -- unmeasured until the
	   silicon arm runs -- and is the reason the bench watches ACIDBOX_VSYNC and max_us, not only fps.
	   No `.fastrun` here, per rule 2 above. */
	*libVGLite*.a:(.text*)
	/* NEW-45 arm (b), MEASURED on silicon 2026-09-11 and kept.  Three more archives that are cold or
	   coarse BY CONSTRUCTION: I2C to the codec and touch controller at UI rate, and the DSI bring-up
	   plus flip path.  Takes headroom 9,728 -> 13,776 B, which is 33x the +412 B of library growth
	   that broke this build in the first place.  Cost, arm (b) against arm (a) in the same session:
	   fps and frame interval unchanged (30 @ 33,511 us vs 29 @ 33,321), enc 125.8 vs 125.6 us/block,
	   touch p95 71.5 ms (inside the 78 ms bound), pcmdrops 0.
	   ★ libMipiDisplay's lcdifv2.cpp carries the LCDIFv2 VSYNC ISR, so this puts a display interrupt
	   in flash.  That was the named risk and it is answered: ACIDBOX_VSYNC timeouts=0 across 801
	   fence lines, flips == isrs.  A first-use miss, not a steady-state penalty.
	   ★ Sbc.cpp stays ITCM-exempt above.  Arm (c) moved it and MEASURED 704 us/block against 125.6 --
	   5.6x, where NEW-36's icache_bench_hw measured 1.5x for the same transition on a ~3 KB working
	   set.  This build's flash working set does not fit the 32 KB I-cache.  It passed its acceptance
	   over 8.8 h (pcmdrops=0, 4.1x real time) and is declined on COST: 200 ms/s of CPU for 2,240 B
	   that the headroom above makes unnecessary.  That is NEW-37 option 1, answered.
	   ★ libLVGL stays in ITCM.  Arm (d) routed it to flash -- 212,400 B free -- and measured 27 fps
	   at 39,275 us with the LVGL slot at 95%% of wall time, WITHOUT the audio load every other arm
	   carried.  A lower bound that already misses 30 fps.  No `.fastrun` in any rule here. */
	*libMipiDisplay*.a:(.text*)
	*libWire*.a:(.text*)
	*libTouchPanel*.a:(.text*)")
    endif()

    string(REPLACE "*libLVGL_flash.a:(.text*)" "${_acidbt_routing}" _acidbt_ld "${_acidbt_ld}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/acid_box_itcm.ld" "${_acidbt_ld}")
    # LINK_DEPENDS: the derived script is generated by this configure step, not
    # referenced by any source file the generator already tracks, so without
    # this a routing-rule edit rewrites acid_box_itcm.ld but ninja/make sees no
    # reason to relink against it -- the stale binary would keep the OLD
    # routing until something else (a source edit, a clean build) forced a
    # relink.  Verified: "grep acid_box_itcm.ld build/CMakeFiles/acid_box.elf.dir/build.make"
    # finds it after this line, found nothing before.
    set_property(TARGET acid_box.elf APPEND PROPERTY
        LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/acid_box_itcm.ld")
    string(REPLACE "${_acidbt_coreld}"
                   "${CMAKE_CURRENT_BINARY_DIR}/acid_box_itcm.ld" _acidbt_lf "${_acidbt_lf}")
    # -Wl,--print-memory-usage is NOT re-added here: target_link_options() above
    # already puts it on this target's link line unconditionally, and LINK_FLAGS
    # is a SEPARATE property that the linker driver also sees -- appending it a
    # second time here printed the region table TWICE on every link (found in
    # review, 2026-09-16).  One flag, one property.
    set_target_properties(acid_box.elf PROPERTIES
        LINK_FLAGS "${_acidbt_lf}")
else()
    message(WARNING "acid_box: could not find the core imxrt1176.ld in LINK_FLAGS to derive the ITCM routing linker script; ITCM may overflow")
endif()

# SynthUI, LVGL and CMSIS-DSP are plain CMake static-lib targets (no `.o`
# suffix on the target name), so they link via raw target_link_libraries on the
# `.elf` target rather than the teensy_ wrapper -- see lvgl_rk055_panel_test for
# why.  LVGL is named directly, not only pulled in transitively via SynthUI's
# PUBLIC edge, because this target also compiles port/lvgl_mipi_panel.cpp itself.
target_link_libraries(acid_box.elf SynthUI LVGL VGLite CMSIS-DSP stdc++ m)

# GT911::read() is [[nodiscard]] and this tree carries no -Werror: without this
# a dropped poll result in the indev port warns and the build SUCCEEDS.  Scoped
# to this target, same rationale as lvgl_rk055_touch_test's identical line.
target_compile_options(acid_box.elf
                       PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-result>)

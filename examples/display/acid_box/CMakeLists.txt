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
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp
    ${M2_BT_OUT_SRCS})
# Audio's own dependency set (Wire/SPI/SdFat/SD/SerialFlash) is named exactly as
# acid_bass_test names it -- import_evkb_audio_full() creates those targets but
# does not link them.  Wire covers Wire2 = LPI2C5, shared by the codec and the
# touch controller, so TouchPanel needs no bus import of its own.
teensy_target_link_libraries(acid_box
    cores Audio Wire SPI SdFat SD SerialFlash MipiDisplay PXP TouchPanel)
if(M2_BT_OUT)
    teensy_target_link_libraries(acid_box M2Radio)
    target_include_directories(acid_box.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    # ITCM relief.  acid_box's ITCM sits at ~254K of the 256K window in the default
    # build (libLVGL alone is ~198K); the M.2 BT stack adds ~14K of .text and
    # overflows ITCM.  Derive a linker script from the core imxrt1176.ld that routes
    # libM2Radio's .text to FLASH (XIP) -- the SAME mechanism the core already uses
    # for libLVGL_flash.a in VGLite mode -- and point ONLY this target at it.  The BT
    # stack runs in setup()/loop(), not a tight audio/display ISR, so flash residency
    # is cheap -- I-cache-covered SINCE NEW-36 (2026-09-05; the core had NO I-cache
    # when this line was first written: the service chain measured 24.5 us/iter
    # then and 0.46 us/iter after, transcript_hw_evkb_bt.txt "NEW-36 I-CACHE A/B";
    # NEW-37 tracks whether this routing is still needed at all).  Derived at
    # configure time from the current core
    # script, so it never drifts; scoped to this M2_BT_OUT build -- no core change,
    # no effect on any other example that links M2Radio.
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
    # NEW-45: make the ITCM margin legible instead of only fatal.  Two halves:
    #   * --print-memory-usage prints region used/size/percent on EVERY link, so a shrinking margin shows up
    #     in ordinary build output rather than only at the cliff;
    #   * an ASSERT so the next NEW-41-sized library growth reports "ITCM headroom below 2048" instead of
    #     "region ITCM overflowed by 140 bytes".  ld cannot interpolate a number into an ASSERT message, so
    #     the literal is injected here at configure time along with the expression.
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
    # acid_box_bt.ld).  One convention that is 4 B loose beats two that disagree -- inventing a third way to
    # measure ITCM in the same file is how the two get out of step later.
    # Scoped to this target.  Putting either in the core imxrt1176.ld would change every example in the tree,
    # including ones that legitimately run close -- a separate change with its own blast radius.
    set(ACIDBOX_ITCM_MIN_HEADROOM "2048" CACHE STRING "Fail the M2_BT_OUT link when ITCM headroom drops below this many bytes.  2048 because measured headroom after the NEW-45 VGLite routing is ~9.8 KB: low enough that it never nags, high enough that a growth the size of the one that opened NEW-45 (+412 B from a library pin bump) reports a named headroom failure instead of a raw 'region ITCM overflowed' link error")
    if(_acidbt_coreld AND EXISTS "${_acidbt_coreld}")
        file(READ "${_acidbt_coreld}" _acidbt_ld)
        string(REPLACE
            "ASSERT(__text_csf_end <= 0x30FC0000, \"Image overlaps the EEPROM flash region (top 256K)\")"
            "ASSERT(__text_csf_end <= 0x30FC0000, \"Image overlaps the EEPROM flash region (top 256K)\")
	ASSERT(SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) <= LENGTH(ITCM) - ${ACIDBOX_ITCM_MIN_HEADROOM}, \"NEW-45: ITCM headroom below ${ACIDBOX_ITCM_MIN_HEADROOM} bytes -- see docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md\")"
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
                "M2_BT_OUT: the NEW-45 ITCM headroom ASSERT was NOT injected into the derived linker script. "
                "The EEPROM-assert line this rule anchors on has changed in the core script "
                "(${_acidbt_coreld}), so string(REPLACE) matched nothing and did nothing.  Left unchecked "
                "this build would link UNGUARDED and the next library growth would report a raw 'region ITCM "
                "overflowed by N bytes' instead of a named headroom failure.  Re-anchor the string(REPLACE) "
                "in examples/display/acid_box/CMakeLists.txt against that script's current ASSERT text.")
        endif()
        string(REPLACE "*libLVGL_flash.a:(.text*)"
                       "*libLVGL_flash.a:(.text*)
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
		*libTouchPanel*.a:(.text*)"
                       _acidbt_ld "${_acidbt_ld}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/acid_box_bt.ld" "${_acidbt_ld}")
        string(REPLACE "${_acidbt_coreld}"
                       "${CMAKE_CURRENT_BINARY_DIR}/acid_box_bt.ld" _acidbt_lf "${_acidbt_lf}")
        set_target_properties(acid_box.elf PROPERTIES
            LINK_FLAGS "${_acidbt_lf} -Wl,--print-memory-usage")
    else()
        message(WARNING "M2_BT_OUT: could not find the core imxrt1176.ld in LINK_FLAGS to derive the BT linker script; ITCM may overflow")
    endif()
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

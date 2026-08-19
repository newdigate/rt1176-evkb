#!/bin/sh
# license-audit.sh — prove no copyleft source is compiled into RT1176 firmware.
#
# Part 1: wrap-tolerant copyleft-header sweep over every ecosystem repo,
#         against a documented allowlist, PLUS a binary-provenance check
#         (grep cannot read binaries, so opaque blobs are checked structurally).
# Part 2: link-manifest audit — walk the CMake depfiles (*.obj.d) of EVERY
#         example that owns a run_qemu.sh (it began as three fat builds and
#         grew; the GATES drift check below now enforces the wider intent); every source AND header that fed any object must have a
#         permissive header. Dual-licensed allowlisted SOURCES must compile to
#         EMPTY objects (nm check) — the "preprocessor-dead" claim, enforced.
# Part 3: Ethernet/NativeEthernet shared files must be byte-identical.
#
# Exit 0 = LICENSE-AUDIT: PASS, nonzero otherwise. Run from anywhere.
set -u
TOOL=/Applications/ARM_10/bin
# EVKB / GATES / GATES_EXEMPT are overridable for the same reason REPOS and
# PARTS are: license-audit.test.sh drives the checks against throwaway trees so
# the negative tests need no gate builds and no network. Nothing in normal use
# sets them.
# ★ The default below names ONE specific checkout, not "wherever this script
# lives" — despite the "Run from anywhere" line above, which is true of the
# script's invocation but NOT of what it audits. A checkout under a different
# directory name (e.g. a parallel session clone such as
# rt1176-evkb-m2-maya-w161) is a DIFFERENT tree with its own build/ output and
# its own uncommitted state; running this script bare from inside it silently
# audits $HOME/Development/rt1170/evkb instead and reports that tree's gates
# as MISSING BUILD (they were never built there) while never seeing the real
# checkout's work at all. Same class of trap as the mon.sock AF_UNIX path
# length documented in CLAUDE.md: a hardcoded assumption about ONE checkout's
# location that a same-repo, different-directory clone silently violates.
# Always pass LICENSE_AUDIT_EVKB=$(pwd) (or the checkout's absolute path) when
# running from anywhere but the canonical clone.
EVKB=${LICENSE_AUDIT_EVKB:-$HOME/Development/rt1170/evkb}
# Sibling-checkout root — the same TEENSY_LIB_ROOT the build resolves against
# (evkb.cmake / teensy-cmake-macros). The audit must sweep the SAME trees the
# firmware compiles from, or it passes by measuring less: the core moved from
# $EVKB/cores to $LIB_ROOT/teensy-cores in the 2026-08-14 resolution change.
LIB_ROOT=${TEENSY_LIB_ROOT:-$HOME/Development}
fail=0

REPOS=${LICENSE_AUDIT_REPOS:-"$LIB_ROOT/teensy-cores $LIB_ROOT/Ethernet $LIB_ROOT/NativeEthernet \
$LIB_ROOT/SdFat $LIB_ROOT/SPI $LIB_ROOT/Wire \
$LIB_ROOT/Audio $LIB_ROOT/SD $LIB_ROOT/PaulS_SD \
$LIB_ROOT/USBHost_t36 $LIB_ROOT/FNET $LIB_ROOT/lwip \
$LIB_ROOT/CMSIS-DSP $LIB_ROOT/CMSIS_6 $LIB_ROOT/SerialFlash \
$LIB_ROOT/PXP $LIB_ROOT/MipiDisplay $LIB_ROOT/LVGL \
$LIB_ROOT/EEPROM $LIB_ROOT/ILI9341_t3 $LIB_ROOT/TouchPanel $LIB_ROOT/Bounce2 \
$LIB_ROOT/SynthUI $LIB_ROOT/VGLite $LIB_ROOT/M2Radio \
$LIB_ROOT/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin"}
# M2Radio joined 2026-08-17 with networking/m2_sdio_probe, the first example to
# link it. MIT, nothing vendored — its sdio/ is written against the RT1176
# USDHC register map, not copied from an SDK. It is in this list for the same
# reason every other entry is: the firmware compiles from it, so Part 1 must
# sweep it or the audit passes by measuring less.
# VGLite joined 2026-08-16 with display/vglite_probe, the first example to link
# it. It is the first VENDORED third-party driver in this list rather than a
# newdigate fork, so the sweep is doing more than coverage bookkeeping here:
# NXP/Vivante ship VGLite permissively (VENDORING.md records what was taken and
# from where), and Part 1 re-proves no copyleft on every run against the actual
# files.
# Nothing under tools/nxp-oracle/ is vendored SDK — it is a CMakeLists, a linker
# script and a README that point at an out-of-tree MCUXpresso install — so no
# NXP-EULA source enters this tree with it.
# ★ It is also this list's first NON-MIT entry, and the way that surfaced is
# worth keeping. VGLite/vg_lite_flat.{c,h} are Apache-2.0 (Raph Levien /
# Nicolas Silva / NXP 2022) and compile into firmware — vg_lite.c calls their
# Bézier flatteners from its stroke path. This audit passed throughout, exactly
# as designed: $COPYLEFT matches GPL/LGPL/MPL, and Apache-2.0 is none of those.
# So a green run means NO COPYLEFT, not "everything here is MIT". If you need
# the latter, survey per file:
#   for f in $(git ls-files '*.c' '*.h'); do \
#     printf '%-46s %s\n' "$f" "$(grep -m1 -o 'MIT License\|Apache License' "$f")"; done
# Do NOT add Apache-2.0 to $COPYLEFT to "cover" this — it is permissive, and
# making the copyleft check fail on permissive licences would destroy the
# meaning of the one signal this script exists to give.
# SynthUI joined 2026-08-16 with display/synthui_knob_test, the first example to
# link it. Same finding as the three below, caught before it fired: its widget
# sources enter the link manifest, so Part 2 would report them OUTSIDE SWEPT
# ROOTS. The repo is MIT throughout; its reference/ material is design reference
# that nothing compiles, so no source in a manifest comes from there.
# ILI9341_t3, TouchPanel and Bounce2 joined 2026-08-14: Part 2's REPOS-coverage
# check (below) found their sources in link manifests while Part 1 never swept
# them — 7, 3 and 2 dep paths respectively. Adding a repo here is always the
# answer to that finding; never exempt a source tree from the sweep.
# conn_fwloader/fw_bin joined 2026-08-19 with networking/m2_lwip_test's Part 2
# OUTSIDE SWEPT ROOTS finding (also latent, unfired, in m2_sdio_probe since
# 2026-08-17 — the depfile walk had simply never run against the right EVKB
# before now, see the note by EVKB's default above). Unlike every other entry
# here, this is NOT a permissively-licensed repo: it is NXP's IW416 firmware
# blob tree from the local MCUXpresso SDK workspace, under
# LA_OPT_NXP_Software_License (fw_bin/LICENSE.txt) — a redistributable BINARY
# licence, not an open-source one. It is deliberately NOT vendored into this
# repo (see m2_sdio_probe/CMakeLists.txt's "IW416 firmware blob (NOT
# vendored)" comment): the .bin.inc it compiles in comes from
# -DM2RADIO_IW416_FW=<path under here>, supplied only at configure time on a
# machine that has the SDK. The root is scoped to fw_bin itself, NOT its
# conn_fwloader parent, because fw_bin is where NXP's own LICENSE.txt sits and
# is also its own git repo (has a .git of its own) — the tightest root the
# binary-provenance check and the copyleft sweep can both reason about
# correctly; conn_fwloader's other sources (fsl_loader.c etc.) never feed any
# example's link manifest and are intentionally left unswept. Sweeping it is
# not vacuous: fw_bin tracks no .a/.o/.so/.dylib/.lib (binary-provenance finds
# nothing to demand licence text for), and the copyleft-header grep is clean
# except for SBOM.spdx.json, excluded below — it is SPDX metadata, not code,
# and its one match is boilerplate ("...such as the BSD License, Apache
# License or the GNU Lesser General Public License...") naming LGPL as an
# example of "an applicable open source license" in a generic disclaimer
# sentence, not a licence grant over anything here.

# Allowlist (extended regex), each entry justified:
#   cores/teensy/, cores/teensy3/
#                        — uncompiled PJRC reference copies inside the
#                          teensy-cores sibling repo, never in any build
#                          (audit part 2 proves nothing under them is compiled)
#   cores/teensy4/       — NOT a reference copy any more. Until the rt1062 board
#                          axis landed, this entry read "never in any build" for
#                          all of cores/teensy*, and that was true. EVKB_BOARD=
#                          rt1062 (evkb.cmake) now compiles this core for real,
#                          which turned five inherited LGPL sources into live
#                          firmware code — WString.cpp (102 symbols), Stream.cpp
#                          (11), WMath.cpp (8), IPAddress.cpp (4), Time.cpp (3).
#                          They were REPLACED, not excused, with the MIT
#                          clean-room versions already carried by
#                          cores/imxrt1176 (IPAddress.cpp from Ethernet), so
#                          this directory ships no copyleft source at all.
#                          Two things to know before trusting this entry:
#                          (a) part 2's EMPTY-object rule is what still
#                          backstops SOURCES here — any future copyleft .cpp
#                          that actually compiles fails the audit rather than
#                          riding this allowlist in;
#                          (b) that rule cannot see HEADERS, which define no
#                          symbols. So a copyleft header under this path is
#                          invisible to both checks. Printable.h and
#                          WCharacter.h were in the rt1062 link manifest
#                          (WCharacter.h is 13 inline functions — it genuinely
#                          emits code) and were replaced for that reason.
#                          Client.h and Server.h still carry LGPL text and are
#                          deliberately left: no link manifest includes them.
#                          Check the manifest, not this comment, before adding
#                          a header here.
#   SPI/SPI.{h,cpp}, Wire/{Wire.h,Wire.cpp}, Wire/utility/twi.{h,c}
#                        — dual-licensed upstream platform branches,
#                          preprocessor-dead under __IMXRT1176__ (documented in
#                          each repo's LICENSE.md); their objects are verified
#                          EMPTY in part 2, so the claim is self-enforcing.
#   LVGL/lvgl/src/libs/thorvg/tvgLottieInterpolator.cpp
#                        — MIT-headered ThorVG file carrying one MPL-2.0 snippet
#                          (the Firefox cubic-bezier solver) inside
#                          #if LV_USE_THORVG_INTERNAL, which is 0. It is NOT
#                          compiled: evkb.cmake globs lvgl/src/*.c only, and
#                          deliberately never *.cpp (all 47 thorvg files are
#                          .cpp). Being a .cpp on this allowlist, part 2 holds it
#                          to the EMPTY-object rule, so enabling thorvg would
#                          fail this audit rather than slip through.
# Entries are deliberately ROOT-INDEPENDENT (no Development/ prefix): they must
# match under $LIB_ROOT wherever it points, AND under this script's own tests'
# throwaway trees. Slightly wider than path-anchored — acceptable because ALLOW
# only applies inside REPOS directories and part 2's EMPTY-object rule
# independently backstops every entry (see each justification above).
#   ILI9341_t3/extras/bdf_to_ili9341.c
#                        — GPL-3.0 HOST-SIDE font-conversion utility (PJRC
#                          upstream, includes <stdio.h>, runs on a desktop —
#                          not firmware code). Found the day ILI9341_t3 joined
#                          REPOS via the Part-2 coverage check; Part 2's walk
#                          of every link manifest confirms nothing compiles it.
#                          Being a .c on this allowlist, the EMPTY-object rule
#                          fires if it ever IS compiled — self-enforcing, the
#                          same shape as thorvg above.
ALLOW='cores/teensy/|cores/teensy3/|cores/teensy4/|/SPI/SPI\.(h|cpp)$|/Wire/Wire\.(h|cpp)$|/Wire/utility/twi\.(h|c)$|/LVGL/lvgl/src/libs/thorvg/tvgLottieInterpolator\.cpp$|/ILI9341_t3/extras/bdf_to_ili9341\.c$'
# Between keywords, tolerate whitespace AND comment decoration (* / # ! -):
# a wrapped header line like "GNU\n * Lesser General Public\n * License"
# must still match. Plain [[:space:]]+ misses star-prefixed continuations —
# that exact gap was caught by this script's own negative test.
SEP='([[:space:]]|[*/#!-])+'
# STRONG copyleft: GNU GPL/LGPL — must never reach firmware.
# WEAK copyleft: MPL-2.0 — file-level, so it is arguably tolerable in a tree that
# does not compile it. That leniency is a DECISION, not a regex accident: MPL text
# FAILS this gate exactly like GPL, and the only way past it is an ALLOW entry
# with a written justification (see thorvg above). LVGL 9.4.0 shipped MPL-2.0
# code in src/libs/frogfs/ that the pre-MPL regex could not see; it was pruned by
# hand, and this gate now catches its like on re-vendor.
COPYLEFT="(GNU${SEP}(General|Lesser)${SEP}(General${SEP})?Public${SEP}License\
|Mozilla${SEP}Public${SEP}License)"

# --- binary provenance -------------------------------------------------------
# grep -I skips binary files entirely, so the header sweep above is structurally
# blind to prebuilt archives. Not hypothetical: LVGL 9.4.0 vendored 8 git-tracked
# .a files (2.6 MB) under lvgl/libs/nema_gfx/ with no licence text anywhere in
# that subtree, and they passed this audit invisibly.
#
# The rule enforced is "binaries without provenance", NOT "no binaries":
# lvgl/src/libs/freetype/LiberationSans-Regular.ttf is equally unreadable to
# grep, but ships LiberationSans-LICENSE.txt right beside it and is legitimately
# retained. So an object/library binary must have licence text in its OWN
# directory or ONE level up.
#
# Why not walk all the way to the repo root: lvgl/LICENCE.txt is an ancestor of
# lvgl/libs/nema_gfx/, so a root-to-leaf walk would have excused the very blobs
# that motivated this check. An ancestor licence describes the host project's own
# source; it says nothing about which licence covers an opaque third-party blob
# vendored beneath it. Provenance for a binary has to be adjacent to the binary.
#
# Tracked files only (git ls-files): build outputs are untracked by design, and
# what ships in a checkout is what matters.
BINARY_EXT='\.(a|o|so|dylib|lib)$'
# Binary allowlist (extended regex, matched against the repo-relative path),
# each entry to be justified here like ALLOW above. Empty today: no swept repo
# tracks a single object/library binary, and any future one needs either adjacent
# licence text or a justification written right here. '^$' matches nothing.
BIN_ALLOW='^$'
# What counts as licence text sitting in a directory. Deliberately excludes
# COPYRIGHT*/NOTICE*: lvgl/COPYRIGHTS.md is an attribution index, not a grant.
LICTEXT='(licen[cs]e|copying)[^/]*$'

# Test hooks, used only by license-audit.test.sh (see LICENSE_AUDIT_REPOS above):
# restrict which parts run, so a negative test can exercise part 1 against a
# throwaway repo without needing the fat gate builds part 2 walks.
PARTS=${LICENSE_AUDIT_PARTS:-123}
SHARED="Client.h Server.h IPAddress.h IPAddress.cpp"
# Snapshot BEFORE the part-1 clearing below: Part 2's REPOS-coverage check
# needs the swept-root list even in a PARTS=2 run (the clearing only exists so
# a parts-restricted run skips part 1's sweep loop, not to change what counts
# as a swept root).
SWEEP_ROOTS="$REPOS"
case "$PARTS" in *1*) ;; *) REPOS="" ;; esac

echo "== Part 1: repo copyleft-header sweep"
for r in $REPOS; do
  [ -d "$r" ] || continue
  # SBOM.spdx.json (conn_fwloader/fw_bin, joined 2026-08-19): an SPDX metadata
  # document, not code. Its one COPYLEFT match is a generic disclaimer
  # sentence naming LGPL as an example of "an applicable open source
  # license", not a grant over anything in this tree — same false-positive
  # class as the LICENSE*/COPYING*/*.md excludes already here, just a name
  # those globs don't happen to cover.
  hits=$(grep -rIlz --exclude-dir=.git --exclude='*.img' --exclude='LICENSE*' \
         --exclude='COPYING*' --exclude='*.md' --exclude='SBOM.spdx.json' \
         -E "$COPYLEFT" "$r" 2>/dev/null \
         | tr '\0' '\n' | grep -vE "$ALLOW" || true)
  if [ -n "$hits" ]; then
    echo "COPYLEFT header, not allowlisted:"
    echo "$hits"
    fail=1
  fi

  # Binary provenance (see BINARY_EXT above). Needs git to tell tracked source
  # from build output; a non-repo directory is reported, never silently skipped.
  if ! git -C "$r" rev-parse --git-dir >/dev/null 2>&1; then
    echo "  note: $r is not a git repo — binary-provenance check not applicable"
    continue
  fi
  tracked=$(git -C "$r" ls-files 2>/dev/null || true)
  bins=$(printf '%s\n' "$tracked" | grep -iE "$BINARY_EXT" | grep -vE "$BIN_ALLOW" || true)
  [ -n "$bins" ] || continue
  # Only computed when the repo actually tracks binaries, so the common case
  # costs one `git ls-files` per repo and nothing else.
  licdirs=$(printf '%s\n' "$tracked" | grep -iE "$LICTEXT" \
            | while IFS= read -r p; do dirname "$p"; done | sort -u)
  while IFS= read -r b; do
    [ -n "$b" ] || continue
    own=$(dirname "$b"); up=$(dirname "$own")
    ok=0
    for cand in "$own" "$up"; do
      # Newline-delimited containment: plain string equality, so a path
      # containing regex metacharacters cannot skew the match.
      case "
$licdirs
" in *"
$cand
"*) ok=1 ;;
      esac
    done
    if [ $ok -eq 0 ]; then
      echo "UNLICENSED BINARY (no licence text in its dir or one level up): $r/$b"
      fail=1
    fi
  done <<EOF
$bins
EOF
done

echo "== Part 2: link-manifest audit (depfile walk)"
# gate_dir:elf_target pairs — the union covers cores+SPI+Wire+Audio+SdFat+SD
# (sd_wav), Ethernet+lwip (ethernet), NativeEthernet+FNET (native_ethernet),
# and the dual-core library Multicore+MessagingUnit (cm4_boot, cm4_image).
# CM4 sub-images (cm4/*.S/.c) are built by the teensy_add_cm4_image macro
# (teensy-cmake-macros), whose gcc step emits <obj>.o.d depfiles (-MMD -MF,
# added 2026-07-18) — so CM4-side sources are covered by this same walk
# (the *.o.d pattern below), not just their provenance headers.
# An entry may also name a build directory directly (…/build-rt1062) to audit a
# second board's build of the same example: EVKB_BOARD=rt1062 links
# cores/teensy4 instead of cores/imxrt1176, so that image has a link manifest
# this list would otherwise never walk.
GATES=${LICENSE_AUDIT_GATES:-"examples/audio/acid_bass_test:acid_bass_test \
examples/audio/audio_h_test:audio_h_test examples/audio/audioinput_i2s_test:audioinput_i2s_test \
examples/audio/audioinput_i2s_test/build-rt1062:audioinput_i2s_test \
examples/audio/audiooutput_i2s_test:audiooutput_i2s_test \
examples/audio/audiooutput_i2s_test/build-rt1062:audiooutput_i2s_test \
examples/audio/audiostream_test:audiostream_test examples/audio/filter_fir_test:filter_fir_test \
examples/audio/guard_sweep_test:guard_sweep_test examples/audio/i2s_audio_test:i2s_audio_test \
examples/audio/i2s_int_test:i2s_int_test examples/audio/sai_rx_test:sai_rx_test \
examples/audio/sd_wav_play_test:sd_wav_play_test \
examples/audio/step_seq_test:step_seq_test examples/audio/tone_test:tone_test \
examples/audio/transport_test:transport_test \
examples/display/camera_preview_synth:camera_preview_synth \
examples/display/lvgl_ili9341_test:lvgl_ili9341_test \
examples/display/lvgl_pxp_copy_bench:lvgl_pxp_copy_bench \
examples/display/lvgl_rk055_flip_test:lvgl_rk055_flip_test \
examples/display/lvgl_rk055_panel_test:lvgl_rk055_panel_test \
examples/display/lvgl_rk055_touch_test:lvgl_rk055_touch_test \
examples/display/lvgl_rpi_panel_test:lvgl_rpi_panel_test \
examples/display/pxp_composite_test:pxp_composite_test \
examples/display/pxp_draw_bench:pxp_draw_bench \
examples/display/lvgl_smoke_test:lvgl_smoke_test examples/display/pxp_blit_test:pxp_blit_test \
examples/display/pxp_decimate_test:pxp_decimate_test examples/display/pxp_yuv_test:pxp_yuv_test \
examples/display/rk055_panel_test:rk055_panel_test \
examples/display/rk055_touch_test:rk055_touch_test \
examples/display/rpi_panel_test:rpi_panel_test \
examples/display/synthui_knob_test:synthui_knob_test \
examples/display/vglite_probe:vglite_probe \
examples/dualcore/cm4_audio_test:cm4_audio_test \
examples/dualcore/cm4_audiostream_test:cm4_audiostream_test \
examples/dualcore/cm4_boot_test:cm4_boot_test examples/dualcore/cm4_cpp_test:cm4_cpp_test \
examples/dualcore/cm4_dual_test:cm4_dual_test examples/dualcore/cm4_fft_test:cm4_fft_test \
examples/dualcore/cm4_graph_usb_capstone:cm4_graph_usb_capstone \
examples/dualcore/cm4_hotswap2_test:cm4_hotswap2_test \
examples/dualcore/cm4_hotswap_test:cm4_hotswap_test examples/dualcore/cm4_image_test:cm4_image_test \
examples/dualcore/cm4_imagebank_test:cm4_imagebank_test \
examples/dualcore/cm4_intr_test:cm4_intr_test examples/dualcore/cm4_sai_irq_probe:cm4_sai_irq_probe \
examples/dualcore/cm4_spi_dma_test:cm4_spi_dma_test examples/dualcore/cm4_spi_test:cm4_spi_test \
examples/dualcore/cm4_usb_audio_probe:cm4_usb_audio_probe \
examples/dualcore/cm4_usb_enum_probe:cm4_usb_enum_probe \
examples/dualcore/cm4_usb_irq_probe:cm4_usb_irq_probe \
examples/dualcore/cm4_wire_dma_test:cm4_wire_dma_test \
examples/dualcore/cm4_wire_int_master_test:cm4_wire_int_master_test \
examples/dualcore/cm4_wire_int_slave_test:cm4_wire_int_slave_test \
examples/dualcore/cm4_wire_test:cm4_wire_test examples/framework/arm_math_test:arm_math_test \
examples/framework/edma_test:edma_test examples/framework/eventresponder_test:eventresponder_test \
examples/framework/stream_test:stream_test examples/framework/string_test:string_test \
examples/framework/string_test/build-rt1062:string_test \
examples/framework/wprogram_parity_test:wprogram_parity_test \
examples/gpio-analog/analog_test:analog_test examples/gpio-analog/dac_test:dac_test \
examples/gpio-analog/irq_attach_test:irq_attach_test examples/networking/enet_test:enet_test \
examples/networking/ethernet_test:ethernet_test examples/networking/lwip_test:lwip_test \
examples/networking/m2_lwip_test:m2_lwip_test \
examples/networking/m2_sdio_probe:m2_sdio_probe \
examples/networking/m2_throughput_test:m2_throughput_test \
examples/networking/native_ethernet_test:native_ethernet_test \
examples/serial/serial_test:serial_test examples/serial/serial_test_rx:serial_test_rx \
examples/serial/serial_test/build-rt1062:serial_test \
examples/storage-memory/eeprom_test:eeprom_test examples/storage-memory/extmem_test:extmem_test \
examples/storage-memory/sdram_test:sdram_test \
examples/timing/interval_timer_test:interval_timer_test examples/timing/rtc_test:rtc_test \
examples/usb/usb_audio_capstone_test/build-rt1062:usb_audio_capstone_test \
examples/usb/usb_audio_capture_test:usb_audio_capture_test \
examples/usb/usb_audio_duplex_test:usb_audio_duplex_test \
examples/usb/usb_audio_uac1_test:usb_audio_uac1_test \
examples/usb/usb_audio_uac1_test/build-rt1062:usb_audio_uac1_test \
examples/usb/usb_descriptor_survey:usb_descriptor_survey \
examples/usb/usb_descriptor_survey/build-rt1062:usb_descriptor_survey \
examples/usb/usb_data_test:usb_data_test examples/usb/usb_enum_test:usb_enum_test \
examples/usb/usb_host_hid_test:usb_host_hid_test examples/usb/usb_joystick_test:usb_joystick_test \
examples/usb/usb_keyboard_test:usb_keyboard_test examples/usb/usb_midi_test:usb_midi_test \
examples/usb/usb_mouse_test:usb_mouse_test examples/usb/usb_msc_block_test:usb_msc_block_test \
examples/usb/usb_msc_fs_test:usb_msc_fs_test"}

# --- GATES drift check -------------------------------------------------------
# GATES is hand-maintained, and an example silently missing from it means Part 2
# never walks its depfiles. Those files still get Part 1's header sweep, so they
# are not unaudited — but the check that proves what actually reaches the
# firmware image skips them, which is the stronger of the two.
#
# This is not hypothetical. rk055_panel_test and lvgl_rpi_panel_test were both
# absent until 2026-07-28, and lvgl_smoke_test — which links LVGL, the most
# licence-sensitive dependency in this tree (it is where the MPL-2.0 frogfs code
# and the eight unlicensed nema_gfx archives came from) — was still absent after
# that gap had supposedly been closed. A hand-maintained list drifts every time
# an example lands.
#
# So the omission is made LOUD instead of silent: every example owning a gate
# must appear in GATES, with the target name matching its project(). An example
# that genuinely should be excluded goes in GATES_EXEMPT with a written
# justification — the same discipline the ALLOW list uses — never a quiet
# deletion.
#
# The glob is run_qemu*.sh, NOT run_qemu.sh (widened 2026-07-29). Roughly half
# the gates in this tree are named for what they test (run_qemu_usb.sh,
# run_qemu_lwip.sh, …), and while this check globbed the bare name it policed 33
# of the then-67 gate-owning examples while claiming to police all of them — the
# drift check had the same blind spot as the thing it was guarding against.
# Closing it added 34 entries below.
#
# Counts here are a snapshot and go stale as examples land — GATES itself is the
# authority, and the drift check is what keeps it honest. Reconciled 2026-07-29:
# 68 gate-owning directories, 68 GATES entries, no drift in either direction.
#
# Scope is examples that own a GATE (68 as of 2026-07-29), never all 84
# examples: Part 2 hard-errors on MISSING BUILD, so this requires those built
# before the
# audit passes. That cost is deliberate and it is the point — a depfile walk
# that silently skips an unbuilt example proves nothing about it, and this tree
# treats a check that quietly does not run as a defect, not a convenience.
# Building only some examples is what `LICENSE_AUDIT_PARTS=13` is for.
GATES_EXEMPT=${LICENSE_AUDIT_GATES_EXEMPT:-""}   # "examples/<cat>/<name>" + why — none today
case "$PARTS" in *2*)
  drift=0
  for gsh in "$EVKB"/examples/*/*/run_qemu*.sh; do
    [ -f "$gsh" ] || continue
    gdir=${gsh%/*}; rel=${gdir#$EVKB/}; name=${rel##*/}
    case " $GATES_EXEMPT " in *" $rel "*) continue ;; esac
    # An entry may satisfy this as "<rel>:" OR as "<rel>/build*:" — a gate-owning
    # example that supports only a NON-default board has no plain build/ and so
    # contributes its build-directory entry alone. usb/usb_audio_capstone_test
    # (rt1062-only, Phase 5b) is the first of those; before it, every two-board
    # example also built for rt1176, so a plain entry always existed and the
    # "<rel>:" test was sufficient by accident. Without the second pattern the
    # check FALSE-POSITIVES on a correctly-listed example, and the only ways out
    # are deleting a real entry or writing a bogus GATES_EXEMPT — pressure to
    # weaken the audit, from the check meant to keep it honest.
    case "$GATES" in
      *"$rel:"*|*"$rel/build"*)
        # listed — also check the target name matches the CMake project()
        proj=$(sed -n 's/^project(\([A-Za-z0-9_.-]*\).*/\1/p' "$gdir/CMakeLists.txt" 2>/dev/null | head -1)
        listed=$(printf '%s\n' $GATES | grep -E "^$rel(/build[^:]*)?:" | head -1); listed=${listed##*:}
        if [ -n "$proj" ] && [ "$proj" != "$listed" ]; then
          echo "GATES MISMATCH: $rel listed as target '$listed' but project() says '$proj'"
          drift=1
        fi
        ;;
      *)
        echo "GATES DRIFT: $rel has a run_qemu.sh but is missing from the Part-2 GATES list"
        echo "  -> add \"$rel:$name\", or add it to GATES_EXEMPT with a written reason"
        drift=1
        ;;
    esac
  done
  [ "$drift" -eq 0 ] || fail=1
esac

case "$PARTS" in *2*) ;; *) GATES="" ;; esac
for pair in $GATES; do
  g=${pair%%:*}; t=${pair##*:}
  bdir=$EVKB/$g/build
  # A gate path may name a build directory OUTRIGHT rather than an example
  # directory (last component starting "build"), which is how a second board's
  # build of the SAME example gets its own entry — one example directory, two
  # link manifests, and the rt1062 one links a different core. No example is
  # named build*, so the suffix test is unambiguous.
  case "${g##*/}" in build*) bdir=$EVKB/$g ;; esac
  if [ ! -f "$bdir/$t.elf" ]; then
    echo "MISSING BUILD: $g (build it first)"; fail=1; continue
  fi
  files=$(find "$bdir" \( -name '*.obj.d' -o -name '*.o.d' \) -exec cat {} + 2>/dev/null \
          | tr ' \\' '\n\n' | grep '^/' | grep -v ':$' | sort -u)
  n=$(printf '%s\n' $files | grep -c '^/' || true)
  # Project files only: GCC + newlib headers are GPL with the GCC Runtime
  # Library Exception / BSD — linking into firmware permitted.
  project=$(printf '%s\n' $files | grep -v '^/Applications/ARM_10/' || true)
  # REPOS coverage: every project dep path must lie under a swept root — $EVKB
  # or a REPOS entry. Part 1 sweeps only what REPOS names, and its
  # `[ -d ] || continue` skips a missing repo silently, so firmware that
  # compiled sources from a tree REPOS does not name would otherwise pass while
  # the audit never looked at those files. Same discipline as the GATES drift
  # check: the omission is made LOUD. Found real gaps on day one — ILI9341_t3,
  # TouchPanel and Bounce2 fed firmware unswept (see the REPOS comment above).
  # Existence is deliberately not required: the depfile RECORD is the evidence
  # that the path fed a compile, whether or not the file is still there.
  outside=$(printf '%s\n' $project | awk -v evkb="$EVKB" -v repos="$SWEEP_ROOTS" '
      BEGIN { n = split(repos, r, /[ \t\n]+/) }
      {
        if (index($0, evkb "/") == 1) next
        for (i = 1; i <= n; i++) if (r[i] != "" && index($0, r[i] "/") == 1) next
        print
      }' | head -5)
  if [ -n "$outside" ]; then
    echo "OUTSIDE SWEPT ROOTS in $g (Part 1 never sweeps these; add the repo to REPOS):"
    printf '%s\n' "$outside" | sed 's/^/  /'
    fail=1
  fi
  checked=0
  for f in $project; do [ -f "$f" ] && checked=$((checked + 1)); done
  # ONE batched grep instead of a head|tr|grep pipeline per file. The LVGL gate
  # pulls ~24k dep paths, where per-file pipelines take minutes and an audit
  # nobody runs protects nothing. -lz matches Part 1's proven flags: -z makes
  # each file a single record, so the COPYLEFT separator class (which includes
  # newlines) still spans headers wrapped across lines, as tr '\n' ' ' did.
  # Note this now scans whole files rather than the first 6000 bytes — strictly
  # more thorough, never less.
  candidates=$(printf '%s\n' $project | tr '\n' '\0' \
               | xargs -0 grep -lzIE "$COPYLEFT" 2>/dev/null || true)
  for f in $candidates; do
    if [ -f "$f" ]; then
      if echo "$f" | grep -qE "$ALLOW"; then
        case "$f" in
          *.c|*.cpp)
            # dual-licensed source: its object must define NO symbols
            base=$(basename "$f").obj
            syms=""
            for a in "$bdir"/lib*.a; do
              [ -f "$a" ] || continue
              if "$TOOL/arm-none-eabi-ar" t "$a" 2>/dev/null | grep -qx "$base"; then
                # ANCHORED (index == 1). nm prints an archive member header at
                # the START of a line, so a substring match also fires on any
                # member whose name merely ENDS with this one: base=Stream.cpp
                # .obj matched AudioStream.cpp.obj: too, and — because the
                # match arm runs before the /:$/ reset — swallowed the real
                # header as well, attributing both members' symbols to Stream
                # .cpp. Measured on serial_test/build-rt1062/libcores.o.a:
                # 47 symbols unanchored, 12 anchored, the 35 extra all
                # AudioStream's. That mis-prints evidence (you go debug the
                # wrong file) and can flag a genuinely-empty ALLOW-listed file
                # because some <Prefix><Name>.cpp in the same archive is not.
                syms=$("$TOOL/arm-none-eabi-nm" --defined-only "$a" 2>/dev/null \
                  | awk -v m="$base:" 'index($0, m) == 1 {inm=1; next} /:$/ {inm=0} inm && NF {print}')
              fi
            done
            if [ -n "$syms" ]; then
              echo "DUAL-LICENSED SOURCE NOT EMPTY in $g: $f"
              echo "$syms" | head -5
              fail=1
            fi ;;
        esac
      else
        echo "COPYLEFT FILE COMPILED into $g: $f"
        fail=1
      fi
    fi
  done
  echo "  $g: $n dep paths, $checked project files checked"
  if [ "$checked" -lt 100 ]; then
    echo "  SUSPICIOUS: too few files checked in $g (depfiles missing?)"; fail=1
  fi
done

echo "== Part 3: Ethernet/NativeEthernet byte-identical shared files"
case "$PARTS" in *3*) ;; *) SHARED="" ;; esac
for f in $SHARED; do
  if ! cmp -s "$HOME/Development/Ethernet/src/$f" "$HOME/Development/NativeEthernet/src/$f"; then
    echo "DRIFT: src/$f differs between Ethernet and NativeEthernet"; fail=1
  fi
done

if [ $fail -eq 0 ]; then
  echo "LICENSE-AUDIT: PASS"
else
  echo "LICENSE-AUDIT: FAIL"
fi
exit $fail

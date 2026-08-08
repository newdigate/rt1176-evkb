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
EVKB=${LICENSE_AUDIT_EVKB:-$HOME/Development/rt1170/evkb}
fail=0

REPOS=${LICENSE_AUDIT_REPOS:-"$EVKB/cores $HOME/Development/Ethernet $HOME/Development/NativeEthernet \
$HOME/Development/SdFat $HOME/Development/SPI $HOME/Development/Wire \
$HOME/Development/Audio $HOME/Development/SD $HOME/Development/PaulS_SD \
$HOME/Development/USBHost_t36 $HOME/Development/FNET $HOME/Development/lwip \
$HOME/Development/CMSIS-DSP $HOME/Development/CMSIS_6 $HOME/Development/SerialFlash \
$HOME/Development/PXP $HOME/Development/MipiDisplay $HOME/Development/LVGL \
$HOME/Development/EEPROM"}

# Allowlist (extended regex), each entry justified:
#   cores/teensy/, cores/teensy3/
#                        — uncompiled PJRC reference copies, never in any build
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
ALLOW='cores/teensy/|cores/teensy3/|cores/teensy4/|Development/SPI/SPI\.(h|cpp)$|Development/Wire/Wire\.(h|cpp)$|Development/Wire/utility/twi\.(h|c)$|Development/LVGL/lvgl/src/libs/thorvg/tvgLottieInterpolator\.cpp$'
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
case "$PARTS" in *1*) ;; *) REPOS="" ;; esac

echo "== Part 1: repo copyleft-header sweep"
for r in $REPOS; do
  [ -d "$r" ] || continue
  hits=$(grep -rIlz --exclude-dir=.git --exclude='*.img' --exclude='LICENSE*' \
         --exclude='COPYING*' --exclude='*.md' -E "$COPYLEFT" "$r" 2>/dev/null \
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
GATES=${LICENSE_AUDIT_GATES:-"examples/audio/audio_h_test:audio_h_test examples/audio/audioinput_i2s_test:audioinput_i2s_test \
examples/audio/audiooutput_i2s_test:audiooutput_i2s_test \
examples/audio/audiostream_test:audiostream_test examples/audio/filter_fir_test:filter_fir_test \
examples/audio/guard_sweep_test:guard_sweep_test examples/audio/i2s_audio_test:i2s_audio_test \
examples/audio/i2s_int_test:i2s_int_test examples/audio/sai_rx_test:sai_rx_test \
examples/audio/sd_wav_play_test:sd_wav_play_test examples/audio/tone_test:tone_test \
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
examples/networking/native_ethernet_test:native_ethernet_test \
examples/serial/serial_test:serial_test examples/serial/serial_test_rx:serial_test_rx \
examples/serial/serial_test/build-rt1062:serial_test \
examples/storage-memory/eeprom_test:eeprom_test examples/storage-memory/extmem_test:extmem_test \
examples/storage-memory/sdram_test:sdram_test \
examples/timing/interval_timer_test:interval_timer_test examples/timing/rtc_test:rtc_test \
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
    case "$GATES" in
      *"$rel:"*)
        # listed — also check the target name matches the CMake project()
        proj=$(sed -n 's/^project(\([A-Za-z0-9_.-]*\).*/\1/p' "$gdir/CMakeLists.txt" 2>/dev/null | head -1)
        listed=$(printf '%s\n' $GATES | grep "^$rel:" | head -1); listed=${listed##*:}
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

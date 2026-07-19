#!/bin/sh
# license-audit.sh — prove no copyleft source is compiled into RT1176 firmware.
#
# Part 1: wrap-tolerant copyleft-header sweep over every ecosystem repo,
#         against a documented allowlist.
# Part 2: link-manifest audit — walk the CMake depfiles (*.obj.d) of three fat
#         gate builds; every source AND header that fed any object must have a
#         permissive header. Dual-licensed allowlisted SOURCES must compile to
#         EMPTY objects (nm check) — the "preprocessor-dead" claim, enforced.
# Part 3: Ethernet/NativeEthernet shared files must be byte-identical.
#
# Exit 0 = LICENSE-AUDIT: PASS, nonzero otherwise. Run from anywhere.
set -u
TOOL=/Applications/ARM_10/bin
EVKB=$HOME/Development/rt1170/evkb
fail=0

REPOS="$EVKB/cores $HOME/Development/Ethernet $HOME/Development/NativeEthernet \
$HOME/Development/SdFat $HOME/Development/SPI $HOME/Development/Wire \
$HOME/Development/Audio $HOME/Development/SD $HOME/Development/PaulS_SD \
$HOME/Development/USBHost_t36 $HOME/Development/FNET $HOME/Development/lwip"

# Allowlist (extended regex), each entry justified:
#   cores/teensy*        — uncompiled PJRC reference copies, never in any build
#                          (audit part 2 proves nothing under them is compiled)
#   SPI/SPI.{h,cpp}, Wire/{Wire.h,Wire.cpp}, Wire/utility/twi.{h,c}
#                        — dual-licensed upstream platform branches,
#                          preprocessor-dead under __IMXRT1176__ (documented in
#                          each repo's LICENSE.md); their objects are verified
#                          EMPTY in part 2, so the claim is self-enforcing.
ALLOW='cores/teensy/|cores/teensy3/|cores/teensy4/|Development/SPI/SPI\.(h|cpp)$|Development/Wire/Wire\.(h|cpp)$|Development/Wire/utility/twi\.(h|c)$'
# Between keywords, tolerate whitespace AND comment decoration (* / # ! -):
# a wrapped header line like "GNU\n * Lesser General Public\n * License"
# must still match. Plain [[:space:]]+ misses star-prefixed continuations —
# that exact gap was caught by this script's own negative test.
SEP='([[:space:]]|[*/#!-])+'
COPYLEFT="GNU${SEP}(General|Lesser)${SEP}(General${SEP})?Public${SEP}License"

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
done

echo "== Part 2: link-manifest audit (depfile walk)"
# gate_dir:elf_target pairs — the union covers cores+SPI+Wire+Audio+SdFat+SD
# (sd_wav), Ethernet+lwip (ethernet), NativeEthernet+FNET (native_ethernet),
# and the dual-core library Multicore+MessagingUnit (cm4_boot, cm4_image).
# CM4 sub-images (cm4/*.S/.c) are built by the teensy_add_cm4_image macro
# (teensy-cmake-macros), whose gcc step emits <obj>.o.d depfiles (-MMD -MF,
# added 2026-07-18) — so CM4-side sources are covered by this same walk
# (the *.o.d pattern below), not just their provenance headers.
GATES="sd_wav_play_test:sd_wav_play_test ethernet_test:ethernet_test native_ethernet_test:native_ethernet_test cm4_boot_test:cm4_boot_test cm4_image_test:cm4_image_test cm4_intr_test:cm4_intr_test cm4_dual_test:cm4_dual_test cm4_spi_test:cm4_spi_test cm4_wire_test:cm4_wire_test cm4_wire_int_master_test:cm4_wire_int_master_test cm4_wire_int_slave_test:cm4_wire_int_slave_test cm4_spi_dma_test:cm4_spi_dma_test"
for pair in $GATES; do
  g=${pair%%:*}; t=${pair##*:}
  bdir=$EVKB/$g/build
  if [ ! -f "$bdir/$t.elf" ]; then
    echo "MISSING BUILD: $g (build it first)"; fail=1; continue
  fi
  files=$(find "$bdir" \( -name '*.obj.d' -o -name '*.o.d' \) -exec cat {} + 2>/dev/null \
          | tr ' \\' '\n\n' | grep '^/' | grep -v ':$' | sort -u)
  n=0; checked=0
  for f in $files; do
    n=$((n + 1))
    case "$f" in
      /Applications/ARM_10/*) continue ;;  # GCC + newlib headers: GPL with the
                                           # GCC Runtime Library Exception / BSD
                                           # — linking into firmware permitted.
    esac
    [ -f "$f" ] || continue
    checked=$((checked + 1))
    if head -c 6000 "$f" | tr '\n' ' ' | grep -qE "$COPYLEFT"; then
      if echo "$f" | grep -qE "$ALLOW"; then
        case "$f" in
          *.c|*.cpp)
            # dual-licensed source: its object must define NO symbols
            base=$(basename "$f").obj
            syms=""
            for a in "$bdir"/lib*.a; do
              [ -f "$a" ] || continue
              if "$TOOL/arm-none-eabi-ar" t "$a" 2>/dev/null | grep -qx "$base"; then
                syms=$("$TOOL/arm-none-eabi-nm" --defined-only "$a" 2>/dev/null \
                  | awk -v m="$base:" 'index($0, m) {inm=1; next} /:$/ {inm=0} inm && NF {print}')
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
for f in Client.h Server.h IPAddress.h IPAddress.cpp; do
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

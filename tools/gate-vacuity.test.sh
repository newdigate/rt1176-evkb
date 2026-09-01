#!/bin/sh
# gate-vacuity.test.sh — proves the gate runners FAIL when they should.
#
# Companion to license-audit.test.sh, and written for the same reason: a check
# that cannot be shown to fire is not a check. These cases pin the two defects
# swept out of all 68 runners on 2026-07-29, both of which had the property that
# the gate looked fine while proving nothing — plus a third class (case 3) that
# arrived with the first gate asserting a device WORKS rather than is absent.
#
#   1. A dead QEMU must fail BY NAME. `kill $P` under `set -e` fails when QEMU
#      has already exited, errexit fires, and the gate dies before any assertion
#      runs -- exit 1 with no message. gate_reap's `|| true` plus
#      gate_require_capture turn that into a named failure.
#   2. A missing token is not proof of the good outcome. The cm4 wire gates
#      asserted "the CM4 took its IRQ" with `grep -q "^irqcnt=00000000" && fail`,
#      which scores a MISSING irqcnt as "not zero, therefore it fired" -- the
#      assertion passed most confidently in the one case it knew nothing about.
#
# Method: run REAL gate scripts against a fake qemu injected through tools/qrun's
# REAL_QEMU hook, so the run's output is whatever we choose. Fixtures are the
# gates' own committed transcript_qemu.txt -- version-controlled, so this test
# needs neither a prior gate run nor a working QEMU. It DOES need each covered
# example built, because the gates check for their ELF.
#
# Usage: sh tools/gate-vacuity.test.sh   (PASS:/FAIL: per case; exit 1 on any FAIL)
# Runtime ~40s: the dead-QEMU cases necessarily burn each gate's full poll
# ceiling, because "nothing ever arrives" is exactly what they are testing.
set -u
EVKB=$(cd "$(dirname "$0")/.." && pwd)
FAILED=0
report() { # <name> <0-pass|1-fail>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; else echo "FAIL: $1"; FAILED=1; fi
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/gate-vacuity.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

# Stand-in for qemu-system-arm. Writes $FAKE_CAPTURE to the `-serial file:`
# target if set, then LINGERS so the gate's reap has a live child; with it unset
# it produces nothing and exits at once, which is the dead-QEMU case.
#
# The linger is load-bearing for case 2: without it defect 1 aborts the gate
# before any assertion runs, masking defect 2 entirely. That interaction is why
# the vacuous assertions survived so long -- the failure that would have exposed
# them killed the script first.
cat > "$WORK/fake-qemu" <<'FAKE'
#!/bin/sh
target=""; prev=""
for a in "$@"; do
    case "$a" in file:*) [ "$prev" = "-serial" ] && target="${a#file:}" ;; esac
    prev="$a"
done
if [ -n "${FAKE_CAPTURE:-}" ]; then
    [ -n "$target" ] && cat "$FAKE_CAPTURE" > "$target"
    sleep 300      # qrun's gtimeout and the gate's own reap both bound this
fi
exit 0
FAKE
chmod +x "$WORK/fake-qemu"

# run_gate <example-rel-path> <gate-filename> [capture-fixture] -> rc, output in $OUT_TEXT
run_gate() {
    _dir="$EVKB/$1"; _gate="$2"; _cap="${3:-}"
    OUT_TEXT=$( cd "$_dir" && REAL_QEMU="$WORK/fake-qemu" FAKE_CAPTURE="$_cap" \
                GATE_TIMEOUT=120 QRUN_TIMEOUT=40 "./$_gate" 2>&1 )
    return $?
}

# --- 1. a run that produced nothing must fail, and SAY SO ------------------
# Three different runner shapes so this is not pinned to one gate's phrasing.
# Asserting the MESSAGE, not just a non-zero exit, is the point: without
# gate_require_capture these still exit non-zero, but blame the firmware
# ("banner missing") for a run that never happened.
for spec in "examples/serial/serial_test:run_qemu.sh" \
            "examples/dualcore/cm4_boot_test:run_qemu.sh" \
            "examples/display/rk055_panel_test:run_qemu.sh"; do
    rel=${spec%%:*}; gate=${spec##*:}
    name="dead_qemu_named_${rel##*/}"
    if [ ! -d "$EVKB/$rel" ]; then echo "SKIP: $name (no such example)"; continue; fi
    run_gate "$rel" "$gate"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                   # must not pass
    echo "$OUT_TEXT" | grep -q "FAIL: no UART capture" || result=1 # must name it
    report "$name" $result
done

# --- 2. a missing counter token must not read as "the IRQ fired" -----------
# Fixture is the gate's own committed transcript with the counter line deleted:
# a firmware that stopped reporting it, and for cm4_wire_dma_test precisely the
# RED-scaffold token timeout its own header describes.
for spec in "examples/dualcore/cm4_wire_int_master_test:irqcnt" \
            "examples/dualcore/cm4_wire_int_slave_test:irqcnt" \
            "examples/dualcore/cm4_wire_dma_test:dmairq"; do
    rel=${spec%%:*}; tok=${spec##*:}; short=${rel##*/}
    src="$EVKB/$rel/transcript_qemu.txt"
    if [ ! -f "$src" ]; then echo "SKIP: missing_${tok}_${short} (no transcript)"; continue; fi

    grep -v "^$tok=" "$src" > "$WORK/$short.no-$tok"
    run_gate "$rel" "run_qemu.sh" "$WORK/$short.no-$tok"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                # a vacuous PASS is the bug
    echo "$OUT_TEXT" | grep -q "$tok not reported" || result=1 # and it must name the token
    report "missing_${tok}_${short}" $result

    # Over-correction guard: the UNTOUCHED transcript must still pass, or the
    # case above would be satisfied by a gate that simply always fails.
    cp "$src" "$WORK/$short.green"
    run_gate "$rel" "run_qemu.sh" "$WORK/$short.green"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_${short}" $result

    # The fabricated capture looks exactly like a real green run; leaving it in
    # the example directory would mislead the next reader. Gates rm -f it on
    # start, so this is tidiness, not correctness.
    rm -f "$EVKB/$rel"/*.uart
done

# --- 3. a "the device WORKS" gate must fail on the device-absent capture ----
# networking/m2_sdio_probe is the first example carrying two gates that assert
# OPPOSITE outcomes: run_qemu.sh wants the card-absent fallback (a plain SD
# memory card ignores CMD5), run_qemu_wifi.sh wants real enumeration against
# QEMU's opt-in IW416 model. The enumeration gate is worthless if it passes
# whether or not `-machine m2-wifi=on` took effect, and that is not a
# theoretical worry: the two runs share an ELF and a directory, and the fallback
# capture is what you get by dropping one machine property.
#
# Fixture is that gate's own committed transcript with the one line swapped for
# the fallback the sibling gate asserts.
wifi_rel="examples/networking/m2_sdio_probe"
wifi_src="$EVKB/$wifi_rel/transcript_qemu_wifi.txt"
if [ ! -f "$wifi_src" ]; then
    echo "SKIP: absent_card_fails_wifi_gate (no transcript)"
else
    sed 's/^sdio_begin=ok rc=0/sdio_begin=cmd5-no-response rc=-6/' "$wifi_src" > "$WORK/m2.absent"
    run_gate "$wifi_rel" "run_qemu_wifi.sh" "$WORK/m2.absent"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                      # must not pass
    echo "$OUT_TEXT" | grep -q "the card-absent fallback ran" || result=1   # and name it
    report "absent_card_fails_wifi_gate" $result

    # Over-correction guard, as above: the untouched transcript must still pass.
    cp "$wifi_src" "$WORK/m2.green"
    run_gate "$wifi_rel" "run_qemu_wifi.sh" "$WORK/m2.green"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_m2_sdio_probe_wifi" $result

    # This gate's captures live in the board build dir (gate_capture_path), not
    # beside the sources, so the fabricated ones are cleaned from there.
    rm -f "$EVKB/$wifi_rel"/build/wifi.uart
fi

# --- 4. the same rule for m2_uap_probe's model gate (W17) --------------------
# networking/m2_uap_probe is the second example carrying two gates asserting
# OPPOSITE outcomes, and its model gate has a sharper reason to be checked than
# m2_sdio_probe's: it asserts a NEGATIVE (every AP command indistinguishable
# from a reserved id). A gate whose expected answer is "no" is exactly the kind
# that can pass on a capture where nothing happened at all -- the card-absent
# run emits no probe cells whatsoever, and "no cell said SUPPORTED" is trivially
# true of it. If this gate ever passes that capture, its silicon counterpart is
# worthless and so is the Phase-0 answer resting on it.
uap_rel="examples/networking/m2_uap_probe"
uap_src="$EVKB/$uap_rel/transcript_qemu_wifi.txt"
if [ ! -f "$uap_src" ]; then
    echo "SKIP: absent_card_fails_uap_gate (no transcript)"
else
    sed 's/^sdio_begin=ok rc=0/sdio_begin=cmd5-no-response rc=-6/' "$uap_src" > "$WORK/uap.absent"
    run_gate "$uap_rel" "run_qemu_wifi.sh" "$WORK/uap.absent"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                      # must not pass
    echo "$OUT_TEXT" | grep -q "the card-absent fallback ran" || result=1   # and name it
    report "absent_card_fails_uap_gate" $result

    # A capture with the probe cells present but the VERDICT missing must fail
    # too. Deleting the verdict is the cheapest way a future refactor breaks
    # this example silently, and "no verdict" must never read as "the negative
    # verdict" -- the tree's standing rule that a MISSING counter is not a zero.
    grep -v '^uap_verdict=' "$uap_src" > "$WORK/uap.noverdict"
    run_gate "$uap_rel" "run_qemu_wifi.sh" "$WORK/uap.noverdict"; rc=$?
    [ "$rc" -ne 0 ] && result=0 || result=1
    report "missing_verdict_fails_uap_gate" $result

    # Over-correction guard, as above: the untouched transcript must still pass.
    cp "$uap_src" "$WORK/uap.green"
    run_gate "$uap_rel" "run_qemu_wifi.sh" "$WORK/uap.green"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_m2_uap_probe_wifi" $result

    rm -f "$EVKB/$uap_rel"/build/wifi.uart
fi

# --- 5. and again for the Arduino WiFi facade (merged from the arduino line) --
# ★ When the uAP and facade sections were merged, section 4's closing `fi` was
# lost at the conflict boundary.  The script then died with "unexpected end of
# file" -- AFTER printing eleven PASS lines, having silently never run sections
# 4 and 5.  Only the exit code (2) said so.
# So: judge this suite by its EXIT CODE and its CASE COUNT, never by the PASS
# lines scrolling past.  A suite that stops early looks exactly like a suite
# that passed, which is the same defect it exists to catch in gates.
# Same class, second instance: networking/wifi_client_test. The Arduino WiFi
# facade's two gates assert opposite outcomes off ONE elf in ONE directory --
# run_qemu.sh wants WL_NO_SHIELD (255) from the card-absent fallback,
# run_qemu_wifi.sh wants WL_NO_SSID_AVAIL (1) from a real scan against the IW416
# model. Dropping `-machine m2-wifi=on` turns the second run into the first, so
# the enumeration gate must reject a 255 capture by name or it proves nothing
# about whether the model was ever attached.
#
# Worth covering separately from m2_sdio_probe above even though the shape is
# identical: that gate's negative keys on a DRIVER token (sdio_begin=...), this
# one on the FACADE's status byte, and the two can regress independently.
fac_rel="examples/networking/wifi_client_test"
fac_src="$EVKB/$fac_rel/transcript_qemu_wifi.txt"
if [ ! -f "$fac_src" ]; then
    echo "SKIP: absent_card_fails_wifi_client_gate (no transcript)"
else
    sed 's/^wifi_status=1$/wifi_status=255/' "$fac_src" > "$WORK/fac.absent"
    run_gate "$fac_rel" "run_qemu_wifi.sh" "$WORK/fac.absent"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                          # must not pass
    echo "$OUT_TEXT" | grep -q "m2-wifi=on did not take" || result=1     # and name it
    report "absent_card_fails_wifi_client_gate" $result

    # Over-correction guard, as above: the untouched transcript must still pass.
    cp "$fac_src" "$WORK/fac.green"
    run_gate "$fac_rel" "run_qemu_wifi.sh" "$WORK/fac.green"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_wifi_client_wifi" $result

    rm -f "$EVKB/$fac_rel"/build/wifi.uart
fi

# --- 6. the m2_hci_probe pair (BT-1) ----------------------------------------
# run_qemu.sh asserts the card-ABSENT fallback (LPUART2 on a null chardev:
# Reset times out BY NAME and the image heartbeats afterwards); run_qemu_hci.sh
# asserts a BIDIRECTIONAL transport against hci_peer.py.  The [hci] gate is
# worthless if it passes the fallback capture -- the two share an ELF, and
# dropping the second -serial is all it takes to get the fallback -- so the
# fallback transcript must FAIL it, by name.  (Under the fake qemu the peer
# cannot connect and exits 2; the gate checks the capture BEFORE the peer's
# exit code, so the named failure is the capture's, as it must be.)
hci_rel="examples/networking/m2_hci_probe"
hci_absent="$EVKB/$hci_rel/transcript_qemu.txt"
if [ ! -f "$hci_absent" ]; then
    echo "SKIP: absent_capture_fails_hci_gate (no transcript)"
else
    run_gate "$hci_rel" "run_qemu_hci.sh" "$hci_absent"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                          # must not pass
    echo "$OUT_TEXT" | grep -q "the card-absent fallback ran" || result=1   # and name it
    report "absent_capture_fails_hci_gate" $result

    # A dead QEMU must fail the fallback gate BY NAME, like every other runner.
    run_gate "$hci_rel" "run_qemu.sh"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "FAIL: no UART capture" || result=1
    report "dead_qemu_named_m2_hci_probe" $result

    # Over-correction guard: the committed fallback transcript still passes its own gate.
    run_gate "$hci_rel" "run_qemu.sh" "$hci_absent"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_m2_hci_probe" $result

    rm -f "$EVKB/$hci_rel"/build/hci_*.uart "$EVKB/$hci_rel"/build/hci_*.peer \
          "$EVKB/$hci_rel"/build/hci_*.dbg "$EVKB/$hci_rel"/build/serial.uart "$EVKB/$hci_rel"/build/serial.dbg
fi

# --- 7. rotary_knob_bench: green fixture passes; tamper and bad-golden fail --
# examples/display/rotary_knob_bench (Task 8): the tripwire and golden checks
# in that gate are individually meaningless unless each can be shown to FAIL
# when it should. Fixture is the gate's own committed transcript_qemu.txt.
RKB="examples/display/rotary_knob_bench"
if [ -d "$EVKB/$RKB" ] && [ -f "$EVKB/$RKB/transcript_qemu.txt" ]; then
    run_gate "$RKB" "run_qemu.sh" "$EVKB/$RKB/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_rotary_knob_bench" $result

    # Tripwire: a fabricated gpu result must fail BY NAME even though every
    # genuine assertion still passes on the rest of the capture.
    cp "$EVKB/$RKB/transcript_qemu.txt" "$WORK/rkb_tamper.txt"
    echo "cell=vector/gpu/notch st=ok crc=0xDEADBEEF init_us=1 rotor_bytes=0" >> "$WORK/rkb_tamper.txt"
    run_gate "$RKB" "run_qemu.sh" "$WORK/rkb_tamper.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                         # must not pass
    echo "$OUT_TEXT" | grep -q "GPU cell reported a result" || result=1  # and name it
    report "rkb_gpu_tripwire_fires" $result

    # A corrupted golden must fail naming the cell, not pass or die silently.
    sed 's|^cell=vector/sw/notch st=ok crc=0x........|cell=vector/sw/notch st=ok crc=0xBADBADBA|' \
        "$EVKB/$RKB/transcript_qemu.txt" > "$WORK/rkb_badcrc.txt"
    run_gate "$RKB" "run_qemu.sh" "$WORK/rkb_badcrc.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "missing/wrong: cell=vector/sw/notch" || result=1
    report "rkb_bad_golden_fails_by_name" $result
else
    echo "SKIP: rotary_knob_bench vacuity (example or fixture missing)"
fi

# --- 8. synthui_fader_test: green fixture passes; bad golden and a missing
# damage counter fail by name (NEW-23).  The missing-counter case is the
# spec's "absent counter must not read as proof" requirement -- and it also
# regression-guards the gate's DAREA extraction, which originally aborted
# under set -e on a missing token instead of reaching its named FAIL
# (fixed 018a4e8).
FDT="examples/display/synthui_fader_test"
if [ -d "$EVKB/$FDT" ] && [ -f "$EVKB/$FDT/transcript_qemu.txt" ]; then
    run_gate "$FDT" "run_qemu.sh" "$EVKB/$FDT/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_synthui_fader_test" $result

    # A corrupted golden must fail naming the check, not pass or die silently.
    sed 's|^fd_crc=0x........|fd_crc=0xBADBADBA|' \
        "$EVKB/$FDT/transcript_qemu.txt" > "$WORK/fd_badcrc.txt"
    run_gate "$FDT" "run_qemu.sh" "$WORK/fd_badcrc.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "FAIL: bank checksum" || result=1
    report "fader_bad_golden_fails_by_name" $result

    # A capture with NO fd_damage line must fail as a missing guard --
    # an absent counter must never read as the good outcome.
    grep -v "^fd_damage " "$EVKB/$FDT/transcript_qemu.txt" > "$WORK/fd_nodmg.txt"
    run_gate "$FDT" "run_qemu.sh" "$WORK/fd_nodmg.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "delta damage guard missing" || result=1
    report "fader_missing_damage_counter_fails" $result
else
    echo "SKIP: synthui_fader_test vacuity (example or fixture missing)"
fi

# --- 9. vglite_conformance: green fixture passes; a fabricated ok verdict, a
# fabricated api2=success and a truncated matrix all fail by name (NEW-32).
# The ok tripwire is the whole reason this gate exists -- QEMU has no GC355, so
# a harness that reported `ok` for cases it never ran would sail through every
# other assertion. The api2 case pins a NEAR-MISS the gate was originally
# written with: the tripwire's first form grepped `api=success`, which does NOT
# match a fabricated `api2=success`, so half the verdict surface was
# unprotected. The truncated-matrix case pins the count check, which is what
# stops a short capture reading as a smaller matrix that happened to pass.
VGC="examples/display/vglite_conformance"
if [ -d "$EVKB/$VGC" ] && [ -f "$EVKB/$VGC/transcript_qemu.txt" ]; then
    run_gate "$VGC" "run_qemu.sh" "$EVKB/$VGC/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_vglite_conformance" $result

    # A fabricated passing verdict must fail BY NAME even though every genuine
    # assertion still passes on the rest of the capture.
    cp "$EVKB/$VGC/transcript_qemu.txt" "$WORK/vgc_ok.txt"
    echo "vgc case=path/single-contour-rect api=success pixel=ok detail=fill=6400 repeat=same" \
        >> "$WORK/vgc_ok.txt"
    run_gate "$VGC" "run_qemu.sh" "$WORK/vgc_ok.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                          # must not pass
    echo "$OUT_TEXT" | grep -q "a case reported ok in QEMU" || result=1  # and name it
    report "vgc_ok_tripwire_fires" $result

    # The near-miss: api2=success alone, with every other column in the skip
    # shape, so ONLY the `api2?=success` form of the tripwire can see it.
    cp "$EVKB/$VGC/transcript_qemu.txt" "$WORK/vgc_api2.txt"
    echo "vgc case=path/single-contour-rect api=skip api2=success pixel=skip detail=engine=absent repeat=skip" \
        >> "$WORK/vgc_api2.txt"
    run_gate "$VGC" "run_qemu.sh" "$WORK/vgc_api2.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "an API call succeeded in QEMU" || result=1
    report "vgc_api2_tripwire_fires" $result

    # A capture missing one case line must fail as a short matrix, not pass as
    # a smaller one.
    grep -v "^vgc case=path/degenerate-zero-area" \
        "$EVKB/$VGC/transcript_qemu.txt" > "$WORK/vgc_short.txt"
    run_gate "$VGC" "run_qemu.sh" "$WORK/vgc_short.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "expected 15 case lines, got 14" || result=1
    report "vgc_truncated_matrix_fails" $result
else
    echo "SKIP: vglite_conformance vacuity (example or fixture missing)"
fi

exit $FAILED

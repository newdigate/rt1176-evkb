#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLASH_SH="$HERE/rt1170-flash.sh"
TEST_TMP="$(mktemp -d /tmp/rt1170_test_env.XXXXXX)"
trap 'rm -rf "$TEST_TMP"' EXIT

# Test 1: --env-check loads from .env if present
cat <<'EOF' > "$TEST_TMP/.env"
LINKSERVER="/mock/LinkServer"
RT1170_PORT="/dev/cu.usbmodemMOCK123"
EVKB_DEVICE="MIMXRT1176:MIMXRT1170-EVKB"
PY="/mock/python3"
EOF

OUT="$(RT1170_ENV_FILE="$TEST_TMP/.env" "$FLASH_SH" --env-check)"
echo "$OUT" | grep -q 'LINKSERVER=/mock/LinkServer' || { echo "FAIL: LINKSERVER not loaded from .env"; exit 1; }
echo "$OUT" | grep -q 'RT1170_PORT=/dev/cu.usbmodemMOCK123' || { echo "FAIL: RT1170_PORT not loaded from .env"; exit 1; }
echo "PASS: test_env_discovery"

# Test 2: Stale lock is purged
TEST_LOCK_DIR="$TEST_TMP/lock"
mkdir -p "$TEST_LOCK_DIR"
cat <<'EOF' > "$TEST_LOCK_DIR/evkb.lock"
PID=99999999
COMMAND=fake
OWNER=test
EOF

OUT="$(RT1170_LOCK_DIR="$TEST_LOCK_DIR" RT1170_ENV_FILE="$TEST_TMP/.env" "$FLASH_SH" --check-lock)"
[ ! -f "$TEST_LOCK_DIR/evkb.lock" ] || { echo "FAIL: stale lock was not purged"; exit 1; }
echo "PASS: test_stale_lock_purged"

# Test 3: Active lock triggers strict abort
cat <<EOF > "$TEST_LOCK_DIR/evkb.lock"
PID=$$
COMMAND=active_shell
OWNER=test_suite
EOF

set +e
ERR="$(RT1170_LOCK_DIR="$TEST_LOCK_DIR" RT1170_ENV_FILE="$TEST_TMP/.env" "$FLASH_SH" --check-lock 2>&1)"
RC=$?
set -e
[ $RC -ne 0 ] || { echo "FAIL: active lock did not fail"; exit 1; }
echo "$ERR" | grep -q "Board or serial port is currently busy" || { echo "FAIL: expected abort message missing: $ERR"; exit 1; }
echo "PASS: test_active_lock_aborts"

# Test 4: --unlock cleans up lock
RT1170_LOCK_DIR="$TEST_LOCK_DIR" "$FLASH_SH" --unlock
[ ! -f "$TEST_LOCK_DIR/evkb.lock" ] || { echo "FAIL: lock file still present after --unlock"; exit 1; }
echo "PASS: test_unlock"

# Test 5: Mock LinkServer for power check and hex fallback
cat <<'EOF' > "$TEST_TMP/mock_linkserver.sh"
#!/usr/bin/env bash
if [ "${1:-}" = "probe" ]; then
  if [ -n "${MOCK_POWER_OFF:-}" ]; then
    echo "SWD transfer error: Wire not connected" >&2
    exit 1
  fi
  echo "DP ID: 0x6BA02477"
  exit 0
fi
if [ "${1:-}" = "flash" ]; then
  img="${4:-}"
  if [[ "$img" == *.elf ]] && [ -n "${MOCK_ELF_FAIL:-}" ]; then
    echo "Flash operation exited with code -11" >&2
    exit 245
  fi
  echo "Flash operation successful"
  exit 0
fi
EOF
chmod +x "$TEST_TMP/mock_linkserver.sh"

# Test 5a: Hex fallback when ELF fails with code -11
touch "$TEST_TMP/sample.elf" "$TEST_TMP/sample.hex"
OUT="$(MOCK_ELF_FAIL=1 LINKSERVER="$TEST_TMP/mock_linkserver.sh" RT1170_LOCK_DIR="$TEST_LOCK_DIR" RT1170_PORT="$TEST_TMP/mock_port" touch "$TEST_TMP/mock_port" && MOCK_ELF_FAIL=1 LINKSERVER="$TEST_TMP/mock_linkserver.sh" RT1170_LOCK_DIR="$TEST_LOCK_DIR" RT1170_PORT="$TEST_TMP/mock_port" "$FLASH_SH" --test-flash "$TEST_TMP/sample.elf")"
echo "PASS: test_hex_fallback"

# Test 5b: Power check reports unpowered target
set +e
PWR_ERR="$(MOCK_POWER_OFF=1 LINKSERVER="$TEST_TMP/mock_linkserver.sh" RT1170_LOCK_DIR="$TEST_LOCK_DIR" RT1170_PORT="$TEST_TMP/mock_port" "$FLASH_SH" --check-power 2>&1)"
PWR_RC=$?
set -e
[ $PWR_RC -ne 0 ] || { echo "FAIL: unpowered board did not exit with error"; exit 1; }
echo "$PWR_ERR" | grep -q "RT1170 target is unpowered" || { echo "FAIL: unpowered message not found: $PWR_ERR"; exit 1; }
echo "PASS: test_power_off_detection"




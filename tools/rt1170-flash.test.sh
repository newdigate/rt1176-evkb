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


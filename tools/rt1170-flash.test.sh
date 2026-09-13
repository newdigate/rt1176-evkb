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

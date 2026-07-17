#!/bin/bash
# Build the TCM-resident CM4 image and emit cm4_image.h (a uint32_t array the
# CM7 stages into the backdoor). Bare-metal arm-none-eabi (NOT the Arduino
# core): own startup + linker. Phase-2A; automated into teensy-cmake-macros in
# Phase 2B.
#
# Usage: build_cm4.sh <output_dir>   # writes <output_dir>/cm4_image.h
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-$DIR}"
TOOL="${ARM_TOOL:-/Applications/ARM_10/bin/arm-none-eabi}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

CFLAGS="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
    -O2 -ffreestanding -fno-common -ffunction-sections -fdata-sections \
    -Wall -Wextra"

"$TOOL-gcc" $CFLAGS -c "$DIR/startup_cm4.S" -o "$WORK/startup.o"
"$TOOL-gcc" $CFLAGS -c "$DIR/main_cm4.c"    -o "$WORK/main.o"
"$TOOL-gcc" $CFLAGS -nostdlib -Wl,--gc-sections -Wl,-Map,"$WORK/cm4.map" \
    -T "$DIR/cm4.ld" "$WORK/startup.o" "$WORK/main.o" -o "$WORK/cm4.elf"
"$TOOL-objcopy" -O binary "$WORK/cm4.elf" "$WORK/cm4_image.bin"

python3 "$DIR/bin2header.py" "$WORK/cm4_image.bin" cm4_image > "$OUT/cm4_image.h"
echo "cm4_image.h: $(wc -c < "$WORK/cm4_image.bin") bytes -> $OUT/cm4_image.h"

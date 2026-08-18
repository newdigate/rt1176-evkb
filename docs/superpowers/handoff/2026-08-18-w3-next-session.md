# Continue M.2 Wi-Fi bring-up (W3: host command interface)

We are bringing up a u-blox **M2-MAYA-W161** M.2 card (NXP **IW416**, aka
**SD8978**) on a **MIMXRT1170-EVKB RevC3**, in the Arduino/Teensyduino-style
repo at `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
The driver lives in the sibling repo `~/Development/M2Radio` (branch `master`,
pushed to github.com/newdigate/M2Radio, pinned by SHA in `evkb.cmake`).

**Read first:** `docs/m2-evkb-revc3.md` (board map + REQUIRED REWORK) and
`examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` (the full hardware
record, including the wrong turns).

## Where things stand

| Phase | State |
|---|---|
| W1 — SDIO enumerate | ✅ hardware-verified: `manfid=0x2DF cardid=0x9158 io_functions=1` |
| W2 — firmware download | ✅ hardware-verified: `fw_download=ok fw_status=0xFEDC` (FIRMWARE_READY) |
| W3 — host commands | ⚠️ TX implemented, **no response ever arrives** |

## FIRST ACTION: the board is running a hanging image

The last W3 change (unmasking `HIM_ENABLE` + polling `CMD_PORT_UPLD`) makes the
firmware **hang before its first report** — `setup()` never completes, so the
console is silent. Before anything else, rebuild and reflash to get back to a
reporting image; comment out the `getHwSpec()` call in
`examples/networking/m2_sdio_probe/m2_sdio_probe.cpp` if needed. Confirm you can
see `fw_download=ok ... fw_status=0xFEDC` again before touching W3 logic.

## The board REQUIRES two hand-fitted resistors (already done on this unit)

A stock RevC3 **cannot** run an M.2 Wi-Fi card. Both are DNP from the factory
and have been bridged by hand on this board:

* **R404** — `GPIO_AD_31` → PDn (J54 pin 56). **Fatal without it.** PDn is the
  radio's master power-down; the 10K pull-up `R829` holds it at the "normal
  mode" high the datasheet asks for, so every static check passes, but a module
  that came up in a bad state can only be recovered by asserting PDn LOW — and
  without R404 no firmware can. This is also why NXP's own `wifi_cli` hangs at
  "Initialize WLAN Driver" on this board: it drives `GPIO_AD_31` too.
* **R1901** — module→MCU BT UART RX. Without it Bluetooth HCI is write-only.

Working power-up sequence (in `m2_sdio_probe.cpp::m2ReleaseWifiReset`):
PDn low ≥10 ms → high → **wait ~1 s for the ROM** → enumerate.

## W3: what is implemented, and exactly what fails

In `~/Development/M2Radio/iw416/Iw416.{h,cpp}`:

* SDIOPkt framing: `[u16 size][u16 pkttype=1][u16 cmd][u16 size][u16 seq][u16 result][body]`
* `sendHostCmd()` → CMD53 write to **`ioport | CMD_PORT_SLCT`** (0x8000).
  The bootloader accepts the bare I/O port during download; the running
  firmware does not.
* `readHostResp()` → poll `HOST_INT_STATUS` (0x0C), read `RD_LEN_P0_L/U`
  (**0x18/0x19**), CMD53-read the reply.
* `getHwSpec()` → `FUNC_INIT` (0x00A9) then `GET_HW_SPEC` (0x0003); MAC at
  body+8, fw_release at body+18, where body = 4 (SDIO hdr) + 8 (HostCmd hdr).

**Symptom:** `hw_spec=cmd-timeout` in all three variants tried — bare ioport,
command port, and command port + `HIM_ENABLE` unmask + polling `CMD_PORT_UPLD`
(bit 6) alongside `UP_LD` (bit 0).

**Two concrete suspects, in order:**

1. **`HOST_INT_STATUS` clear semantics.** The current code writes `~mask` to it,
   which was a guess, never verified. If the register is write-1-to-clear rather
   than write-0-to-clear, we are clearing the wrong bits — and possibly wedging
   the card, which fits the hang appearing exactly when that write was added.
   **Verify what the register does before writing to it again.**
2. **Command-port read length.** A reply to a command-port write may publish its
   length somewhere other than `RD_LEN_P0`.

## Method note — this is what worked, and what did not

The successes in this project all came from **instrumenting first**: reading a
pad back with an internal pull-up *and* pull-down to tell "driven" from
"floating" proved the card was alive; reading `PRES_STATE` line levels
exonerated the whole host side; a register read-back proved the 1.8 V switch
actually took. The W3 attempts, by contrast, pattern-matched against NXP's
source and changed several things per iteration. Go back to instrumenting.

## Traps already paid for — do not rediscover these

* **`CARD_STATUS_REG` is 0x5C** on SD8978. NXP's `fwdnld_sdio.c` has a stale
  comment saying `(0X30)`, which is the **SD8801** address. Reading 0x30 returns
  0x00 forever, the card-readiness gate never fires, **the whole image still
  transfers and every chunk reports success**, and only final validation fails.
* **`RD_LEN_P0_L/U` is 0x18/0x19** on SD8978 (0x08/0x09 is SD8801). Same trap.
* The bootloader asks for **2049 bytes** at a time — a 2048-byte buffer fails.
* The firmware array must be `__attribute__((section(".progmem")))`. The
  `imxrt1176` linker script folds `.rodata*` into `.data`, which is **DTCM
  (256K)**; a 273 KB const array there fails the link with a confusing
  `.bss is not within region DTCM`. **`FLASHMEM` is `#define FLASHMEM` (empty)
  in `AudioStream.h` and does NOT do this job.**
* **Do NOT enable LPUART2 hardware flow control.** CTS/RTS mux
  `GPIO_DISP_B2_12/13`, which on this board are `RGMII1_PHY_INTB` and
  **`ETHPHY_RST_B`** — asserting RTS holds the gigabit PHY in reset.

## Firmware blob — never commit it

NXP LA_OPT binary licence. Supplied at configure time:

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2RADIO_IW416_FW=$HOME/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc
```

Without it the example still builds and reports `fw_download=skipped`, so the
QEMU gate is unaffected on a machine with no SDK.

NXP's driver source (BSD-3-Clause, the protocol reference) is at
`~/Development/mcuxsdk-ws/mcuxsdk/middleware/wifi_nxp/` — see
`sdio_nxp_abs/fwdnld_sdio.c`, `wifidriver/wifi-sdio.c`,
`sdio_nxp_abs/incl/mlan_sdio_defs.h`.

## Hardware workflow

* Flash: `/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/m2_sdio_probe.elf`
  — **do not hold the VCOM while programming**. Clear stale daemons first:
  `pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink`.
* Console: `python3 tools/rt1170-console.py /dev/cu.usbmodem<TAB> 115200`.
  The probe re-reports every 5 s, so there is no need to race the boot.
* **The microSD slot J15 must stay EMPTY** — it shares uSDHC1 with the M.2
  socket. `LinkServer run` cannot load SDRAM builds; use `flash ... load`.
* Another session may share this board. Coordinate before killing probe daemons.

## Gate discipline

`examples/networking/m2_sdio_probe/run_qemu.sh` asserts the **module-absent**
path (`cmd5-no-response`) — QEMU has no SDIO model, so that remains correct and
must keep passing. Never weaken it to match hardware behaviour. Run it as
`./run_qemu.sh`, never `sh run_qemu.sh`.

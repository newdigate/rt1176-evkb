# RT1176 SerialUSB — Phase 2: USB CDC bulk data + `Serial : Stream` — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-04
**Target:** `cores/imxrt1176` Arduino core for the MIMXRT1170-EVKB. QEMU-gated (the ChipIdea CDC virtual host bridges bulk data to a chardev bidirectionally) **and** hardware-verified (the Mac's native-USB CDC port echoes bytes).

**Decomposition note:** This is **Phase 2 of 2** for `SerialUSB`. Phase 1 = the device **enumerates** as CDC-ACM (reaches `usb_configuration != 0`) — DONE, HW-verified & pushed. Phase 2 = **bulk RX/TX data movement** + the `Serial : Stream` API. Success = an **echo/loopback** works: the host sends bytes, firmware echoes them, the host reads them back.

## Goal

Make the enumerated CDC device move bytes. `Serial.print()`/`write()` → the host receives them; host → `Serial.read()`/`available()`/`peek()`. Concrete success criterion: **echo works end-to-end** — in QEMU (socket chardev ↔ ChipIdea CDC bridge ↔ guest bulk EPs) and on hardware (pyserial on `/dev/cu.usbmodem…`, VID 0x1209).

## Scope

**In:** port `cores/teensy4/usb_serial.c` (CDC-trimmed) → `cores/imxrt1176/usb_serial.c`/`.h`: the RX/TX ring buffers; `usb_serial_configure()` (prime bulk RX transfers + configure EP2/3/4 + arm the flush timer) and `usb_serial_reset()`; the `usb_serial_class : public Stream` (`begin/end`, `available/read/peek`, `write/print/flush/clear/availableForWrite/send_now`, `baud/dtr/rts/…`); instantiate `Serial` + a `SerialUSB` alias; the GPTIMER0 auto-flush timer; auto-init USB at startup; resolve the duplicate line-state symbols; the QEMU **data** gate (echo) and HW echo verification.

**Out (YAGNI):** `SerialUSB1`/`SerialUSB2` (the CDC2/CDC3 blocks of teensy4 `usb_serial.h`); the `serialEvent()`/`yield`-dispatch hook (this core's `yield()` services only `Serial1`); USB host/OTG mode; USB_OTG2; suspend/resume/remote-wakeup; composite devices; the HalfKay reboot hooks; dynamic dTD free/alloc on reset (`usb_serial_reset()` stays a light reset like teensy4's).

## Decisions (from brainstorming)

1. **Flush/TX strategy: port the GPTIMER0 auto-flush timer** (faithful teensy4 port). Small writes batch into the TX ring; a 75 µs USB hardware one-shot flushes a partial packet so a bare `Serial.print()` reaches the host on HW. The **gate additionally calls `Serial.send_now()`** so it is correct regardless of whether QEMU models GPTIMER0.
2. **Object name: `Serial` + `SerialUSB` alias.** `usb_serial_class Serial;` plus `usb_serial_class &SerialUSB = Serial;` (one object, two names). Teensy-faithful (`Serial` = USB CDC), and `Serial1` = LPUART1 already exists and is unaffected.
3. **USB init: auto in `main.cpp`.** `usb_init()` is called before `setup()` so `Serial` is live with no manual init — Arduino-idiomatic. **Consequence:** every sketch now enumerates USB (adds the bounded ~1 ms PLL poll at boot) → full QEMU-gate regression required.

## Ground truth already in place (do NOT re-derive or re-port)

- **Transfer scheduler is DONE** in the Phase-1 `cores/imxrt1176/usb.c`: `usb_config_rx`/`usb_config_tx`, `usb_prepare_transfer`, `usb_transmit`/`usb_receive`, `schedule_transfer`, `run_callbacks`, `usb_transfer_status`, the `endpointN_notify_mask` completion dispatch in `usb_isr`, and `usb_endpoint_config`. Phase 2 only *calls* these — it does not touch them (except the additive timer hook below).
- **QEMU CDC model bridges data bidirectionally** (`qemu2/hw/usb/chipidea_cdc.c`): `cdc_on_bulk_in` → `qemu_chr_fe_write_all(&ci->cdc_be, vh_in, len)` drains guest TX (bulk-IN) to the chardev; `cdc_chr_read` → `ci_udc_arm_bulk_out(...)` forwards chardev input to guest RX (bulk-OUT). Activates once the guest asserts DTR (logs `CI-CDC: DTR asserted -> bridging USB serial`).
- **`usbcdc` named chardev already wired** in `qemu2/hw/arm/fsl-imxrt1170.c:~890` (`qemu_chr_find("usbcdc")` → `qdev_prop_set_chr(DEVICE(&s->usb[i]),"chardev",cdc)`). **qemu2 needs NO change.** The Phase-1 runner passed `-chardev null,id=usbcdc`; the Phase-2 gate swaps that for a real (socket) backend.
- **Endpoints (from `usb_desc.h`):** `CDC_ACM_ENDPOINT 2` (interrupt-IN, 16 B), `CDC_RX_ENDPOINT 3` (bulk-OUT), `CDC_TX_ENDPOINT 4` (bulk-IN); `CDC_RX_SIZE_480 512`/`CDC_TX_SIZE_480 512`, `CDC_*_SIZE_12 64`; `ENDPOINT2/3/4_CONFIG` already defined. `NUM_ENDPOINTS 4`. VID/PID `0x1209`/`0x0001`.
- **D-cache is OFF** (Phase-1 HW-confirmed) → `arm_dcache_*` are no-ops; DMA structures live in `DMAMEM` (`.dmabuffers` → OCRAM).

## The seams Phase 2 resolves

### Seam A — duplicate line-state symbols (do FIRST)
`usb.c` currently *defines* `usb_cdc_line_coding[2]`, `usb_cdc_line_rtsdtr_millis`, `usb_cdc_line_rtsdtr` (`usb.c:87-89`). teensy4 `usb_serial.c:54-56` also defines them → "multiple definition" under `-fno-common`. **Resolution:** `usb_serial.c` owns them (defines all four, incl. `usb_cdc_transmit_flush_timer`); **delete `usb.c:87-89`** — the `extern`s already exist in `usb_dev.h:40-42`, so `endpoint0_setup`/`endpoint0_complete` still resolve at link. Also **delete the two stubs** `usb_serial_configure(){}` / `usb_serial_reset(){}` (`usb.c:108-109`); the real ones come from `usb_serial.c`.

### Seam B — DMAMEM on ALL bulk DMA structures (highest risk)
teensy4 marks only `txbuffer`/`rx_buffer` as `DMAMEM` and leaves **`tx_transfer[TX_NUM]`** and **`rx_transfer[RX_NUM]`** (the dTDs the USB bus-master walks) in plain `.bss`. On RT1176 plain `.bss` = **DTCM = unreachable by the USB DMA** (the exact Phase-1 class-of-bug: the two EP0 dTDs missed `DMAMEM` initially). **Resolution:** add `DMAMEM` to **all four** — `txbuffer`, `rx_buffer`, `tx_transfer[]`, `rx_transfer[]` — each `__attribute__((aligned(32)))` (buffers) / `DMAMEM __attribute__((used, aligned(32)))` (descriptor arrays). OCRAM footprint: `rx_buffer` 8×512 = 4 KB, `txbuffer` 4×2048 = 8 KB, descriptors ~384 B — fits alongside the Phase-1 dQH/dTDs.

### Seam C — GPTIMER0 flush timer (decision 1)
The Phase-1 `usb.c` does **not** handle the USB general-purpose timer. Additive changes:
- **`imxrt1176.h`** — add (style matches the existing `USB1_*` block at `USB_OTG1_BASE + offset`):
  - `USB1_GPTIMER0LD` = `USB_OTG1_BASE + 0x080`, `USB1_GPTIMER0CTRL` = `USB_OTG1_BASE + 0x084`
  - `USB_USBSTS_TI0` = `1<<24`, `USB_USBINTR_TIE0` = `1<<24`, `USB_GPTIMERCTRL_GPTRUN` = `1<<31`, `USB_GPTIMERCTRL_GPTRST` = `1<<30` (verbatim from teensy4 `imxrt.h`).
- **`usb.c`** — add global `void (*usb_timer0_callback)(void) = NULL;` and, in `usb_isr`, a branch: `if (status & USB_USBSTS_TI0) { if (usb_timer0_callback) usb_timer0_callback(); }`. (`USBINTR`'s `TIE0` is enabled by `timer_config` in `usb_serial.c`, not `usb_init`.)
- **`usb_dev.h`** — `extern void (*usb_timer0_callback)(void);`.
- **`usb_serial.c`** — port `timer_config`/`timer_start_oneshot`/`timer_stop` verbatim (they use `USB1_GPTIMER0LD/CTRL`, `USBINTR |= TIE0`).

### Seam D — small port adaptations
- `NVIC_DISABLE_IRQ(IRQ_USB1)` / `NVIC_ENABLE_IRQ(IRQ_USB1)` → `IRQ_USB_OTG1` (= 136).
- `arm_dcache_flush_delete()` / `arm_dcache_delete()` → **no-op `static inline` in `imxrt1176.h`** (D-cache off; keeps `usb_serial.c` a verbatim port and stays safe if D-cache is later enabled — replace the no-ops then).
- **Drop** the `serialEvent`/`yield_active_check_flags |= YIELD_CHECK_USB_SERIAL` lines (`usb_serial_configure` tail): this core's `yield()` (`yield.cpp`) only services `Serial1`/`serialEvent1`. The echo gate polls `Serial.available()` directly.
- Drop the `#if F_CPU < 30000000` DTCM-fallback block (not applicable; RT1176 runs at 996 MHz).

## Architecture & files

```
cores/imxrt1176/               (core repo → github teensy-cores)
  usb_serial.c   NEW  — RX/TX ring buffers, usb_serial_configure/reset,
                        read/peek/available/write/flush/availableForWrite,
                        GPTIMER0 timer_config/start/stop + flush callback.
                        Port of teensy4/usb_serial.c, single-CDC, Seams B/C/D applied.
  usb_serial.h   NEW  — usb_serial_class : public Stream (teensy4 usb_serial.h:36-190),
                        CDC2/CDC3 blocks dropped. externs for the C API + line-state.
  usb_inst.cpp   NEW  — usb_serial_class Serial;  usb_serial_class &SerialUSB = Serial;
  usb.c          MOD  — del line-state defs (→extern) + del 2 stubs; add
                        usb_timer0_callback + USB_USBSTS_TI0 branch in usb_isr.
  usb_dev.h      MOD  — add extern usb_timer0_callback.
  imxrt1176.h    MOD  — GPTIMER0 regs/bits; no-op arm_dcache_* inlines.
  main.cpp       MOD  — usb_init() before setup() (guarded by CDC macros).
evkb/
  usb_data_test/ NEW  — echo firmware + socket-chardev runner + Python echo driver.
                        The QEMU DATA gate. (usb_enum_test stays as regression.)
qemu2/                 — UNCHANGED (bridge + usbcdc chardev already present).
```

The build (`import_arduino_library(cores …/cores/imxrt1176)`) globs the whole core dir, so the new `.c`/`.cpp` files and `Serial` instantiation are picked up with no CMake edit.

## Mechanism

### RX path (host → firmware)
`usb_serial_configure()` primes `RX_NUM (8)` bulk-OUT transfers via `rx_queue_transfer(i)` (`usb_prepare_transfer` + `usb_receive(CDC_RX_ENDPOINT, …)`). On completion `usb_isr` → `run_callbacks` → `rx_event(t)` computes `len = rx_packet_size - (status>>16 & 0x7FFF)`, appends to `rx_list`/`rx_buffer`, bumps `rx_available`, re-queues the transfer. `usb_serial_read/peekchar/available/getchar/flush_input` drain the ring under `NVIC_DISABLE_IRQ(IRQ_USB_OTG1)`. `Stream::read/peek/available` delegate to these.

### TX path (firmware → host)
`usb_serial_write(buf,size)` copies into the current `txbuffer` slot; a full `TX_SIZE (2048)` slot is handed to `usb_transmit(CDC_TX_ENDPOINT,…)` immediately, a partial slot arms the GPTIMER0 one-shot (`timer_start_oneshot`). `usb_serial_flush_callback` (timer) / `usb_serial_flush_output` (`flush()`/`send_now()`) push a partial slot on demand. `availableForWrite` reports free slots. `write_buffer_free`/`putchar` as teensy4.

### configure / reset
`usb_serial_configure()`: pick `tx/rx_packet_size` from `usb_high_speed` (480→512 / FS→64); zero the ring state; `usb_config_tx(CDC_ACM_ENDPOINT, CDC_ACM_SIZE, 0, NULL)`, `usb_config_rx(CDC_RX_ENDPOINT, rx_packet_size, 0, rx_event)`, `usb_config_tx(CDC_TX_ENDPOINT, tx_packet_size, 1, NULL)`; prime the 8 RX transfers; `timer_config(usb_serial_flush_callback, 75)`. `usb_serial_reset()`: light reset (matches teensy4 — no dynamic dTD free). Both are already *called* by `usb.c` under `#if defined(CDC_STATUS_INTERFACE)&&defined(CDC_DATA_INTERFACE)` (SET_CONFIGURATION / bus-reset).

### instantiation + auto-init
`usb_inst.cpp` defines `Serial` then `SerialUSB` (reference alias, same TU → init order safe). `main.cpp`:
```c
#if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
    usb_init();     // before setup(); Serial is live for the sketch
#endif
```

## Verification

### QEMU DATA gate (`evkb/usb_data_test/`) — the echo, not just enum
- **Firmware** (`usb_data_test.cpp`): `Serial1.begin(115200)` (debug/VCOM marker); no manual `usb_init()` (auto in `main`); wait bounded until `usb_configuration != 0`; then `loop()`: `while (Serial.available()) { int c = Serial.read(); Serial.write((uint8_t)c); } Serial.send_now();` and print a `Serial1` marker (e.g. `ECHOED n`). No `Serial.begin()` wait needed (QEMU asserts DTR fast).
- **Runner** (`run_qemu_usb_data.sh`): `qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel … -display none -serial file:<vcom> -chardev socket,id=usbcdc,host=127.0.0.1,port=<P>,server=on,wait=off -d guest_errors -D <dbg>` (background). A **Python driver** connects the socket, sends `PHASE2-ECHO\n`, reads back (bounded, e.g. 3 s), asserts the echo equals what was sent → `PASS`. Cross-check `grep "CI-CDC: DTR asserted -> bridging" <dbg>` and the `ECHOED` marker in `<vcom>`.
- Rationale for **socket** over pty: deterministic, no `/dev/pts` path-scraping; the project already has Python serial tooling.

### Hardware (the real arbiter)
Flash via `LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/usb_data_test.elf` (kill LinkServer+redlinkserv first; J11 replug on probe fault — see `rt1170-evkb-flashing`). Read the debug VCOM (`/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200, pyserial) for the `ECHOED` marker. Then open the **native-USB** CDC port (`/dev/cu.usbmodem…`, VID 0x1209) with pyserial, send bytes, confirm the identical echo returns. Watch the bulk-path DMA/coherency (expected clean: D-cache off + DMAMEM).

## Error handling / edge cases

- **Host not listening (TX):** teensy4's `TX_TIMEOUT_MSEC (120)` / `transmit_previous_timeout` logic ported verbatim — `write()` returns short rather than blocking forever.
- **`!usb_configuration`:** `write` returns 0; `configure` re-runs on every SET_CONFIGURATION; bus reset (`URI`) calls `usb_serial_reset()`.
- **Short packets / ZLP:** `rx_event` handles a 0-length packet (re-queues); the `do_zlp` arg on the TX config is `1` (send ZLP on exact-multiple) as teensy4. The echo gate exercises both.
- **Buffer full (RX):** excess host input is naturally back-pressured (only 8 RX transfers primed); acceptable for the echo scope.
- **Bounded loops:** the gate's wait-for-configured and the Python read are time-bounded so the runner always terminates.

## Risks / open items (resolved in plan)

- **GPTIMER0 unmodeled in QEMU** → autoflush may not fire under QEMU. **Mitigated:** the gate calls `Serial.send_now()` explicitly; the timer is validated on HW (bare `print()` reaching the host).
- **Auto-init regressing other gates** → every sketch now brings up USB. **Mitigated:** full regression — re-run `usb_enum_test`, `tone_test`, `interval_timer_test`, `irq_attach_test`, `wire_master_test`, `spi_loopback_test` after the `main.cpp` change; confirm no boot delay/IRQ interference (USB IRQ is quiet with no host).
- **DMAMEM on the descriptor arrays (Seam B)** → if `tx_transfer[]`/`rx_transfer[]` are left in `.bss`, the bus-master can't walk them and TX/RX silently fail on HW while possibly "working" in QEMU. **Mitigated:** spec mandates DMAMEM on all four; linker-map check in the plan (all four in `.dmabuffers`).
- **Bulk DMA coherency on HW** → D-cache off makes `arm_dcache_*` no-ops. **Mitigated:** Phase-1-proven; the HW echo is the check. If a future build enables D-cache, replace the no-op inlines with real ops.
- **Socket-bridge timing** → the Python driver must connect after the guest asserts DTR and retry the read within the bound. **Mitigated:** driver connects with a short retry and a bounded read; cross-checked against the `CI-CDC: DTR asserted` log line.
```

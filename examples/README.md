# examples — RT1170-EVKB firmware

Each subdirectory is a self-contained bare-metal firmware for the NXP
**MIMXRT1170-EVKB** (i.MX RT1176), built with the `teensy-cmake-macros` +
`teensy-cores` (`imxrt1176/`) sibling repos, gated in QEMU (`tools/qrun`) and
mostly HW-verified on the EVKB. Organized into categories on 2026-07-20
(previously all flat at the `evkb/` root).

**Build any example** (from its own directory):
```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu.sh            # QEMU gate — never `sh run_qemu.sh` (it re-execs under gtimeout)
```
The two toolchain files (`rt1170-evkb.toolchain.cmake`, `rt1062-evkb.toolchain.cmake`)
live once at the repo root, in `toolchain/`, and are shared by every example —
a new example needs no `toolchain/` directory of its own.

Four examples inline their toolchain and need no flag: `storage-memory/sd_test`
and `audio/sd_wav_play_test` `include()` the shared root file directly;
`display/pxp_composite_test` and `display/pxp_draw_bench` point
`CMAKE_TOOLCHAIN_FILE` at it behind an `if(NOT ...)` guard, so an explicit `-D`
still wins.

Every example bootstraps through **`../../../evkb.cmake`**: the build macros,
the `cores` library, and all peripheral libraries resolve **local-first**
(a `$TEENSY_LIB_ROOT/<lib>` checkout wins; default `~/Development`) with a
**pinned-GitHub fallback** — a
fresh clone with no sibling checkouts fetches everything at the SHAs pinned in
`evkb.cmake`. Set `CPM_SOURCE_CACHE` (e.g. `~/.cache/CPM`) to clone each repo
once; pass `-DEVKB_FORCE_FETCH=ON` to force the fetch path (fresh-user mode);
note the macros repo itself is fetched with plain FetchContent — the one repo
`CPM_SOURCE_CACHE` doesn't cover, deliberately;
set `ARM_TOOLCHAIN_BIN` if your ARM GCC isn't at `/Applications/ARM_10/bin/`.

## Categories

| Folder | Examples |
|--------|----------|
| **dualcore** | `cm4_boot_test`, `cm4_dual_test`, `cm4_image_test`, `cm4_intr_test`, `cm4_spi_test`, `cm4_wire_test`, `cm4_wire_int_master_test`, `cm4_wire_int_slave_test`, `cm4_spi_dma_test`, `cm4_wire_dma_test`, `cm4_hotswap_test`, `cm4_hotswap2_test`, `cm4_imagebank_test`, `cm4_sai_irq_probe`, `cm4_cpp_test`, `cm4_audiostream_test`, `cm4_fft_test`, `cm4_audio_test`, `cm4_usb_irq_probe`, `cm4_usb_enum_probe`, `cm4_usb_audio_probe`, `cm4_graph_usb_capstone`, `dualcore_mu_test` |
| **usb** | `usb_data_test`, `usb_enum_test`, `usb_host_hid_test`, `usb_joystick_test`, `usb_keyboard_test`, `usb_midi_test`, `usb_mouse_test`, `usb_msc_block_test`, `usb_msc_fs_test` |
| **audio** | `audio_h_test`, `audioinput_i2s_test`, `audiooutput_i2s_test`, `audiostream_test`, `filter_fir_test`, `guard_sweep_test`, `i2s_audio_test`, `i2s_int_test`, `sai_rx_test`, `sd_wav_play_test`, `tone_test`, `tone_hw` |
| **camera** | `syspll3_bringup` (SYS_PLL3 @ 480 MHz), `camera_ov5640_id` (OV5640 chip-ID over SCCB/LPI2C6), `camera_ov5640_config` (640x480 YUV422 over MIPI), `camera_csi2_clocks` (MIPI-CSI2 RX clocking + D-PHY power-on), `camera_capture` (OV5640 → MIPI-CSI2 RX → CSI) — **bring-up in progress, no QEMU gates yet**: hardware-only checkpoints, so none owns a `run_qemu.sh` and none appears in the sweep's 89 |
| **networking** | `enet_test`, `ethernet_test`, `native_ethernet_test`, `lwip_test` |
| **storage-memory** | `sd_test`, `eeprom_test`, `sdram_test`, `extmem_test` |
| **gpio-analog** | `blink`, `gpio_loopback_hw`, `adc_loopback_hw`, `analog_test`, `dac_test`, `pwm_test`, `irq_attach_test` |
| **timing** | `interval_timer_test`, `interval_timer_hw`, `rtc_test` |
| **serial** | `serial_test`, `serial_test_rx` |
| **display** | `ssd1306_display`, `pxp_blit_test`, `lvgl_smoke_test`, `lvgl_ili9341_test`, `lvgl_rpi_panel_test`, `lvgl_rk055_panel_test` (golden human-confirmed on glass; the RPi LVGL golden pins reproducibility only), `lvgl_rk055_touch_test` (asserts LVGL's reaction — widget state/position — not pixels), `lvgl_rk055_flip_test` (golden-free; asserts flip discipline + panel-scanned-A-then-B), `lvgl_pxp_copy_bench` (PXP-vs-CPU copy correctness QEMU-gated; timings hardware-only), `pxp_composite_test` (AS compositing/colorkey/ROPs vs a silicon-measured oracle; four eye-ritual frames), `pxp_draw_bench` (draw-unit economics: fills CPU-won, blits/composites PXP-won; ADOPTION DECLINED 2026-08-01 -- revisit when image-heavy scenes exist) |
| **framework** | `arm_math_test`, `string_test`, `stream_test`, `wprogram_parity_test`, `eventresponder_test`, `edma_test` |

## Not examples (still at `evkb/` root)

- `tools/` — `qrun`, `gate-lib.sh`, `license-audit.sh`, and the board helper scripts `rt1170-flash.sh`, `rt1170-qemu.sh`, `rt1170-console.py`.
- `docs/` — specs, plans, QEMU peripheral status.
- `mkr_ssd1306_test/`, `qemu_dcd_boot_test/` — an MKR-Zero companion sketch + a DCD boot probe (not EVKB-target gates).

The Teensy-derived core (`teensy-cores`, subdirs `imxrt1176/` and `teensy4/`)
and the build macros (`teensy-cmake-macros`) are **not** in this repo at all —
they're sibling checkouts under `$TEENSY_LIB_ROOT` (default `~/Development`),
their own git repos, resolved by `evkb.cmake` (see above).

> **Path note:** each example's `CMakeLists.txt` reaches
> the shared core and macros via `${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake`
> (three levels up from `examples/<category>/<name>/`), which in turn resolves
> `teensy-cores`/`teensy-cmake-macros` under `$TEENSY_LIB_ROOT` — not a
> relative path into this repo. `tools/license-audit.sh`
> references gates by their `examples/<category>/<name>` path. Historical
> `docs/superpowers/{plans,specs}/*` and dated roadmap log entries keep their
> original flat paths as timestamped records.

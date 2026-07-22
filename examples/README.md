# examples — RT1170-EVKB firmware

Each subdirectory is a self-contained bare-metal firmware for the NXP
**MIMXRT1170-EVKB** (i.MX RT1176), built with `teensy-cmake-macros` + the
`cores/imxrt1176` Teensy-derived core, gated in QEMU (`tools/qrun`) and mostly
HW-verified on the EVKB. Organized into categories on 2026-07-20 (previously all
flat at the `evkb/` root).

**Build any example** (from its own directory):
```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu.sh            # QEMU gate — never `sh run_qemu.sh` (it re-execs under gtimeout)
```
The two SD examples (`storage-memory/sd_test`, `audio/sd_wav_play_test`) inline
their toolchain, so they build with a plain `cmake -B build`.

Every example bootstraps through **`../../../evkb.cmake`**: the build macros,
the `cores` library, and all peripheral libraries resolve **local-first**
(a `~/Development/<lib>` checkout wins) with a **pinned-GitHub fallback** — a
fresh clone with no sibling checkouts fetches everything at the SHAs pinned in
`evkb.cmake`. Set `CPM_SOURCE_CACHE` (e.g. `~/.cache/CPM`) to clone each repo
once; pass `-DEVKB_FORCE_FETCH=ON` to force the fetch path (fresh-user mode);
set `ARM_TOOLCHAIN_BIN` if your ARM GCC isn't at `/Applications/ARM_10/bin/`.

## Categories

| Folder | Examples |
|--------|----------|
| **dualcore** | `cm4_boot_test`, `cm4_dual_test`, `cm4_image_test`, `cm4_intr_test`, `cm4_spi_test`, `cm4_wire_test`, `cm4_wire_int_master_test`, `cm4_wire_int_slave_test`, `cm4_spi_dma_test`, `cm4_wire_dma_test`, `cm4_hotswap_test`, `cm4_hotswap2_test`, `cm4_imagebank_test`, `cm4_sai_irq_probe`, `cm4_cpp_test`, `cm4_audiostream_test`, `cm4_fft_test`, `cm4_audio_test`, `dualcore_mu_test` |
| **usb** | `usb_data_test`, `usb_enum_test`, `usb_host_hid_test`, `usb_joystick_test`, `usb_keyboard_test`, `usb_midi_test`, `usb_mouse_test`, `usb_msc_block_test`, `usb_msc_fs_test` |
| **audio** | `audioinput_i2s_test`, `audiooutput_i2s_test`, `audiostream_test`, `filter_fir_test`, `guard_sweep_test`, `i2s_audio_test`, `i2s_int_test`, `sai_rx_test`, `sd_wav_play_test`, `tone_test`, `tone_hw` |
| **networking** | `enet_test`, `ethernet_test`, `native_ethernet_test`, `lwip_test` |
| **storage-memory** | `sd_test`, `eeprom_test`, `sdram_test`, `extmem_test` |
| **gpio-analog** | `blink`, `gpio_loopback_hw`, `adc_loopback_hw`, `analog_test`, `dac_test`, `pwm_test`, `irq_attach_test` |
| **timing** | `interval_timer_test`, `interval_timer_hw`, `rtc_test` |
| **serial** | `serial_test`, `serial_test_rx` |
| **display** | `ssd1306_display` |
| **framework** | `arm_math_test`, `string_test`, `stream_test`, `wprogram_parity_test`, `eventresponder_test`, `edma_test` |

## Not examples (still at `evkb/` root)

- `cores/`, `teensy-cmake-macros/` — the Teensy-derived core + build macros (their own nested git repos).
- `tools/` — `qrun`, `gate-lib.sh`, `license-audit.sh`, and the board helper scripts `rt1170-flash.sh`, `rt1170-qemu.sh`, `rt1170-console.py`.
- `docs/` — specs, plans, QEMU peripheral status.
- `mkr_ssd1306_test/`, `qemu_dcd_boot_test/` — an MKR-Zero companion sketch + a DCD boot probe (not EVKB-target gates).

> **Path note:** each example's `CMakeLists.txt` and `toolchain/…cmake` reach the
> shared core via `${CMAKE_CURRENT_LIST_DIR}/../../../{cores,teensy-cmake-macros}`
> (three levels up from `examples/<category>/<name>/`). `tools/license-audit.sh`
> references gates by their `examples/<category>/<name>` path. Historical
> `docs/superpowers/{plans,specs}/*` and dated roadmap log entries keep their
> original flat paths as timestamped records.

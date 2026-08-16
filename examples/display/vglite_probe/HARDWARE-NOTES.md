# GC355 / GPU2D hardware facts (RT1176)

Established 2026-08-16 for the bare-metal VGLite port. **Every value here is
cited.** The port writes registers from these numbers, so a wrong one is a
hang, not a compile error — re-verify against the source before changing any.

`RM` = `~/Development/rt1170/rm_full.txt` (RT1170 reference manual as text).

## Peripheral

| fact | value | source |
| --- | --- | --- |
| Register base | `0x4180_0000` (1 MB window, "GPU2D (Peripheral, AHB)") | RM:2108, RM:2184 |
| Interrupt | **60** ("GPU2D interrupt") | RM:2975, RM:3736 |

## Clocks — three of them, one gate

GPU2D consumes **three** clocks, all gated by the SAME LPCG (RM:83158-83170):

| clock | root | gate |
| --- | --- | --- |
| `gpu2d_aclk` | `BUS_CLK_ROOT` | `clk_enable_gpu2d` (LPCG128) |
| `gpu2d_clk2x` | `GPU2D_CLK_ROOT` | `clk_enable_gpu2d` (LPCG128) |
| `gpu2d_hclk` | `BUS_CLK_ROOT` | `clk_enable_gpu2d` (LPCG128) |

So enabling LPCG128 ungates all three; only `gpu2d_clk2x` needs a root
configured (the bus clock root is already up — the core configures it at
startup for the AHB).

### Gate: LPCG128

- Formula: `LPCGa_DIRECT = 6000h + (a × 20h)` (RM:90202); range
  `6000h - 7120h` covers `LPCG0_DIRECT - LPCG137_DIRECT` (RM:87406).
- LPCG128 → `6000h + 128×20h = 6000h + 1000h = **7000h**`.
- CCM base is `0x40CC_0000` (RM:2553, RM:86424; also `CCM_BASE` in the core's
  `imxrt1176.h:21`), so **`LPCG128_DIRECT = 0x40CC7000`**.
- **Reset value is `0000_0001h`** (RM:87406) — bit 0 = ON, so the gate is
  already open at reset. Write it explicitly anyway: it is one store, and it
  makes the dependency legible instead of load-bearing-by-accident.

### Root: CLOCK_ROOT68 = GPU2D_CLK_ROOT

- `CLOCK_ROOT68_CONTROL` is at offset **`2200h`** (RM:87106) →
  **`0x40CC2200`**. This matches the stride the core already uses
  (`CLOCK_ROOTn_CONTROL = 0x40CC0000 + n×0x80`; `imxrt1176.h:120-122` has
  ROOT0/2/8 at `0x40CC0000`/`0100`/`0400`, and 68×0x80 = 0x2200).
- Field helpers already exist in the core: `CCM_CLOCK_ROOT_CONTROL_MUX(x)`
  (bits 10:8) and `CCM_CLOCK_ROOT_CONTROL_DIV(x)` (bits 7:0),
  `imxrt1176.h:123-124`.
- **DIV divides by `DIVIDE + 1`** (RM:91484) — DIV field 0 means ÷1.
- Maximum frequency **500 MHz** (RM:85491).
- Reset state is MUX=000, DIV=0 → `OSC_RC_48M_DIV2` ≈ **24 MHz**. Leaving it
  alone would run the GPU ~20× slower than it can go, so the port must set it.

Mux options (RM:85491-85499):

| MUX | source |
| --- | --- |
| 000 | OSC_RC_48M_DIV2 (reset) |
| 001 | OSC_24M |
| 010 | OSC_RC_400M |
| 011 | OSC_RC_16M |
| 100 | SYS_PLL2_CLK (528 MHz) |
| 101 | SYS_PLL2_PFD1 |
| **110** | **SYS_PLL3_CLK (480 MHz)** ← chosen |
| 111 | VIDEO_PLL_CLK |

**Choice: MUX=110 (SYS_PLL3_CLK), DIV=0 → 480 MHz.**

Rationale: 480 MHz is just under the 500 MHz ceiling, and **SYS_PLL3 is
already locked at 480 MHz before this code runs** — the core's own startup
records a corrected investigation of exactly this: "SYS_PLL3_CTRL @
0x40C84210 = 0x2020201B on this board at boot, in which bit 16 is 0 and bit 29
(STABLE) is 1: SysPll3 is LOCKED at 480 MHz" (`teensy-cores/imxrt1176/startup.c:385-393`).
So no PLL bring-up is needed. SYS_PLL2 (528 MHz) would need DIV=1 → 264 MHz,
which is slower for no benefit.

## Driver init contract

`vg_lite_platform.h` declares, and the FreeRTOS HAL implements
(`.../VGLiteKernel/rtos/vg_lite_hal.c:59-68`), a pure setter — it stores four
file statics that every other HAL function then reads:

```c
void vg_lite_init_mem(uint32_t register_mem_base,   /* -> registerMemBase */
                      uint32_t gpu_mem_base,        /* -> gpuMemBase      */
                      volatile void *contiguous_mem_base, /* -> contiguousMem */
                      uint32_t contiguous_mem_size);      /* -> heap_size     */
```

The bare-metal port must honour the same contract: **do not hardcode
`0x4180_0000` in the HAL** — the application passes it in, which is what keeps
the port board-agnostic. `vg_lite_hal_peek/poke` address relative to
`registerMemBase`.

Upstream's default `registerMemBase` is `0x40240000` (an RT500 address) with
`0x43c80000` under `_BAREMETAL` (an FPGA address) — **neither is right for the
RT1176**, which is precisely why the value is passed in.

## ★ `_BAREMETAL` is not the bare-metal path

Upstream's `_BAREMETAL 1` hardcodes the FPGA register base above, calls
Xilinx's `Xil_DCacheFlush()`, and still declares `int_queue` as a FreeRTOS
`xSemaphoreHandle`. It is Xilinx FPGA scaffolding. Do not enable it.

## Cache

No maintenance needed. The `imxrt1176` core never writes `SCB_CCR`, so the
D-cache is off and CPU/GPU views of memory agree; `vg_lite_hal_barrier()`
needs only `__DSB()`. This is the opposite of the rt1062 situation — do not
port that cache handling here.

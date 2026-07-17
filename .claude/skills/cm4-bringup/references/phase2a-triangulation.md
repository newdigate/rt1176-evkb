<!-- Archived 2026-07-17 by /cm4-bringup Phase 2A. 4-reader triangulation
     (SDK cm4 startup/system/linker, RT1170 RM, teensy4/imxrt1176/Zephyr
     idioms) driving the CM4 startup+linker. See cm4-roadmap.md. -->

# Phase-2A CM4 Startup + Linker — Synthesis

Derived from the three reader reports (SDK startup/system/linker, RM Rev.5, and the teensy4 / imxrt1176 / Zephyr idioms). Everything below is consistent across all three sources; where a source is the sole authority I say so.

---

## 1. CM4 memory layout to use

### MEMORY regions (linker)

| Region | ORIGIN | LENGTH | What lands here |
|---|---|---|---|
| `m_interrupts` (RX) | `0x1FFE0000` | `0x400` | `.isr_vector` / `__isr_vector` — 256 words (`0x400` = m_interrupts length exactly) |
| `m_text` (RX) | `0x1FFE0400` | `0x1FC00` | `.text`, `.rodata`, and the **`.data` LMA** (`AT(__etext)`) |
| `m_data2` (RW) | `0x20000000` | `0x20000` | `.data` VMA, `.bss`, `.stack` — **this is where we re-route** |

`m_interrupts + m_text = 0x1FFE0000..0x1FFFFFFF` = the full **128 KB CM4-private ITCM** (Code TCM / LMEM RAM_L). `m_data2 = 0x20000000..0x2001FFFF` = the **128 KB CM4-private DTCM** (System TCM / LMEM RAM_U). Confirmed by two independent RM chapters — memory map `rm_full.txt:2220` (ITCM 0x1FFE0000 128KB) / `rm_full.txt:2219` (DTCM 0x20000000 128KB), and the LMEM chapter `rm_full.txt:151210-151211` — and by the SDK linker `MIMXRT1176xxxxx_cm4_ram.ld:50-53`, and by Zephyr `nxp_rt11xx_cm4.dtsi` (sram0@1ffe0000=128K, sram1@20000000=128K).

### Section placement (the critical deviation from the SDK example)

```
ENTRY(Reset_Handler)                          # cm4_ram.ld:29

.isr_vector   -> m_interrupts   (VMA=LMA @ 0x1FFE0000)
.text/.rodata -> m_text         (VMA=LMA, ITCM)
__etext = .;                    # end of code in ITCM
.data : AT(__etext) {           # LMA in ITCM, right after code
  __data_start__ = .;           # VMA in DTCM
  ...
} > m_data2                     # <-- re-routed to DTCM (was > m_data / SDRAM)
.bss  { ... } > m_data2         # <-- re-routed to DTCM
.stack { ... } > m_data2        # <-- re-routed to DTCM
__StackTop = ORIGIN(m_data2) + LENGTH(m_data2);   # = 0x20020000
```

**Why this deviation is mandatory.** The supplied SDK linker is the mpp/eiq **RPMSG** example: it routes `.data`/`.bss`/`.stack` to `m_data`, an **SDRAM-derived** region (`DATA_START = 0x80000000 + 0x04000000 - DATA_SIZE`, `cm4_ram.ld:42`), and declares `m_data2` (DTCM) but **assigns no output section to it** (`.data/.bss/.stack all `> m_data``, `cm4_ram.ld:192/227/245/248`). With no `__use_shmem__`, `DATA_SIZE=0` and `m_data` underflows — it is an SDRAM-data multicore linker, **not** a TCM-stack linker (SDK report `disagreements_or_gaps[0]`). For Phase-2A we must:
- route `.data`/`.bss`/`.stack` to `m_data2`,
- set `__StackTop = 0x20000000 + 0x20000 = 0x20020000` (matches the HW-verified Phase-1 `SP=0x20020000`),
- **keep `.data` LMA in ITCM** (`AT(__etext)`) so the CM7 stages **one contiguous ITCM image** and the CM4 `Reset_Handler` copies `.data` ITCM→DTCM and zeroes `.bss` in DTCM (SDK report finding 5, "KEY caveat").

Do **not** copy the SDK's default parametrics unchanged (`STACK_SIZE`/`HEAP_SIZE` = 0x400 each, `cm4_ram.ld:33`; a small heap is fine but the stack lives at the top of DTCM).

The idiom report flags the single most likely port bug: our existing CM7 linker sets `_estack = ORIGIN(DTCM) + (8<<15) = 0x20040000` (`imxrt1176.ld:130`). The CM4 DTCM is only 128K, so reusing that value pushes to `0x20040000` — **outside CM4 RAM → bus fault on the first push/interrupt frame**. The CM4 linker MUST cap `_estack`/`__StackTop` at `0x20020000`.

### Line-18757 ambiguity — RESOLVED, it is a DIFFERENT TABLE, not a contradiction

`rm_full.txt:18757` reads `1  M4 ITCM + M7  0x2007FFFF  DTCM 0x1FFE0000  640 KB`. This is **Table 10-64 "Allowable address range for AHAB"** (`rm_full.txt:18754`) — a list of contiguous regions a secure-boot (AHAB) image is *allowed to load into*, **not a memory map**. Two text-extraction artifacts create the false alarm: (a) the Start/End columns are reversed, and (b) the region name `M4 ITCM + M7 DTCM` wraps, so "DTCM" spills into the End-address cell. The real row is region **"M4 ITCM + M7 DTCM"**, span `0x1FFE0000..0x2007FFFF` = `0xA0000` = **640 KB = 128 KB (M4 ITCM @0x1FFE0000) + 512 KB (M7/CM7 DTCM FlexRAM @0x20000000)** — cross-checked against `rm_full.txt:2134` (CM7 512KB DTCM). It does **not** place the *M4* DTCM at 0x1FFE0000 and does **not** contradict the 0x1FFE0000-ITCM / 0x20000000-DTCM view (RM report finding 4 + `disagreements_or_gaps[1]`). **Use it for nothing in the linker except confirming 0x1FFE0000 = M4 ITCM start.**

---

## 2. Minimal CM4 ResetHandler

The SDK's own CM4 `SystemInit` is the authority on "minimal": it does **no** clock/PLL/FlexRAM/DCDC/SDRAM work (`system_MIMXRT1176_cm4.c:79-172`, SDK finding 3). Zephyr's CM4 path independently confirms this — `soc_reset_hook` skips `flexram_dt_partition()` (guarded by `FLEXRAM_RUNTIME_BANKS_USED`, not set for CM4) and the CM4 `clock_init` only re-muxes the M4 root, never touching OSC24M/ARM-PLL/DCDC (`soc.c:875-883`, `soc.c:327-332`, idiom finding "(b) DROP confirms").

### KEEP — the whole smallest reset sequence (ordered)

1. **Set MSP = `__StackTop` (`0x20020000`)** — belt-and-suspenders; the SRC/LPSR_GPR backdoor already loads `SP=word0`, but set it explicitly. (`startup_MIMXRT1176_cm4.S:305-306`; idiom `imxrt1176/startup.c:176`.)
2. **Set `SCB->VTOR`** (see "VTOR set to what" below). (`startup_MIMXRT1176_cm4.S:301-304`; idiom `imxrt1176/startup.c:206`.)
3. **Enable FPU** — `SCB->CPACR |= (0xF<<20)` (CP10/CP11 full access). **Required** (see below). (idiom `imxrt1176/startup.c:179`; teensy `startup.c:133`; Zephyr `z_arm_floating_point_init`, `prep_c.c:106/109`.)
4. **Copy `.data` LMA(ITCM)→VMA(DTCM)** — `__etext → __data_start__..__data_end__`. (`startup_MIMXRT1176_cm4.S:320-343`.)
5. **Zero `.bss`** — `__bss_start__..__bss_end__`. **Must be guaranteed**: in the SDK asm this is gated on `#ifdef __STARTUP_CLEAR_BSS` (`startup_MIMXRT1176_cm4.S:401`); if you don't run a full newlib crt0, you MUST define `__STARTUP_CLEAR_BSS` or clear `.bss` inline (SDK `disagreements_or_gaps[2]`).
6. **`__libc_init_array()` then `main()`** — keep `__libc_init_array` for C++ ctor/`init_array` parity even if empty. (idiom `imxrt1176/startup.c:270-271`; can be a no-op for pure-C leaf, idiom `disagreements_or_gaps`.)

That is the complete minimal `ResetHandler` (idiom finding "(a) KEEP the minimal correct set").

### DROP — the CM7/ROM already did all of it

- **FlexRAM bank config** (GPR16/17/18/14) — the CM4 TCM is **fixed LMEM** (RAM_L/RAM_U), *not* FlexRAM banks; those IOMUXC_GPR regs are the CM7 domain and must not be touched from CM4. (idiom `imxrt1176.ld:120-130`, `soc.c`; RM finding 2 — CM4 = LMEM, CM7 = FlexRAM, `rm_full.txt:81904-81905`, no bank-config knob.)
- **DCDC VDD1P0 OverDrive** — supply already raised by CM7. (idiom `imxrt1176/startup.c` DCDC step.)
- **ARM-PLL / OSC24M / `set_arm_clock`** — CM4 runs off `CLOCK_ROOT1` (M4 root), ROM-set to `0x201` (≈200 MHz off OSC_RC_400M ÷2). (RM finding on CLOCK_ROOT1, `rm_full.txt:84801/86443`; idiom DROP list.)
- **`semc_sdram_init` + `.bss.extram` zero** — a TCM-resident CM4 image has no EXTMEM section. (idiom.)
- **`SRC_SRMR` M4-lockup hygiene** — that is an M7-role write *about* the CM4, nonsensical from the CM4. (idiom `imxrt1176/startup.c:161`.)
- **`.text→ITCM` self-copy** — the CM7 stages code+vectors already resident in TCM; only `.data`/`.bss` init remain. (idiom finding "(a) VTOR + .text copy"; keep a `.text` copy *only* if you later choose a distinct `.text` LMA — we do not.)
- **SDK `SystemInit` extras**: WDOG1/2/RTWDOG3/4 disable, SysTick disable, LMEM PCCCR/PSCCR cache enable, GPR28 CACHE_USB clear, SRSR/ECC save, DIV_0_TRP — all optional for a bare-bones boot (SDK finding 5). Drop them for Phase-2A.

### The three explicit questions

- **FPU enable needed?** **YES.** The CM4 is a Cortex-M4**F** (FPv4-SP, `FPU_PRESENT=1`, `rm_full.txt:150744`, `150835-150836`; `nxp_rt11xx_cm4.dtsi` `cortex-m4f`). C compiled with `-mfpu=fpv4-sp-d16 -mfloat-abi=hard` emits FP in the prologue/FP-stacking; with CPACR clear the first FP op raises **NOCP UsageFault → HardFault**. The Phase-1 leaf blob never touched FP, so this is genuinely new (idiom finding "FAULT risk: FPU not enabled"; RM finding 5 — the CPACR step is the CMSIS/ABI contract, not an RM register). Alternatively build soft-float to avoid it, but hard-float + CPACR is the idiom.
- **`SCB->VTOR` set to what?** To the **CM4's own vector base**. Two equivalent choices, *same physical memory*: the CM4-private link address **`0x1FFE0000`** (what the SDK asm writes, `startup_MIMXRT1176_cm4.S:301-304`; note `SystemInit`'s VTOR write is `#if defined(__MCUXPRESSO)` only, so a plain GAS build's runtime VTOR comes solely from the asm — SDK finding 3b + `disagreements_or_gaps[3]`) **or** the system alias `0x20200000`. Both point at the same vectors. **Distinct from the boot VTOR**: the value the CM7 hands to the SRC via LPSR_GPR MUST be the **system alias `0x20200000`** — `0x1FFE0000` as the *staged boot* VTOR does **not** boot (Phase-1 HW fact; RM finding 3 — `0x1FFE0000` isn't even in the CM7 map, `rm_full.txt:2135`). Recommendation: set runtime `SCB->VTOR = 0x1FFE0000` (the link base) to match the SDK/idiom pattern.
- **`SystemInit()` needed?** **Not as a function.** The only genuinely-required piece it carries is the FPU CPACR enable — fold that into `ResetHandler` (step 3). Everything else `SystemInit` does is either DROP-list SoC bring-up (which the CM4 version already omits) or optional hygiene (WDOG/SysTick/cache/ECC). You can ship Phase-2A with **no `SystemInit` at all**, or a stub. `SystemCoreClockUpdate()` is a "TBD" empty stub even in the SDK (`system_MIMXRT1176_cm4.c:180`).

---

## 3. Risk triggers → probes

Enumerating what a real compiled CM4 image newly depends on that the **Phase-1 leaf blob (looped on MU regs only, SP set but no push, PC=0x1FFE0009)** did NOT exercise. "Covered" = proven by the Phase-1 HW run; "NEW" = needs qemu2 modelling and/or an EVKB probe.

| # | New dependency | Phase-1 coverage | Obligation |
|---|---|---|---|
| A | **CM7-stages / CM4-fetches via the ITCM alias** (CM7 writes `0x20200000`, CM4 fetches `0x1FFE0000`) | **COVERED (HW)** — the blob booted at PC=0x1FFE0009 at all, which *only* works if the CM7's backdoor write appeared at the CM4-private ITCM. Proven for the first ~0x400. | HW: none for small region. **qemu2: MUST verify aliasing** (see below). |
| B | **ITCM code fetch of a *larger* image** (`.text` spans `0x1FFE0400+`, not just the first word) | **PARTIAL** — only the first few words fetched in Phase-1. | Low HW risk (same physical 128K array); a real multi-KB image is the first real test. qemu2: ensure the whole 128K ITCM alias is backed, not just page 0. **Probe:** run code that lives past `0x1FFE0400`. |
| C | **DTCM read/write via the CM4-private view `0x20000000`** (`.data` VMA, `.bss`, and **stack pushes**) | **NOT COVERED** — Phase-1 set SP but was a leaf loop; no push/store to DTCM. | **NEW HW probe + qemu2.** First actual stores/loads at CM4 `0x20000000`. Must confirm the alias is writable from the CM4 side and readable back. |
| D | **FPU (CPACR CP10/CP11)** | **NOT COVERED** — blob had no FP, never touched CPACR. | **NEW.** qemu2: model CPACR + FPv4-SP so an FP op doesn't fault (or trapped correctly if CPACR clear). **Probe:** one hard-float multiply reaching main. |
| E | **Runtime `SCB->VTOR` relocation** (CM4 writes VTOR to `0x1FFE0000`, distinct from the LPSR_GPR boot vector `0x20200000`) | **NOT COVERED** — Phase-1's VTOR was only the LPSR_GPR boot vector; the CM4 never re-wrote `SCB->VTOR`. | **NEW**, but low HW risk (architectural). qemu2: honor a CM4 `SCB->VTOR` write and dispatch a fault/IRQ from it. |
| F | **`.data` copy correctness end-to-end** (LMA in ITCM, VMA in DTCM — i.e. read ITCM, write DTCM in the same routine) | NOT COVERED | Emergent from B+C. Covered once B and C pass. |

### The central qemu2 question — TCM-view aliasing

**Does qemu2 model the CM4-private `0x1FFE0000`/`0x20000000` views *distinctly from* the system alias `0x20200000`/`0x20220000`, backed by the SAME RAM?**

The RM is explicit that these are **one physical memory** seen from two sides: `0x2020_0000-0x2023_FFFF` = "OCRAM M4 (LMEM 128KB SRAM_L + 128KB SRAM_U backdoor)" (`rm_full.txt:2130`), footnote 2 "remapping address for CM4 TCM ... CM7 can access CM4 TCM through this aliased region" (`rm_full.txt:2234/2149`). So:
- **ITCM alias**: CM4-private `0x1FFE0000` ⇄ system `0x20200000` (`0x20200000-0x2021FFFF`, SRAM_L = RAM_L = Code TCM).
- **DTCM alias**: CM4-private `0x20000000` ⇄ system `0x20220000` (`0x20220000-0x2023FFFF`, SRAM_U = RAM_U = System TCM). (RM finding 3.)

**Failure mode if qemu2 does NOT alias them.** The CM7 stages the image by writing the **system alias** (`0x20200000` for code+vectors; the CM7-side stager). The CM4 then **fetches/reads from its private view** (`0x1FFE0000` / `0x20000000`). If qemu2 backs the `0x20200000` backdoor with a *separate* RAM block from the CM4-private `0x1FFE0000`/`0x20000000`, then:
- the CM7's staged bytes never appear at the CM4's fetch/load addresses → **garbage fetch → lockup**, and specifically
- **`.data`/`.bss`/stack (risk C) is where the gap first bites**: even if code fetch (A) happened to work in Phase-1 because that path was modeled, a real image's DTCM traffic at `0x20000000` may hit an unmodeled/aliased-elsewhere region.

Because Phase-1 booted, qemu2 must *already* map the ITCM path such that the CM4 sees the CM7-written blob (A is implicitly modeled for ITCM). But **whether the DTCM private view `0x20000000` is aliased to the `0x20220000` backdoor — and whether the ITCM alias covers the full 128K, not just the boot region — is unverified by Phase-1** and is the specific new qemu2 obligation. Implement these as `memory_region_init_alias` over a single backing RAM per TCM (one for ITCM, one for DTCM), each exposed at both the private and system-alias base; then a real image exposes no gap.

> Caveat (from idiom `disagreements_or_gaps`): the reports did not open the Phase-1 qemu2 machine source, so the current aliasing state is unknown from here — **verify it directly in the qemu2 mimxrt1170 model before the first gate run.** This is the highest-leverage check.

---

## 4. Open questions for the implementer / gate design

### How the CM4 signals liveness
Reuse the **MU heartbeat** the Phase-1 blob already exercised (it "looped on MU regs" — that path is HW-proven). The CM4 writes an incrementing/known token to an MU transmit register; the CM7 polls the matching MU receive register. This is the one CM4→CM7 channel already known-good, so it's the safe carrier for all gate assertions below. (RM/idiom: MU is the established IPC; the boot/release + MU mechanism matches Zephyr `soc.c:901-902` SRC release and our HW-verified `Multicore.begin()`.)

### What the gate MUST assert to prove .data/.bss/stack actually worked
A bare "MU heartbeat fires" only proves the core booted and reached *some* code — it does **not** prove the C runtime init. Design the CM4 sketch so the heartbeat payload is a value that is **only correct if each init step ran**:

1. **`.data` copied (ITCM→DTCM):** declare a global initialized to a distinctive non-zero magic, e.g. `volatile uint32_t g_data_magic = 0xC0DE2A17;`, placed in `.data`. CM4 sends `g_data_magic` over MU. It reads back correct **only if** `Reset_Handler` copied `.data` LMA(ITCM)→VMA(DTCM); if the copy was skipped, the DTCM slot holds garbage/zero. This directly exercises risk C + F.
2. **`.bss` zeroed:** declare `volatile uint32_t g_bss_counter;` (zero-init, in `.bss`). CM4 sends its value **before** writing it — the gate asserts it reads `0`. If `.bss` wasn't zeroed (the `__STARTUP_CLEAR_BSS` trap, §2 step 5), it starts non-zero and the assert fails.
3. **Stack in backed DTCM:** force real stack traffic — a small nested / non-inlined function chain that spills registers, or compute a checksum in a local array. If `_estack` were the CM7 `0x20040000` value (the §1 fault trap), the first deep push bus-faults before the heartbeat. Passing the heartbeat *after* a genuine stack push proves the DTCM stack at `0x20020000` is backed and writable (risk C).
4. **FPU (fold into the same payload):** do one hard-float op, e.g. `g_result = (float)g_data_magic * 1.5f;` and send `g_result`'s bit pattern. Correct value proves CPACR was enabled (risk D); a NOCP fault would kill the heartbeat instead.

**Recommended single combined assertion:** CM4 computes `token = checksum(.data array) ^ (g_bss_counter==0 ? 0 : 0xBAD) ^ float_op_result` and sends it. One correct token over MU proves **data-copy + bss-zero + FPU + stack** in one shot; any single failure changes the token or suppresses the heartbeat entirely.

### Open questions to settle before/at implementation
- **qemu2 TCM aliasing (blocking):** confirm/implement that CM4-private `0x1FFE0000`(ITCM) and `0x20000000`(DTCM) alias the same backing RAM as the system `0x20200000`/`0x20220000` backdoor, full 128K each. Without this, the gate can't model a real staged image (§3). *Verify the actual qemu2 source — not assumed from the reports.*
- **Runtime `SCB->VTOR` target:** `0x1FFE0000` (link base, SDK/idiom default) vs `0x20200000` (system alias). Both physically valid; the reports call this an inference not independently HW-verified for the running CM4 (idiom `disagreements_or_gaps[3]`). Pick `0x1FFE0000` for idiom parity, but the gate should also fire a CM4 IRQ/fault to prove VTOR relocation actually dispatches (risk E), since Phase-1 never did.
- **`.text` LMA model:** we drop the `.text`→ITCM self-copy (code staged resident). Confirm the CM4 linker keeps `.text` VMA=LMA in ITCM and only `.data` carries a distinct LMA (idiom `disagreements_or_gaps[0]`) — otherwise a missing `.text` copy would leave code uninitialized.
- **`__libc_init_array` necessity:** no-op for a pure-C leaf; keep it for C++/ctor parity (idiom `disagreements_or_gaps`). Not a gate risk, but decide before writing the sketch.

---

### Source note
All citations above are as given in the three reader reports: SDK = `startup_MIMXRT1176_cm4.S` / `system_MIMXRT1176_cm4.c` / `MIMXRT1176xxxxx_cm4_ram.ld`; RM = `/Users/nicholasnewdigate/Development/rt1170/rm_full.txt`; idiom = `evkb/cores/teensy4/startup.c`, `evkb/cores/imxrt1176/startup.c`, `imxrt1176.ld`, and the Zephyr `nxp_rt11xx_cm4.dtsi` / `soc.c` / `reset.S` / `prep_c.c` / `sysbuild.cmake` / `cm4-roadmap.md`. No file needed to be opened to synthesize this — it is a pure reconciliation of the provided findings, and all three sources agree on the KEEP/DROP split and the DTCM-stack re-route.

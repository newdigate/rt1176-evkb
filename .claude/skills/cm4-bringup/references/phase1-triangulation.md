<!-- Archived 2026-07-17 by /cm4-bringup Phase 1.  Produced by a 4-reader
     triangulation workflow (MCMGR, fsl_mu, Zephyr, SDK example) reconciled
     against the RM and the dualcore_mu_test HW transcript (2026-07-16).
     Drives the Phase 1 implementation; see cm4-roadmap.md. -->

# CM4 Bring-Up (Phase 1) — Reference Reconciliation

Four reader reports (`mcmgr`, `fslmu`, `zephyr`, `sdkExample`) triangulated against the HW-verified `evkb/dualcore_mu_test` silicon transcript (2026-07-16). Citations are `file:line` from the reports; silicon facts are quoted as transcript tokens.

---

## 1. Consensus (confirmed facts)

| # | Fact | Backing sources |
|---|------|-----------------|
| C1 | **Boot VTOR = system address `0x20200000`** (the OCRAM/backdoor alias of CM4 ITCM), *not* the CM4-private `0x1FFE0000` alias. | mcmgr `app.h:20`; zephyr `soc.c:812` + `nxp_rt11xx.dtsi:976`; sdkExample `app.h:15`. fslmu abstains (out of driver scope). Matches transcript: *"boot VTOR = 0x20200000 (SYSTEM addr, NOT the CM4-private 0x1FFE0000 alias which does not boot)."* |
| C2 | **GPR0 = VTOR[15:3]** = `((VTOR>>3)<<3)&0xFFF8`; for `0x20200000` → **`0x0000`**. | mcmgr `PERI_IOMUXC_LPSR_GPR.h:215-218` / call `:116`; zephyr `soc.c:814`; sdkExample `:116`. Matches *"GPR0 = CM4_INIT_VTOR bits[15:3] (VTOR & 0xFFF8)."* |
| C3 | **GPR1 = VTOR[31:16]**; for `0x20200000` → **`0x2020`**. Reconstructed VTOR `(GPR1<<16)|(GPR0&0xFFF8) = 0x20200000`. | mcmgr `PERI_IOMUXC_LPSR_GPR.h:244-247` / `:117`; zephyr `soc.c:815`; sdkExample `:117`. Matches *"GPR1 = VTOR>>16."* |
| C4 | **Release order:** GPR0/GPR1 first → `SRC->CTRL_M4CORE = SW_RESET` (plain `=`, self-clearing) → `SRC->SCR \|= BT_RELEASE_M4`. SW-reset **before** boot-release. | mcmgr `:119-120`; zephyr `soc.c:901-902`; sdkExample `:119-120`. Matches transcript SRC bit map (`SCR bit0 = BT_RELEASE_M4`, `CTRL_M4CORE@0x284 bit0 = SW_RESET self-clearing`). |
| C5 | **MU register layout (classic):** `TR[4]@0x00` (WO), `RR[4]@0x10` (RO), `SR@0x20`, `CR@0x24`. No split GSR/GIER/TCR/RCR, no VERID/PARAM. | fslmu `PERI_MU.h:167-172`. Matches transcript offsets exactly. |
| C6 | **MU bit positions & `n=0` is the HIGH-order bit** of each 4-bit field. SR: `GIP[31:28] RF[27:24] TE[23:20] FUP8 RS7 EP4 Fn[2:0]`; CR: `GIE[31:28] RIE[27:24] TIE[23:20] GIR[19:16] MUR5 Fn[2:0]`. Ch0 flags: Tx0Empty=bit23, Rx0Full=bit27, GenInt0=bit31, GenInt0Trigger(GIR)=CR bit19. | fslmu `PERI_MU.h:204-304`, `fsl_mu.h:79-82,89,161,455-460`; zephyr `fsl_mu.h:38-41` + `mbox_nxp_imx_mu.c:29-32`. Matches transcript SR/CR field map verbatim. |
| C7 | **GIP is W1C; GIR auto-clears on the peer's GIP ack.** Sender sets `CR.GIRn`; receiver acks via `SR = mask & MU_SR_GIPn_MASK`, which HW-clears the sender's `CR.GIRn`. `MU_TriggerInterrupts` returns `kStatus_Fail` if GIR still set. | fslmu `fsl_mu.h:531-552`, `fsl_mu.c:317-330`; zephyr mbox `mbox_nxp_imx_mu.c:200-201`. Matches *"GIP is W1C; acking the other side's GIP auto-clears this side's GIR."* |
| C8 | **MUA=`0x40C48000` (CM7=Processor A), MUB=`0x40C4C000` (CM4=Processor B).** CM4-launch branch gated by `FSL_FEATURE_MU_SIDE_A`; targets `kMCMGR_Core1`. | mcmgr `:28-30,114-115`; fslmu `MIMXRT1176_cm7_COMMON.h:1856` / `cm4:1875`; zephyr `nxp_rt11xx_cm7.dtsi:114` / `cm4.dtsi:72`; sdkExample `:114-137`. Matches transcript. |
| C9 | **No reference writes the CM4 clock gate in the release path.** All rely on boot-ROM default LPCG state; Zephyr only sets the M4 *root* clock (`kCLOCK_Root_M4`), never LPCG1. | mcmgr `no-clock-gate-no-GPC` (zero CCM/LPCG refs); zephyr `soc.c:900-902` + `:213-215`; fslmu (all clock helpers gated out); sdkExample. **Transcript still names `CCM LPCG1 DIRECT @0x40CC6020` as *the* M4 gate** — see R1. |
| C10 | **No stop/hold path exists in any reference.** MCMGR `mcmgr_stop_core_internal` → `kStatus_MCMGR_NotImplemented`; Zephyr has only a one-way start. | mcmgr `:163-167`; sdkExample `:163-166`; zephyr `No stop/shutdown path`. `dualcore_mu_test` drives `SRC CTRL_M4CORE` bit0 directly for reset. |
| C11 | **MU NVIC IRQ = 118** for both instances (single-source, uncontradicted). | zephyr only: `nxp_rt11xx_cm7.dtsi:117`, `cm4.dtsi:75`. Not stated by transcript → verify (see R5). |

---

## 2. Disagreements = HARDWARE-PROBE TRIGGERS

**D1 — SCR write style: `|=` (RMW) vs "write-1-only".**
`mcmgr:120` and `zephyr soc.c:902` both use `SRC->SCR |= BT_RELEASE_M4`; the transcript characterizes `SCR bit0 BT_RELEASE_M4` as **write-1-only**. Both set bit0; the discrepancy is behavioral: if bit0 is truly write-1-only, it **cannot be de-asserted by writing 0**, so `|=` is safe but you can never "un-release."
**Resolved by transcript** (token: *"bit0=BT_RELEASE_M4 (write-1-only per silicon)"*). Not a new HW probe, but a **QEMU-model fidelity requirement**: model `SCR.BT_RELEASE_M4` as W1S (set-only, no clear). This is the root reason stop/re-hold must route through `CTRL_M4CORE.SW_RESET`, not `SCR`.

**D2 — Reset/hold status: MCMGR reads MU `CR` bit7; fslmu locates RS at `SR` bit7; silicon says RS never sets.**
`mcmgr:212-222` reports `InReset` from `MUA->CR & (1<<7)` — its own comment says "MU_SR" but the **code reads CR** (CR bit7 is undefined). fslmu places RS at `SR` bit7 (`0x80`) and notes its only consumer (`MU_ResetBothSides`) is compiled out (`NO_MUR=1`). Triple divergence.
**Resolved by transcript** (tokens: *"ASR.RS(bit7) never sets"*, *"hold indicator = SRC STAT_M4CORE bit0 (1 held / 0 released)"*). **Do NOT mirror MCMGR's status logic.** Library must sense `SRC STAT_M4CORE bit0` (`@0x40C04290`, mask `0x1`); QEMU model must implement that bit as the held/released indicator. Not a new probe — but a hard "avoid the reference bug" note.

**D3 — Undocumented `ASR bit9 always 1`.**
No reader report mentions MU SR bit9; the transcript asserts *"undocumented ASR bit9 always 1."*
**Resolved by transcript** (already HW-verified). **QEMU-model requirement:** the MU SR read must return bit9=1; the library must mask bit9 off before comparing raw SR against expected flags. Not a HW probe, but a silicon-only fact absent from all four SDK sources — easy to miss when copying `fsl_mu.h` masks.

**D4 — Start sequencing: one-shot (MCMGR) vs two-phase (Zephyr).**
MCMGR does GPR+SW_RESET+BT_RELEASE inside one `MCMGR_StartCore` (`:116-120`). Zephyr splits VTOR+image-copy at `PRE_KERNEL_1` (`soc.c:810-815`) from SW_RESET+BT_RELEASE at `PRE_KERNEL_2` (`soc.c:901-902`). **Not a HW contradiction** — relative order within is identical (GPR→SW_RESET→BT_RELEASE) and the only invariant is *image staged + GPR0/1 set before BT_RELEASE*. `dualcore_mu_test` does it inline (one-shot). Not a probe; an ordering constraint to preserve.

**D5 — Boot "I'm alive" handshake mechanism differs across references.**
Three distinct patterns, all HW-real:
- **Fn flag field** (Zephyr default IPM driver, `nxp,imx-mu`): CM4 `MU_SetFlags(CR.Fn=1)`, CM7 polls `SR.Fn` (`soc.c:72,819-820,907`). Simplest, no interrupt.
- **GenInt GIR/GIP** (Zephyr mbox driver, present but *not bound* by default DT; and MCMGR's `TriggerEvent`): `MU_TriggerInterrupts(CR.GIR)` + `MU_ClearStatusFlags(SR.GIP W1C)` (`mbox_nxp_imx_mu.c:70,200-201`).
- **TR/RR data + two-phase CoreUp/FeedStartupData** (MCMGR contract): `mcmgr.c:206-221, 228-256`.

**Design choice, not a contradiction.** Whichever `dualcore_mu_test` proved is the Phase-1 pattern; the transcript's full SR/CR/TR/RR/GIP/GIR surface is HW-exercised. **QEMU-model implication:** if the library uses GenInt, the model MUST implement the cross-core GIP-ack→GIR-auto-clear coupling (C7), or `MU_TriggerInterrupts` will wedge on `kStatus_Fail`.

**D6 — LPCG1 clock gate: transcript names it, all references skip it.** *(borderline NEW probe)*
Every reference relies on boot-ROM default (C9), but the transcript lists *"M4 clock gate = CCM LPCG1 DIRECT @0x40CC6020"* as a distinct fact — implying `dualcore_mu_test` at least references/writes it. What is **not** confirmed: whether our **Arduino-core startup** leaves LPCG1 in the same state the NXP boot ROM assumes. **NEW verification candidate:** confirm the CM4 LPCG1 gate state after our `startup.c` runs; QEMU gate should assert that releasing with LPCG1 gated leaves the CM4 dead (negative control). Likely covered defensively by the transcript, but our startup path is a different code base — pin it down.

**D7 — Restart with a new VTOR: unverified by any source.** *(NEW probe)*
MCMGR models "restart" as calling `StartCore` again → re-pulse `CTRL_M4CORE.SW_RESET` (`stop-core` note). Because `BT_RELEASE` is write-1-only (D1), a restart never de-asserts release; it relies on the SW_RESET pulse re-fetching VTOR from GPR0/1. **Neither any reference nor the transcript confirms that re-pulsing `CTRL_M4CORE.SW_RESET` after reprogramming GPR0/1 actually restarts the CM4 at the *new* address.** NEW probe candidate for both silicon and the QEMU model.

**D8 — MCMGR `SRC->SCR |=` preserves other SCR bits; plain `=` on CTRL_M4CORE clobbers.**
All three code sources agree `CTRL_M4CORE` is written with plain `=` of the mask (`0x1`) while `SCR` uses `|=`. No inter-source conflict. Model note: `CTRL_M4CORE` plain-write of `0x1`, bit0 self-clears; other bits reserved.

---

## 3. `fsl_mu` API shape to mirror

**Variant: RT1176 uses the CLASSIC `mu` driver, NOT `mu1`.** Confirmed by `fslmu driver-flavor-selection` (`Kconfig.chip:315` declares only `MCUX_HW_IP_DriverType_MU`, zero `MU_1` hits; `FSL_MU_DRIVER_VERSION 2.3.3`) and corroborated by zephyr (`mbox_nxp_imx_mu.c:29-32` remap note). The `mu1` split-register layout (`TSR/TCR/RSR/RCR/GSR/GCR/GIER`, `MU_TriggerGeneralPurposeInterrupts`, **opposite** bit ordering 0→3) does **not** apply — do not copy any `mu1` masks.

**Register struct to expose (single SR + single CR):**
```
TR[4] @0x00 (__IO, WO)   RR[4] @0x10 (__I, RO)   SR @0x20 (__IO)   CR @0x24 (__IO)
```
`PERI_MU.h:167-172`. `MU_TR_COUNT = MU_RR_COUNT = 4`.

**Bit macros (n=0 = high-order bit; `1 << (SHIFT + (3 - n))`):**
- SR: `Fn` mask `0x7` sh0; `EP` bit4 `0x10`; `RS` bit7 `0x80`; `FUP` bit8 `0x100`; `TEn` sh20 `0xF00000`; `RFn` sh24 `0xF000000`; `GIPn` sh28 `0xF0000000`.
- CR: `Fn` mask `0x7` sh0; `MUR` bit5 `0x20`; `GIRn` sh16 `0xF0000`; `TIEn` sh20; `RIEn` sh24; `GIEn` sh28.
Source: `PERI_MU.h:204-304`, `fsl_mu.h:79-82,89,161`.
**Reconciliation note:** the transcript labels `SR@0x20` broadly "W1C," but only the `GIPn` nibble is W1C — `TE/RF/EP/FUP/RS` are HW-cleared (`fsl_mu.h:531-552`). Expose `MU_ClearStatusFlags` masking writes to `GIPn` only.

**Ack / trigger semantics (the load-bearing part):**
- Trigger to peer: `CR = (CR & ~(GIRn_MASK | NMI_MASK)) | mask;` guarded by `if (0 == (CR & mask))` else `kStatus_Fail` (`fsl_mu.c:317-330`).
- Ack incoming GP int: `SR = mask & MU_SR_GIPn_MASK;` — this write also HW-clears the sender's `CR.GIRn` (C7).
- **Every CR write must mask out `GIRn` (and NMI)** so enabling an interrupt / setting Fn never accidentally re-triggers (`MU_EnableInterrupts` `fsl_mu.h:571-572`, `MU_SetFlagsNonBlocking`).

**Function surface worth mirroring:** `MU_SendMsg`/`MU_SendMsgNonBlocking` (`fsl_mu.h:236,254`); `MU_ReceiveMsg`/`MU_ReceiveMsgNonBlocking`/`MU_ReceiveMsgTimeout` (`:281,308,328`); `MU_TriggerInterrupts` (`:618`); `MU_GetStatusFlags`/`MU_GetRxStatusFlags`/`MU_GetInterruptsPending` (`:430,470,499`); `MU_EnableInterrupts`/`MU_DisableInterrupts` (`:568,589`); `MU_ClearStatusFlags` (`:531`); `MU_SetFlags`/`MU_SetFlagsNonBlocking`/`MU_GetFlags` (3-bit Fn, `:358,385,395`); `MU_Init`/`MU_Deinit`.

**Do NOT implement (feature-gated OUT on RT1176):** `MU_BootOtherCore`, `MU_HoldOtherCoreReset`, `MU_ResetBothSides`, `MU_HardwareResetOtherCore`, `MU_SetClockOnOtherCoreEnable`, `MU_GetOtherCorePowerMode`, NMI trigger/`MU_ClearNmi`. Gated by `MIMXRT1176_cm4_features.h:843/845/847/839/851/841` (`NO_RSTH/NO_MUR/NO_HR/NO_CLKE/NO_PM/NO_NMI = 1`; `HAS_CCR=0`, so `MU_CR_NMI_MASK` folds to 0). CM4 boot/reset is **SRC-based, outside the MU library** — the MU lib is pure IPC.

---

## 4. Image-embed decision — confirm `cm4_blob[]` for Phase 1

**The SDK scheme is structurally incompatible with an Arduino single-TU build.** `sdkExample arduino-single-cpp-compat` is explicit: NXP requires (a) a separately compiled+linked CM4 ELF with its own CM4 ITCM linker script; (b) a GNU-as `.incbin` of the raw `core1_image.bin` into a `.core1_code` section (`fsl_incbin.S:29-42`); (c) a bespoke CM7 linker region `m_core1_image @0x30FC0000` + `KEEP(*(.core1_code))` (`MIMXRT1176xxxxx_cm7_flexspi_nor.ld:35,47,76-82`); (d) a two-project sysbuild + include path (`sysbuild.cmake:6-16`, `primary/CMakeLists.txt:38-42`). A stock Arduino `.ino/.cpp` + core linker provides none of these.

**Confirmed: the `dualcore_mu_test` pattern — a word-aligned `const uint32_t cm4_blob[]` copied to `0x20200000` — is the correct Phase-1 library pattern.** `sdkExample` reaches the same conclusion unprompted: the Arduino-friendly equivalent is "exactly the user's `dualcore_mu_test` cm4_blob[] C array … Functionally identical (raw blob → memcpy to 0x20200000 → GPR0/GPR1+SRC release); only the packaging differs."

**Phase-1 implementation notes (load-bearing):**
- Blob type must be `uint32_t[]` (or `alignas(4)`) — the copy target holds the CM4 vector table (initial SP + reset PC) and must be 4-byte aligned; size = `sizeof(cm4_blob)` (no linker size symbol needed, unlike the `.incbin core1_image_size` route).
- **Link the CM4 stub at the CM4-private ITCM `0x1FFE0000`** (vectors `0x1FFE0000`, text `0x1FFE0400`, DTCM `0x20000000` — `sdkExample CM4-link-addresses`, `MIMXRT1176xxxxx_cm4_ram.ld:50-53`), but **stage the bytes at `0x20200000` and set VTOR=`0x20200000`.** Both aliases map the same physical CM4 TCM; the vector table's absolute reset pointer stays `0x1FFE04xx` and the CM4 resolves it via its own ITCM. Putting `0x1FFE0000` in GPR0/1 does **not** boot (transcript token) — the VTOR value must be the system address.
- Write GPR0 **unconditionally** even though it computes to `0x0000` for `0x20200000` (C2) — no "skip if zero" optimization.

**Phase-2 (real CM4 image) would need:** a separate CM4 translation unit / ELF with the CM4 ITCM linker script above, `objcopy` to `.bin`, then a **build step that regenerates the `cm4_blob[]` C array** (keeps the Arduino single-binary contract) — or, if leaving single-TU, adopt the `.incbin` `.S` + bespoke CM7 linker region. Recommendation: stay on the C-array route and automate blob regeneration; do not import the two-project sysbuild.

---

## 5. Open risks for the implementer

1. **LPCG1 gate state on our startup path (R1 / D6).** Transcript names `CCM LPCG1 DIRECT @0x40CC6020` as the M4 gate, but all four references rely on boot-ROM default. Confirm the gate state after our Arduino `startup.c` runs; add a QEMU negative-control gate (release with LPCG1 gated → CM4 stays dead). *Likely covered defensively by `dualcore_mu_test`, but our startup differs from the NXP boot ROM — verify, don't assume.*

2. **Hold indicator = `SRC STAT_M4CORE bit0` — never MU RS/CR bit7 (D2).** RS never sets on silicon and MCMGR reads the wrong register anyway. Library senses `@0x40C04290` bit0 (`1`=held/`0`=released); QEMU model must drive that bit from the SW_RESET/BT_RELEASE state machine.

3. **MU SR bit9 always 1 (D3).** Silicon-only, absent from all SDK masks. Model must return bit9=1 on SR reads; library must mask it before flag comparisons.

4. **`BT_RELEASE_M4` is write-1-only / non-clearable (D1).** Model as W1S. Consequence: there is no clean CM4 "un-release" — hold/stop must go through `CTRL_M4CORE.SW_RESET`, and a full re-hold may require a system reset.

5. **MU IRQ number 118 is single-sourced (C11).** Only zephyr DTS asserts it; transcript is silent. Verify against the RT1176 RM / the core's existing NVIC table before wiring interrupt-driven MU. Low risk, load-bearing for any IRQ-driven path.

6. **Restart-with-new-VTOR is unverified (D7).** Re-pulsing `CTRL_M4CORE.SW_RESET` after reprogramming GPR0/1 is assumed (MCMGR's model) but proven by neither reference nor transcript. NEW probe if Phase-1 needs restart; otherwise document as unsupported.

7. **Boot-handshake mechanism selection (D5).** Pick the pattern `dualcore_mu_test` proved (Fn-flag is simplest and interrupt-free per zephyr `soc.c:907`). If GenInt is chosen instead, the QEMU model MUST implement GIP-ack→GIR-auto-clear (C7) or `MU_TriggerInterrupts` wedges on `kStatus_Fail` (`fsl_mu.c:317-330`).

8. **Cache coherency on the staging copy.** Zephyr deliberately copies the image **before** enabling M7 D-cache (`soc.c:807-808`). Our core likely has D-cache already on when we `memcpy` to `0x20200000` — clean/invalidate that range (or map it non-cacheable) after the copy, or the CM4 boots stale bytes. Not exercised by QEMU (no cache model) → **HW-only risk**, must be handled in code and can't be gate-verified.

9. **CM4 vector-table address mixing (Section 4).** The single most likely boot fault: linking the blob at `0x20200000` (instead of `0x1FFE0000`) or setting VTOR to `0x1FFE0000` (instead of `0x20200000`). Either → CM4 hard-fault at reset. Assert both in the build/gate.

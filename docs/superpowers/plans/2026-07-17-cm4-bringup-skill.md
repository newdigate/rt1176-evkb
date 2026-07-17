# cm4-bringup Skill Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Author the `cm4-bringup` methodology-playbook skill (SKILL.md + three reference files + probe-firmware template) in the evkb repo, with a discovery symlink from `rt1170/.claude/skills`.

**Architecture:** A lean rigid-process SKILL.md points at three on-demand reference files (silicon-truth loop, license firewall, living CM4 roadmap) and a buildable probe-firmware template derived from the proven `dualcore_mu_test`/`dcd_test` pattern. Everything versioned in evkb; the outer `rt1170/.claude` gets only a symlink.

**Tech Stack:** Markdown skill files (superpowers writing-skills conventions), GNU ARM assembly (arm-none-eabi at `/Applications/ARM_10/bin`), QEMU `mimxrt1170-evk` machine from `~/Development/qemu2/build`.

**Spec:** `docs/superpowers/specs/2026-07-17-cm4-bringup-skill-design.md`

---

### Task 1: SKILL.md

**Files:**
- Create: `/Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/SKILL.md`

- [ ] **Step 1: Write SKILL.md with exactly this content**

````markdown
---
name: cm4-bringup
description: Use for ANY i.MX RT1176 CM4/dual-core work — extending the imxrt1176 core or Arduino libraries to the CM4, MU/IPC features, CM4 boot/release sequencing, picking up the "next bring-up phase", or work touching the dual-core register surfaces (SRC core-release, MU, IOMUXC_LPSR GPRs, GPC, CM4 TCM/backdoor) in the cores, libraries, or the qemu2 machine model. Enforces the silicon-truth validation loop and the license firewall.
---

# CM4 bring-up (silicon-truth dual-core methodology)

## Why this skill exists

The RT1176 docs lie exactly where it is expensive: the RM and NXP's own
`PERI_MU.h` document MU ASR.RS at bit 7, but silicon never sets it —
and undocumented bit 9 reads 1 in every state, so neither is a hold
indicator (use `SRC STAT_M4CORE` bit 0); `SCR.BT_RELEASE_M4` is
write-1-only; the CM7 FlexRAM fuse default is 256K+256K (the
CM4's own TCMs are a separate fixed 128K+128K), not what
third-party writeups claim; a CM4 VTOR in
the TCM windows does not boot. All of this was measured on the EVKB
(2026-07-16/17, `dualcore_mu_test/transcript_hw_evkb.txt`). This skill
encodes the loop that caught those: triangulate written references, gate
everything in QEMU, and probe real silicon whenever a risk trigger fires.

## Checklist (create a task per item, in order)

1. Read `references/cm4-roadmap.md`; identify the current phase and pick
   the work item. If the roadmap is stale versus reality, fix it first.
2. Triangulate references BEFORE writing code (table below). Disagreement
   between any two references is itself a hardware-probe trigger.
3. Write or extend the QEMU gate for the capability (gate-lib.sh pattern,
   like `evkb/serial_test/run_qemu.sh`) before implementing.
4. Check the risk-trigger table below. If any trigger fires, plan a probe
   (see `references/silicon-truth-loop.md`) before relying on the code.
5. Implement, following `references/license-firewall.md` for any code that
   is adapted or vendored from anywhere.
6. Run the QEMU gate. If qemu2 was touched, also run its regression set
   (listed in `references/silicon-truth-loop.md`) and
   scripts/checkpatch.pl.
7. If a probe was triggered: run it on the EVKB, diff transcripts, and
   apply "silicon wins" (below); if that changed any code, repeat step 6.
8. If any source tree was added or vendored: run
   `evkb/tools/license-audit.sh` and extend its coverage in the SAME
   change (see `references/license-firewall.md`).
9. Update `references/cm4-roadmap.md` (phase status, queued probes, new
   discoveries) and commit before ending the session.

## Reference triangulation

| Source | Where | Use for |
|---|---|---|
| RT1170 RM (text, greppable) | `~/Development/rt1170/rm_full.txt` | register offsets, bit fields, IRQ tables, memory maps |
| NXP SDK source | `~/Development/mcuxsdk-ws/mcuxsdk` (+ `~/Development/mcuxsdk-examples`) | driver sequences, MCMGR/RPMsg protocols, dcd/xmcd, linker layouts |
| Zephyr HAL/soc | `~/Development/zephyr/projects/zepherproject` | second-core boot flow, devicetree facts (MU=IRQ 118), alternative driver shapes |
| Repo gate firmware | `evkb/*/run_qemu*.sh` + sources | known-good bare-metal behavior on this exact machine model |

## Risk-trigger table — these MANDATE an EVKB probe

- undocumented or reserved bits in any register you now depend on
- reset/default values you now depend on
- boot, reset, or core-release sequencing
- memory aliasing, TCM/backdoor windows, or address-map subtleties
- clock or power gating behavior
- any case where two triangulation sources disagree
- any new qemu2 device model, or behavior change to an existing one

## Rules

- **Silicon wins.** A qemu2 change justified by a measurement cites the
  probe and date in a code comment and the commit message. Deliberate
  RM deviations and expected QEMU-vs-silicon divergences are documented,
  never silently absorbed.
- **A capability without a QEMU gate is not done.**
- **GPL one-way firewall:** code from qemu2 (GPL-2.0) never flows into
  firmware repos. Firmware-to-qemu2 is fine.
- **Leave the roadmap true.** Future sessions rely on it, not on memory.

## Read next (load on demand)

- `references/cm4-roadmap.md` — ALWAYS, at session start.
- `references/silicon-truth-loop.md` — before your first probe, any
  hardware run, or any qemu2 model change.
- `references/license-firewall.md` — before adapting or vendoring ANY
  source, or adding a dependency.
- `templates/probe_firmware/` — copy this skeleton to start a new probe.
````

- [ ] **Step 2: Verify frontmatter and required sections**

Run: `head -5 /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/SKILL.md && grep -c "^## " /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/SKILL.md`
Expected: first line `---`, `name: cm4-bringup` on line 2, and section count `6`.

- [ ] **Step 3: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add .claude/skills/cm4-bringup/SKILL.md
git commit -m "skills: cm4-bringup SKILL.md (silicon-truth dual-core playbook)"
```

---

### Task 2: references/silicon-truth-loop.md

**Files:**
- Create: `/Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/references/silicon-truth-loop.md`

- [ ] **Step 1: Write the file with exactly this content**

````markdown
# The silicon-truth loop

## Probe pattern

A probe is firmware that prints one `token=HEXVALUE` line per observation
on LPUART1, in a fixed order, with zero timestamps or nondeterminism, so a
QEMU transcript and an EVKB transcript diff directly. Exemplars:
`evkb/dualcore_mu_test/` (C, boot-header image, with both reference
transcripts checked in) and `evkb/qemu_dcd_boot_test/` (asm). Start new
probes from `templates/probe_firmware/`.

Print raw register values, not just pass/fail — divergences must be
informative. Timing-sensitive readbacks (e.g. GIR auto-clear) are allowed
but must be marked as such when transcripts are compared.

## Running a probe

QEMU (image is a flexspi_nor boot-header image; the stub needs boot-xip):

    ~/Development/qemu2/build/qemu-system-arm -M mimxrt1170-evk \
        -global fsl-imxrt1170.boot-xip=on -kernel probe.elf \
        -display none -serial file:qemu.uart -monitor none

EVKB (MCU-Link on the debug USB; VCOM is the LPUART1 console):

    # console capture (survives reconnects); port from rt1170-flash.sh
    python3 ~/Development/rt1170/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > hw.uart &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load probe.elf     # auto-runs after
    sleep 3; : > hw.uart          # drop the contaminated post-flash output
    # for an UNCONTAMINATED run (see traps below):
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Diff after stripping serial line endings:

    diff <(tr -d '\r\0' < qemu.uart) <(tr -d '\r\0' < hw.uart)

## Debugger contamination traps (all EVKB-verified)

- LinkServer's RT1176 connect script (`RT1170_connect_M7_wake_M4.scp`)
  WAKES THE CM4 into a spin loop at 0x2021FF00, sets CLOCK_ROOT1=0x201,
  and releases `SCR.BT_RELEASE_M4`.
- `SCR.BT_RELEASE_M4` is write-1-only and survives LinkServer's
  flash/reset flow (only a full system reset clears it): a post-flash
  run starts with `SCR=1` and a woken CM4.
- `DEMCR.VC_CORERESET` stays latched from flash sessions: wire or
  SYSRESETREQ resets halt silently at the reset vector.
- The boot ROM parks after any debugger-initiated reset — only a true POR
  (SW4 / power cycle) re-runs the flexspi boot AND the ROM's XMCD/DCD
  pass. `clean_boot.scp` works around all of this headlessly: SYSRESETREQ
  (SoC back to reset state, CM4 held, SCR=0), snapshot registers over the
  DAP while nothing has run, then manually dispatch the image at
  0x30002000. Note what it CANNOT do: exercise the real ROM's XMCD/DCD.

## "Silicon wins" bookkeeping

- qemu2 model changes cite the measurement: probe name + date in the code
  comment and the commit message (see `hw/misc/imxrt_mu.c` ASR bit-9
  comment for the pattern).
- Expected divergences are listed where the transcripts live (see
  `dualcore_mu_test/README.md`): currently unmodelled CCM-LPCG/GPC reads
  and timing-sensitive GIR readback.
- One board, one silicon rev: findings are recorded as measurements, not
  universal truths. If an errata or RM revision later explains one,
  update the comment.

## qemu2 regression set (run when qemu2 is touched)

From `~/Development/qemu2/build`:

    ninja qemu-system-arm
    # functional suites (imxrt1170 + imxrt1062, all must pass):
    export QEMU_TEST_QEMU_BINARY=$PWD/qemu-system-arm \
           QEMU_TEST_ARM_GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc \
           MESON_BUILD_ROOT=$PWD \
           PYTHONPATH=$PWD/../tests/functional:$PWD/../python
    ./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1170.py
    ./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1062.py
    # repo gates most affected by dual-core work:
    #   evkb/serial_test/run_qemu.sh, dualcore_mu_test (diff its
    #   transcript_qemu.txt), NXP SDK mcmgr hello_world + rpmsg pingpong
    git diff | ../scripts/checkpatch.pl --no-signoff -   # CI enforces it
````

- [ ] **Step 2: Verify the boot-xip flag and clean_boot references are correct**

Run: `grep -cE "boot-xip=on|clean_boot.scp|BT_RELEASE_M4" /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/references/silicon-truth-loop.md && test -f /Users/nicholasnewdigate/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp && echo scp-exists`
Expected: a count >= 4, then `scp-exists`.

- [ ] **Step 3: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add .claude/skills/cm4-bringup/references/silicon-truth-loop.md
git commit -m "skills: cm4-bringup silicon-truth-loop reference"
```

---

### Task 3: references/license-firewall.md

**Files:**
- Create: `/Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/references/license-firewall.md`

- [ ] **Step 1: Write the file with exactly this content**

````markdown
# License firewall

Policy: **headers-yes, logic-fresh.** The firmware tree stays free of
copyleft; `evkb/tools/license-audit.sh` is the enforcement mechanism and
must PASS as a gate on any change that adds or vendors source.

## Source quick-table

| Source | License | Allowed use |
|---|---|---|
| NXP SDK / CMSIS device headers (register layouts, `PERI_*.h`) | BSD-3 | vendor as-is, keep the notice |
| NXP SDK driver/middleware C code | BSD-3 | READ for behavior; isolated adaptations allowed with retained notice + provenance comment |
| Zephyr (HAL, soc, drivers) | Apache-2.0 (hal_nxp module: BSD-3) | read-for-understanding, cross-validation; avoid copying (NOTICE burden) |
| PJRC teensy cores (`cores/teensy*`) | MIT/PJRC + LGPL-2.1 per file | uncompiled reference copies ONLY — never copy; the copyleft sweep + link audit backstop this |
| RT1170 RM (`rm_full.txt`) | NXP doc | facts (offsets/bits) are fine; do not paste prose |
| qemu2 (GPL-2.0) | GPL | **NEVER into firmware repos.** One-way: firmware -> qemu2 is fine |

## Rules for new CM4 code

1. Register/device headers may be vendored verbatim, keeping each
   file's own notice: NXP device headers are BSD-3; ARM CMSIS Core
   headers (core_cm4.h, cmsis_gcc.h, ...) are Apache-2.0.
2. Driver/startup LOGIC is written fresh against the RM, then validated
   behaviorally against SDK/Zephyr and the QEMU gates.
3. A direct SDK function adaptation is the exception, not the rule: keep
   it isolated in its own file with the BSD-3 notice, and mark it:

       /* Adapted from mcuxsdk <path> @ <rev>, BSD-3-Clause.
        * Provenance: <what changed and why>. */

4. Every adapted block gets that provenance comment — greppable, for
   human attribution review (the audit only hunts copyleft).

## Extending the audit (same change, not later)

When a phase adds a source tree or a new gate build:
- add the tree to `REPOS` in `evkb/tools/license-audit.sh`,
- ensure the new build's depfiles are covered by the part-2 link
  manifest walk (add a `gate_dir:elf_target` pair to `GATES` if needed),
- run `sh evkb/tools/license-audit.sh` and require `LICENSE-AUDIT: PASS`.

Any allowlist addition needs a written justification comment in the
script, following the existing entries (uncompiled reference copies, or
preprocessor-dead dual-licensed branches proven empty by nm).
````

- [ ] **Step 2: Verify the audit script paths referenced actually exist**

Run: `test -x /Users/nicholasnewdigate/Development/rt1170/evkb/tools/license-audit.sh && grep -cE "REPOS|nm|allowlist|ALLOW" /Users/nicholasnewdigate/Development/rt1170/evkb/tools/license-audit.sh | head -1`
Expected: a nonzero count (script exists and has the referenced structure).

- [ ] **Step 3: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add .claude/skills/cm4-bringup/references/license-firewall.md
git commit -m "skills: cm4-bringup license-firewall reference"
```

---

### Task 4: references/cm4-roadmap.md

**Files:**
- Create: `/Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/references/cm4-roadmap.md`

- [ ] **Step 1: Write the file with exactly this content**

````markdown
# CM4 roadmap (LIVING document — update every session)

**Current phase: 1 (ready to start).** Append a dated entry to the
session log below whenever anything here changes.

## Phase 1 — CM7 boots the CM4 + MU IPC library

**Entry criteria:** none — qemu2 models the CM4/MU/SRC, EVKB-validated
(dualcore_mu_test transcripts, 2026-07-16).
**Deliverables:**
- CM7-side Arduino API: embed a CM4 image and boot it. The
  silicon-validated sequence: copy image to 0x20200000 (the TCM
  backdoor), `IOMUXC_LPSR_GPR0/GPR1` = VTOR (use the SYSTEM address
  0x20200000 — a 0x1FFE0000 TCM-window VTOR does not boot on silicon),
  `SRC->CTRL_M4CORE = SW_RESET`, `SRC->SCR |= BT_RELEASE_M4`.
  Hold-state indicator: `SRC STAT_M4CORE` bit 0 (ASR.RS is dead on
  silicon).
- MU mailbox/doorbell library: TR/RR (4 ch), GIR/GIP with ack-side GIR
  auto-clear, TIE/RIE/GIE, IRQ 118 both cores, `ASR` bit 9 quirk noted.
**Gates:** run_qemu*.sh-style gate (gate-lib.sh pattern); behavior
cross-checked against MCMGR (`mcuxsdk`) and `dualcore_mu_test`
transcripts; probe any register behavior not already covered by those
transcripts; license audit if any tree is added or files are vendored.

## Phase 2 — CM4 core variant (sketches compile for the CM4)

**Entry criteria:** Phase 1 merged; its gate green.
**Deliverables:** CM4 startup + linker (vector table at the backdoor
address, code/data in CM4 TCM views: ITCM 0x1FFE0000 / DTCM 0x20000000,
128K each), NVIC/SysTick (218 ext IRQs, 4 prio bits, 400 MHz),
`teensy-cmake-macros` dual-target build, dual-sketch blink + IPC demo as
the gate.
**Known qemu2 limitation to work through the loop:** peripheral IRQs fan
out to the CM7 NVIC only (MU excepted). Split-irq wiring lands in qemu2
per-line as CM4 libraries need it — each such change is a
new-model-behavior risk trigger (probe).

## Phase 3+ — per-library CM4 enablement

Wire/SPI/etc., each validated against the SDK bare-metal equivalent and
gated in QEMU. Ordering decided when Phase 2 lands. Each entry records:
entry criteria, deliverables, QEMU gate, probe obligations (met/queued),
audit status.

## Queued hardware checks

- Derive and EVKB-validate a minimal LPUART1 init for the asm probe
  template (templates/probe_firmware/), so probes print on clean-boot
  silicon without the Arduino core (queued 2026-07-17).

## Session log

- 2026-07-17: roadmap created; Phase 1 ready. Background: CM4/MU/SRC
  modelled in qemu2 and EVKB-validated; boot-ROM XMCD/DCD executes in
  QEMU; NXP MCMGR hello_world and rpmsg_lite_pingpong byte-identical
  QEMU-vs-board.
````

- [ ] **Step 2: Verify the roadmap facts against the repo**

Run: `test -f /Users/nicholasnewdigate/Development/rt1170/evkb/dualcore_mu_test/transcript_hw_evkb.txt && test -f /Users/nicholasnewdigate/Development/rt1170/evkb/dualcore_mu_test/transcript_qemu.txt && echo transcripts-present`
Expected: `transcripts-present`.

- [ ] **Step 3: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add .claude/skills/cm4-bringup/references/cm4-roadmap.md
git commit -m "skills: cm4-bringup living roadmap (Phase 1 ready)"
```

---

### Task 5: probe firmware template

**Files:**
- Create: `/Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/templates/probe_firmware/probe.s`
- Create: `/Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/templates/probe_firmware/README.md`

- [ ] **Step 1: Write probe.s with exactly this content**

````asm
/*
 * RT1176 register-probe template (CM7, flexspi_nor boot-header image).
 *
 * Prints deterministic `token=HEXVALUE` lines on LPUART1 so a QEMU
 * transcript and an EVKB transcript diff directly.  Copy, rename, and
 * replace the EXAMPLE PROBES section.  Same ELF runs on both sides:
 *   QEMU:  -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel …
 *   EVKB:  LinkServer flash … load probe.elf   (+ clean_boot.scp for an
 *          uncontaminated run; see references/silicon-truth-loop.md)
 *
 * Build: arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 -mthumb \
 *            -Wl,-Ttext=0x30000000 -o probe.elf probe.s
 */
    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .text
    .global _start

    /* FCB at +0x400: the boot-ROM stub validates only the tag. */
    .org 0x400
    .ascii "FCFB"

    /* IVT at +0x1000. */
    .org 0x1000
    .word 0x412000D1            /* header: tag 0xD1, len 0x20, ver 0x41 */
    .word _start + 1            /* entry (Thumb) */
    .word 0                     /* reserved1 */
    .word 0                     /* dcd pointer (none) */
    .word 0x30001020            /* boot_data pointer */
    .word 0x30001000            /* self */
    .word 0                     /* csf (unsigned) */
    .word 0                     /* reserved2 */
    .word 0x30000000            /* boot_data.start */
    .word 0x3000                /* boot_data.size */
    .word 0                     /* boot_data.plugin */
    /* No XMCD: +0x1040 stays zero. */

    /* Application vector table at +0x2000 (stub sets MSP/VTOR here). */
    .org 0x2000
vectors:
    .word 0x20040000            /* MSP: top of the 256K fuse-default DTCM */
    .word _start + 1

    .thumb_func
_start:
    /*
     * LPUART1 raw DATA writes need no init under QEMU.  On the EVKB a
     * clean boot has no UART clock/pinmux/baud/TE set up: add a
     * silicon-validated LPUART1 init here before printing (reference:
     * the Arduino core's Serial1.begin path used by dualcore_mu_test),
     * or expect an empty hw.uart.
     */
    ldr     r7, =0x4007C000     /* LPUART1 */

    adr     r0, msg_start
    bl      puts

    /* ---- EXAMPLE PROBES: replace from here ---- */
    ldr     r4, =0x40C48000     /* MUA */
    adr     r0, tok_sr0
    ldr     r1, [r4, #0x20]     /* MUA.SR */
    bl      phex                /* expect 00F00200 (bit9 quirk, TEs set) */

    ldr     r4, =0x40C04000     /* SRC */
    adr     r0, tok_stat
    ldr     r1, [r4, #0x290]    /* STAT_M4CORE */
    bl      phex                /* expect 00000001 while the CM4 is held */
    /* ---- to here (also replace the tok_* strings below) ---- */

    adr     r0, msg_done
    bl      puts
park:
    wfi
    b       park

    /* puts: r0 = NUL-terminated string (clobbers r0,r1) */
    .thumb_func
puts:
    ldrb    r1, [r0], #1
    cbz     r1, 9f
    str     r1, [r7, #0x1C]     /* LPUART DATA */
    b       puts
9:  bx      lr

    /* phex: r0 = token string, r1 = 32-bit value  ->  "token=XXXXXXXX\n"
       (clobbers r0-r3, r5, r6, lr-safe via r6) */
    .thumb_func
phex:
    mov     r5, r1
    mov     r6, lr
    bl      puts
    movs    r0, #61             /* '=' */
    str     r0, [r7, #0x1C]
    movs    r2, #8
1:  lsrs    r3, r5, #28
    cmp     r3, #10
    ite     lt
    addlt   r3, #48             /* '0' */
    addge   r3, #55             /* 'A' - 10 */
    str     r3, [r7, #0x1C]
    lsls    r5, r5, #4
    subs    r2, #1
    bne     1b
    movs    r0, #13             /* '\r' */
    str     r0, [r7, #0x1C]
    movs    r0, #10             /* '\n' */
    str     r0, [r7, #0x1C]
    bx      r6

    .ltorg
    .align 2
msg_start: .asciz "PROBE-START\r\n"
msg_done:  .asciz "PROBE-DONE\r\n"
tok_sr0:   .asciz "sr0"
tok_stat:  .asciz "stat_m4"
````

- [ ] **Step 2: Write README.md with exactly this content**

````markdown
# probe_firmware template

Copy this directory, rename `probe.s`, and replace the EXAMPLE PROBES
section. Keep tokens fixed-order and values raw-hex so transcripts diff.

Build:

    /Applications/ARM_10/bin/arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 \
        -mthumb -Wl,-Ttext=0x30000000 -o probe.elf probe.s

Run on QEMU and on the EVKB, and diff — commands and the debugger
contamination traps are in the skill's `references/silicon-truth-loop.md`.
NOTE: under QEMU the raw LPUART writes print with no init; on the EVKB
you must add a silicon-validated LPUART1 init first (see the probe.s
comment), or the hardware transcript will be empty.
For probes needing C (interrupt legs, larger flows), start from
`evkb/dualcore_mu_test/` instead.
````

- [ ] **Step 3: Build the template and verify the ELF layout**

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup/templates/probe_firmware
/Applications/ARM_10/bin/arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 -mthumb -Wl,-Ttext=0x30000000 -o /tmp/probe_template.elf probe.s
/Applications/ARM_10/bin/arm-none-eabi-objdump -h /tmp/probe_template.elf | grep -q "30000000" && echo BUILD-OK
```
Expected: `BUILD-OK` (a linker entry-symbol warning is fine).

- [ ] **Step 4: Run it in QEMU and verify the transcript**

Run:
```bash
timeout 10 ~/Development/qemu2/build/qemu-system-arm -M mimxrt1170-evk \
    -global fsl-imxrt1170.boot-xip=on -kernel /tmp/probe_template.elf \
    -display none -monitor none -serial file:/tmp/probe_template.uart 2>/dev/null
tr -d '\r' < /tmp/probe_template.uart
```
Expected output exactly:
```
PROBE-START
sr0=00F00200
stat_m4=00000001
PROBE-DONE
```

- [ ] **Step 5: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add .claude/skills/cm4-bringup/templates/probe_firmware
git commit -m "skills: cm4-bringup probe-firmware template (QEMU-verified)"
```

---

### Task 6: discovery symlink

**Files:**
- Create: symlink `/Users/nicholasnewdigate/Development/rt1170/.claude/skills/cm4-bringup` -> `../../evkb/.claude/skills/cm4-bringup` (outside the git repo — no commit)

- [ ] **Step 1: Create the symlink**

```bash
mkdir -p /Users/nicholasnewdigate/Development/rt1170/.claude/skills
ln -sfn ../../evkb/.claude/skills/cm4-bringup \
    /Users/nicholasnewdigate/Development/rt1170/.claude/skills/cm4-bringup
```

- [ ] **Step 2: Verify it resolves to the canonical SKILL.md**

Run: `readlink /Users/nicholasnewdigate/Development/rt1170/.claude/skills/cm4-bringup && test -f /Users/nicholasnewdigate/Development/rt1170/.claude/skills/cm4-bringup/SKILL.md && echo LINK-OK`
Expected: the relative path, then `LINK-OK`.

---

### Task 7: final layout check

- [ ] **Step 1: Verify the complete tree**

Run: `find /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/skills/cm4-bringup -type f | sort`
Expected exactly these six files:
```
.../cm4-bringup/SKILL.md
.../cm4-bringup/references/cm4-roadmap.md
.../cm4-bringup/references/license-firewall.md
.../cm4-bringup/references/silicon-truth-loop.md
.../cm4-bringup/templates/probe_firmware/README.md
.../cm4-bringup/templates/probe_firmware/probe.s
```

- [ ] **Step 2: Confirm the evkb tree is clean of plan work**

Run: `git -C /Users/nicholasnewdigate/Development/rt1170/evkb status --short -- .claude`
Expected: no output (everything under .claude committed).

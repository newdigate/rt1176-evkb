---
name: cm4-bringup
description: Use for ANY i.MX RT1176 CM4/dual-core work — extending the imxrt1176 core or Arduino libraries to the CM4, MU/IPC features, CM4 boot/release sequencing, picking up the "next bring-up phase", or any change that touches a new RT1176 register surface in the cores, libraries, or the qemu2 machine model. Enforces the silicon-truth validation loop and the license firewall.
---

# CM4 bring-up (silicon-truth dual-core methodology)

## Why this skill exists

The RT1176 docs lie exactly where it is expensive: the RM and NXP's own
`PERI_MU.h` document MU ASR.RS at bit 7, but silicon never sets it (bit 9
reads 1 instead); `SCR.BT_RELEASE_M4` is write-1-only; the FlexRAM fuse
default is 256K+256K, not what third-party writeups claim; a CM4 VTOR in
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
   (listed in silicon-truth-loop.md) and scripts/checkpatch.pl.
7. If a probe was triggered: run it on the EVKB, diff transcripts, and
   apply "silicon wins" (below).
8. If any source tree was added or vendored: run
   `evkb/tools/license-audit.sh` and extend its coverage in the SAME
   change (see license-firewall.md).
9. Update `references/cm4-roadmap.md` (phase status, queued probes, new
   discoveries) and commit before ending the session.

## Reference triangulation

| Source | Where | Use for |
|---|---|---|
| RT1170 RM (text, greppable) | `~/Development/rt1170/rm_full.txt` | register offsets, bit fields, IRQ tables, memory maps |
| NXP SDK source | `~/Development/mcuxsdk-ws/mcuxsdk` (+ `~/Development/mcuxsdk-examples`) | driver sequences, MCMGR/RPMsg protocols, dcd/xmcd, linker layouts |
| Zephyr HAL/soc | `~/Development/zephyr/projects/zepherproject` | second-core boot flow, devicetree facts (MU=IRQ 118), alternative driver shapes |
| Repo gate firmware | `evkb/*/run_qemu.sh` + sources | known-good bare-metal behavior on this exact machine model |

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

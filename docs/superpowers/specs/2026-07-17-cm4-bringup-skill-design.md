# cm4-bringup skill (silicon-truth dual-core methodology) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-17

## Goal

Create a **methodology playbook skill** at `evkb/.claude/skills/cm4-bringup/` (plus a
symlink from `rt1170/.claude/skills/cm4-bringup`) that future sessions invoke for any
RT1176 CM4 / dual-core work: extending the imxrt1176 Arduino core and libraries to the
CM4, refining the qemu2 `mimxrt1170-evk` model to match silicon in the same loop,
cross-referencing everything against existing bare-metal code (NXP SDK, Zephyr, the
repo's own gate firmware), and never letting restrictively-licensed code into the
firmware tree.

The skill is the deliverable — **not** the CM4 port itself. Executing the port happens
in later sessions *using* the skill, one roadmap phase at a time. Success criterion: a
fresh session with no memory of this conversation can invoke `/cm4-bringup`, identify
the current phase from the roadmap file, execute a work item end-to-end (references →
QEMU gate → hardware probe when triggered → license audit), and leave the roadmap
updated.

## Why this shape (exploration findings)

- **The methodology already exists and is proven; it just isn't written down.** The
  2026-07-16/17 dual-core QEMU sessions produced a working loop: diffable
  `token=HEXVALUE` probe firmware run on both QEMU and the EVKB
  (`evkb/dualcore_mu_test/`, transcripts checked in), headless clean-boot via
  `clean_boot.scp` (LinkServer's connect script wakes the CM4, latches
  `VC_CORERESET`, and `SCR` survives debugger resets — a true POR is the only path
  through the ROM's XMCD/DCD pass), and "silicon wins" model fixes with measurements
  cited in comments (MU ASR bit 9 vs the documented bit 7, write-1-only
  `BT_RELEASE_M4`, 256K FlexRAM fuse default, 16 MPU regions, FlexSPI aliasing).
  That knowledge evaporates without a skill.
- **Docs lie precisely where it is expensive.** RM rev 5 and NXP's own `PERI_MU.h`
  both document ASR.RS at bit 7; silicon never sets it. The RM's DCD/GPR guesses in
  third-party writeups were wrong about offsets and bits. Hence the **risk-triggered
  hardware mandate** below rather than trust-the-manual or hardware-always.
- **The license firewall is already engineered.** `evkb/tools/license-audit.sh` is a
  three-part audit (wrap-tolerant copyleft header sweep with justified allowlist;
  link-manifest depfile audit with nm-verified empty objects for dual-licensed files;
  byte-identity checks). The skill builds on it and makes extending its coverage a
  phase gate, instead of inventing a parallel mechanism.
- **evkb is the git repo; the outer `rt1170/` dir is not version-controlled.** The
  canonical skill lives in evkb (versioned, reviewable); the symlink keeps discovery
  working for sessions rooted at `rt1170/`.

## Decisions (each explored with alternatives during brainstorming)

1. **Artifact type: methodology playbook** with a companion living roadmap reference —
   not a one-shot campaign plan. (Rejected: CM4-campaign-as-skill — the methodology
   generalizes past the CM4; two-skill split — more maintenance than the content
   justifies.)
2. **CM4 support shape: phased.** Phase 1 = CM7-side boot API + MU IPC library;
   Phase 2 = full CM4 core variant (sketches compile for the CM4); Phase 3+ =
   per-library enablement. (Rejected: full dual-sketch core first — biggest lift with
   no early value; coprocessor-only — undersells the ask.)
3. **Hardware gate rule: risk-triggered probes.** A trigger table mandates an EVKB
   probe; everything else may proceed on QEMU + triangulated references with the
   hardware check queued in the roadmap. (Rejected: hardware-always — stalls when the
   board is away; QEMU-first-batch — too much doc-vs-silicon drift exposure.)
4. **License policy: headers-yes, logic-fresh.** NXP BSD-3 register/CMSIS headers may
   be vendored as-is; logic is written fresh against the RM and validated
   behaviorally against SDK/Zephyr; isolated SDK adaptations allowed with retained
   BSD-3 notice and a provenance comment; `license-audit.sh` pass is a phase gate and
   its coverage must be extended in the same change that adds source trees. Hard
   one-way firewall: **QEMU (GPL-2.0) code never flows into firmware repos**
   (firmware → QEMU is fine). PJRC reference copies stay uncompiled. (Rejected:
   copy-freely — allowlist creep; clean-room-only — header duplication busywork.)
5. **Placement: evkb repo + pointer symlink**, as above.

## Skill layout

```
evkb/.claude/skills/cm4-bringup/
├── SKILL.md                      # lean process; ~150 lines; rigid-process style
├── references/
│   ├── silicon-truth-loop.md     # validation methodology (detail)
│   ├── license-firewall.md       # sourcing policy + audit gates (detail)
│   └── cm4-roadmap.md            # LIVING phase tracker; updated as phases land
└── templates/
    └── probe_firmware/           # trimmed dualcore_mu_test-derived probe skeleton
rt1170/.claude/skills/cm4-bringup -> ../../evkb/.claude/skills/cm4-bringup  (symlink)
```

`SKILL.md` frontmatter: `name: cm4-bringup`; description triggers on CM4/dual-core
core or library work, MU/IPC, "next bring-up phase", QEMU-vs-silicon validation, and
work touching the dual-core register surfaces (SRC core-release, MU, IOMUXC_LPSR
GPRs, GPC, CM4 TCM/backdoor) in cores/libraries/qemu2.  *Amended during
implementation:* the original "any new RT1176 register surface" clause was narrowed
to the dual-core surfaces after the Task 1 quality review showed it over-triggered
on unrelated single-core work (e.g. CAN or USB bring-up), for which the skill's
CM4-roadmap-first checklist is meaningless. Body: the core
loop (below), a pointer to each reference file with a one-line "read when" rule, and
the checklist the session must follow. Authoring follows the superpowers
writing-skills conventions.

## The core loop (SKILL.md body, summarized)

1. **Triangulate references before writing code.** RM text (`rt1170/rm_full.txt`,
   grep recipes included), NXP SDK source (`~/Development/mcuxsdk-ws/mcuxsdk`, and
   `mcuxsdk-examples` for demos), Zephyr HAL/soc
   (`~/Development/zephyr/projects/zepherproject`), and the repo's own gate firmware
   (`evkb/*/run_qemu.sh` tests). Disagreement between any two references is itself a
   hardware-probe trigger.
2. **Develop against QEMU gates.** Every new capability gets a `run_qemu.sh`-style
   gate (gate-lib.sh pattern) before it is "done"; qemu2 functional suites
   (`tests/functional/arm/test_imxrt1170.py`, `test_imxrt1062.py`) must stay green
   when qemu2 changes.
3. **Consult the risk-trigger table.** Mandatory silicon probe when touching:
   undocumented/reserved bits; reset values; boot/reset/release sequencing; memory
   aliasing or backdoor windows; clock/power gating; anything where RM and SDK
   disagree; any new or behaviorally-changed qemu2 device model.
4. **Probe pattern.** Diffable `token=HEXVALUE` transcript firmware (template
   provided); same ELF on QEMU (`boot-xip=on`) and EVKB (LinkServer flash +
   `clean_boot.scp` for uncontaminated runs); transcripts diffed and committed next
   to the probe. Known contamination traps documented inline (CM4 wake by the
   connect script, sticky `SCR`, latched vector catch, ROM parking after debugger
   resets).
5. **Silicon wins.** qemu2 model changes cite the measurement (probe + date) in code
   comments and commit messages; deliberate RM deviations and expected
   QEMU-vs-silicon divergences (unmodelled LPCG/GPC reads, timing-sensitive
   readbacks) are documented, never silently absorbed.
6. **License audit as exit gate** for any phase or change that adds/vendors source.
7. **Update the roadmap file** (phase status, queued hardware checks, new
   discoveries) before ending the session.

## Roadmap seed (references/cm4-roadmap.md initial content)

- **Phase 1 — CM7 boots the CM4 + MU IPC library.** CM7-side Arduino API to embed a
  CM4 image and boot it (LPSR_GPR0/1 VTOR at the 0x20200000 backdoor + `CTRL_M4CORE`
  SW reset + `SCR.BT_RELEASE_M4` — the silicon-validated sequence); an MU
  mailbox/doorbell library (TR/RR, GIR/GIP, IRQ 118). Gates: QEMU test; probe
  transcript for any newly-touched register behavior; behavior cross-checked against
  MCMGR (`mcuxsdk`) and the checked-in `dualcore_mu_test` transcripts; license audit
  extended to any new tree. Entry criteria: none (ready now).
- **Phase 2 — CM4 core variant.** Sketches compile for the CM4: startup + linker
  (vector table at the backdoor address; code/data in CM4 TCM views), NVIC/SysTick
  (218 ext IRQs, 4 prio bits, 400 MHz), `teensy-cmake-macros` dual-target build,
  dual-sketch blink + IPC demo as the gate. Known qemu2 limitation to work through
  the loop: peripheral IRQs currently fan out to the CM7 NVIC only (MU excepted) —
  split-irq wiring lands in qemu2 per-line as CM4 libraries need it.
- **Phase 3+ — per-library CM4 enablement.** Wire/SPI/etc., each validated against
  the SDK bare-metal equivalent and gated in QEMU; ordering decided as Phase 2
  lands. Each phase entry records: entry criteria, deliverables, QEMU gate, probe
  obligations (met/queued), audit status.

## Out of scope (this design)

Executing any roadmap phase; changes to qemu2 or the cores themselves; CI wiring for
the audit (stays a manually-run gate as today); porting the skill to other boards.

## Implementation sketch (for the plan)

1. Author `SKILL.md` + the three reference files + probe template (content largely
   distilled from the 2026-07-16/17 session artifacts and memory notes; the
   writing-skills skill governs format).
2. Create the `rt1170/.claude/skills` symlink.
3. Verify discovery (skill listed in a session rooted at each location) and that the
   probe template builds with `/Applications/ARM_10` and runs on QEMU.
4. Commit to evkb; the roadmap file starts at "Phase 1: ready".

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

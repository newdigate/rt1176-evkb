# W17 Phase 0 — the uAP question, answered

**Date:** 2026-08-20  ·  **Answer: YES, the firmware has the uAP command layer.**

The handoff (`docs/superpowers/handoff/2026-08-20-w17-uap-host-mode.md`) made
Phase 0 the gate on everything else: *"Send `SYS_CONFIGURE` (a GET is enough)
and/or `BSS_START` to the running firmware and read the reply's `result` field.
… `result=0` or a real error code = uAP present; unknown-command = this blob
lacks it and the whole plan changes."*

It is answered, and the answer is more useful than a yes — with one new fault
that Phase 1 will hit on its first day.

Evidence, with every reply byte quoted:
`examples/networking/m2_uap_probe/transcript_hw_evkb.txt`.

## What was built

`examples/networking/m2_uap_probe` — a self-contained probe that brings the
card up, downloads the blob, and asks a matrix of commands, printing one
machine-parsable line per cell.

One driver change, additive and the smallest that could work:
`Iw416::sendHostCmdBss(cmd, body, len, bssType, bssNum = 0)`. mlan routes a
command to an interface through `seq_num`'s high byte
(`HostCmd_SET_SEQ_NO_BSS_INFO`: bss_num 11:8, bss_type 15:12), and every
command this driver had ever sent left both at 0. `sendHostCmd()` now delegates
with `(0, 0)`, so no existing caller changes shape or wire bytes — verified by
inspection and by all eleven pre-existing M.2 QEMU gates staying green.

Two gates: `run_qemu.sh` (card-absent) and `run_qemu_wifi.sh` (the IW416 model).
Sweep goes 108 → **110**.

## The answer

| command | id | result | body |
|---|---|---|---|
| `HOST_CMD_APCMD_SYS_INFO`  | 0x00AE | `0x0000` OK | `"w8978o-V0, RF878X, FP92, 16.92.21.p1"` |
| `HOST_CMD_APCMD_STA_LIST`  | 0x00B3 | `0x0000` OK | `sta_count = 0`, in a 10-byte reply (`S_DS_GEN` + 2) |
| reserved `0x7FFE` (control)| —      | `0x0002` NOT_SUPPORT | — |
| `HOST_CMD_APCMD_SYS_CONFIGURE` | 0x00B0 | *no reply, and the port dies* | see below |

Both AP commands answer on **both** `bss_type=0` and `bss_type=1`, and both are
distinct from what this firmware does with an id nothing defines. The
`SYS_INFO` body is the un-fakeable part: that build string exists nowhere in
our image.

`STA_LIST` returning a real, empty station list is the second result worth
having — it is exactly the oracle the future join/leave gates need, and it
works today.

### The card acts on `bss_type`

Replies echo the `seq_num` they were sent, and the uAP nibble comes back:
`bss=0` requests return `0x0003, 0x0007, …`; `bss=1` requests return
`0x1004, 0x1008, …` (bits 15:12 = 1 = `MLAN_BSS_TYPE_UAP`).

Two consequences:

1. `waitCmdResp()`'s low-byte-only seq comparison is now **mandatory**. It was
   written speculatively in W12 ("keeps the comparison honest even if that ever
   changes") — this is that change, and a whole-word compare would reject every
   uAP reply as stale.
2. **QEMU cannot catch a regression there.** The model zeroes the high byte in
   its replies, so masked and unmasked comparisons both pass against it. If a
   uAP reply ever reads as stale on silicon while every gate is green, this is
   the divergence.

## The method, because run 1 was wrong

Run 1 put the positive control first and the negative control last. It read
`SYS_INFO` OK, then `SYS_CONFIGURE`, `STA_LIST` and the reserved-id control all
timing out — and the obvious conclusion, "this firmware answers unknown
commands with silence, so `STA_LIST` is missing", **is false**. One command had
wedged the command port and every later timeout was an echo of that one event.
The control ran after the damage, so the run could not tell the two stories
apart.

The probe now enforces a **bracketing rule**: a cell is evidence only if the
nearest positive control on *each side* of it answered. Unbracketed cells are
printed as `uap_unbracketed` and excluded from the tally, never folded into a
count. The negative control runs first, on a port known healthy, and again at
the far end; positive controls run between every AP command.

This is the tree's own method (bisect one variable, controls at both ends)
applied to a command matrix, and it is the difference between run 1's wrong
answer and run 2's right one — same firmware, same driver, ten minutes apart.

## ★ FAULT 1 (OPEN): `SYS_CONFIGURE` 0x00B0 kills the command port

Not "unsupported" — the port dies. After it, no packet of any kind arrives
again: `CMD_PORT_UPLD` is never raised, `int_seen` stays `0xC2`,
`seq_mismatches` stays 0, and the port does not recover within the ~15 s of
further commands the matrix issues. The port was demonstrably alive one command
earlier.

Bisected, one variable, controls at both ends. The obvious hypothesis was a
malformed request — action(2) with an **empty** TLV buffer — so a second form
was sent first, carrying the one TLV mlan pairs with a channel GET
(`TLV_TYPE_UAP_CHAN_BAND_CONFIG` 0x012A). **Both wedge.** The empty TLV buffer
is not the cause. And the bare form is not our invention either: mlan's own
`wlan_uap_cmd_sys_configure_ext` emits exactly action(2) and nothing else when
it has neither an ioctl buffer nor a data buffer.

Not established, and not to be assumed:

- whether the firmware **crashed** or merely stopped answering;
- whether some earlier uAP initialisation is required that mlan does and we do
  not — the likeliest remaining explanation, and where Phase 1 should look
  first (`wlan_uap_ioctl.c`, and how `wlan.c` reaches `WLAN_BSS_ROLE_UAP`);
- whether `bss_num` must be non-zero for the uAP interface — everything here
  used `bss_num=0, bss_type=1`;
- whether a power cycle is the only recovery. Every run reset the card at boot,
  so recovery was never tested in isolation.

`SYS_CONFIGURE` sits **last** of the AP rows in the committed matrix for this
reason: it is the known port-killer, and putting it last confines the damage to
the trailing controls instead of costing every other row its reading.

## The blob A/B

Both SDK blobs were built and run on the same matrix:
`sduartIW416_wlan_bt.bin` (Wi-Fi + BT) and `sdIW416_wlan.bin` (Wi-Fi only).
Genuinely different images (text 427364 vs 295908 bytes), and **53 capture
lines each, zero differing** — same `fw_release=0x9B105C15`, same version
string, same everything. The blobs differ by the appended Bluetooth image, not
by the Wi-Fi build.

So the handoff's "probably in the firmware already running" is confirmed, and
strengthened: it is in *both*, and nothing in Phase 1 needs to depend on which
is loaded.

## What this does NOT settle

- **That an AP can be started.** `BSS_START` was never sent — it emits RF with
  whatever the firmware defaults to, and is compiled out unless
  `-DM2_UAP_PROBE_BSS_START=ON`. "The handler exists" is a long way from "the
  BSS comes up".
- **`SYS_CONFIGURE`**, which is how an AP gets configured at all. Fault 1 is
  Phase 1's gate, before any TLV-soup design work.
- **The data path.** `bss_type`/`bss_num` are still hardcoded to 0 in the TxPD
  the driver builds and ignored in the RxPD it parses. W17 touched the
  **command** path only; the handoff's item 2 is untouched and is still the
  change that lands in the hot path W8/W12/W16 were dug out of.
- **IEEE power save under a live uAP BSS** — still the open question the
  handoff flagged, still unasked.

## Recommended next step

Not the uAP command set. **Fault 1 first**: find out why `SYS_CONFIGURE` stops
the command port, because every remaining Phase-1 item goes through that
command. Read `wlan_uap_ioctl.c`'s path into `wlan_ops_uap_prepare_cmd` for the
initialisation mlan performs before it, and check whether the uAP `mlan_private`
carries a `bss_num` this driver is not sending.

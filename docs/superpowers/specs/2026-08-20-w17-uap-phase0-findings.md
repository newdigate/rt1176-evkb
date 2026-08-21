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

## ★ FAULT 1 — **RESOLVED 2026-08-21**. The handler wants a populated request.

> **The account in this section was written on 2026-08-20 and its conclusion
> had an uncontrolled variable.** Every row that answered in those runs was
> empty-bodied and both rows that wedged carried a body, so "0x00B0 is the
> killer" and "a bodied command is the killer" fit the evidence equally well.
> The follow-up run closed that, and three more besides: see
> **[FAULT 1, investigated](#fault-1-investigated-2026-08-21)** at the end of
> this document and the `W17 FAULT 1, INVESTIGATED` section of
> `examples/networking/m2_uap_probe/transcript_hw_evkb.txt`. The section below
> is kept as written, because the confound is the lesson.

## FAULT 1 as first recorded (2026-08-20): `SYS_CONFIGURE` 0x00B0 kills the command port

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

## The pin (closed)

`M2Radio` master is pushed (`c7d0510..494f230`) and `evkb.cmake` names
`494f230`. Verified in fresh-user mode rather than assumed: configured and
built `m2_uap_probe` with `-DEVKB_FORCE_FETCH=ON`, which fetched M2Radio at
that SHA into `_deps/m2radio-src/`, and the fetched `Iw416.cpp` carries
`sendHostCmdBss()`. Clean build.

★ A pin bump makes every consuming build dir **reconfigure** on its next build.
Dirs configured before 2026-08-14 fail that reconfigure with a message that
never mentions a toolchain — `COMPILERPATH is UNDEFINED`. Gates are unaffected
(a gate runs the cached `.elf` and never reconfigures), which is also the
reason a green sweep is no evidence that a build dir can still configure.

## Running the 110-gate sweep

The sweep belongs in the full-build checkout, and `m2_uap_probe` arrives there
with **no build directory** — two SKIPs unless it is built first:

```
cd examples/networking/m2_uap_probe
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

No blob and no credentials: `build/` is deliberately the fresh-clone build, and
both gates require it (the model gate refuses a build that supplies a blob,
because the model cannot do a download). The silicon builds live in
`build-hw/` (combo blob) and `build-hw-wlan/` (Wi-Fi-only blob).


## FAULT 1, investigated (2026-08-21)

Four more runs on the same board and card, three on the default matrix and one
on an `action=SET` variant. **The wedge reproduced in every run.** Full
evidence, with the reply bytes and the raw captures, is in the transcript.

### Settled

- **It is the command, not the body.** Two new controls: the reserved id
  `0x7FFE` *carrying* `action(2)` answers `NOT_SUPPORT` on both interfaces, and
  `HostCmd_CMD_802_11_MAC_ADDRESS` 0x004D (action(2)+mac(6), size 16) —
  **mlan's own** uAP-addressed init command, `wlan_get_mac_addr_uap()` —
  answers `RESULT_OK` on `bss_type=1` with the real MAC in the body. A bodied
  command on the uAP interface is not merely accepted but understood.
- **Action, TLV and size are all ruled out.** Three forms, each sent first in
  its own firmware life: bare GET (size 10), GET + `CHAN_BAND` TLV (16),
  SET + `MAX_STA_CNT` TLV (16, mlan's own SET shape). All three wedge
  identically. `MACADDR.uap` is also 16 bytes and is answered.
- **The firmware does not crash.** `fwstatus` reads `0xFEDC` (FIRMWARE_READY)
  before, at the moment of failure, and after; CMD52 keeps working; the I/O
  port is unchanged. The card is alive and answers nothing.
- **The wedge has a register signature.** `CARD_TO_HOST_EVENT` (0x5C) reads a
  constant `0x0008` across every healthy command — sampled either side of all
  34/38 cells per run, so this is a distribution and not a single reading — and
  shows `0x0088` (bit 7, `DN_LD_CP_RDY`) in exactly the window of the command
  that kills the port, in both runs, on two different forms. What bit 7 *means*
  here is not claimed; that it is a reproducible trigger is.
- **The data port is not involved.** Thirty `serviceLink()` passes after the
  wedge found no frames, no data and no event, and draining did not bring the
  command port back. The "undrained upload holds off the command port" family
  is eliminated.
- **No self-recovery** in ~90 s (30 retries at 1 s, each also costing its 2 s
  timeout). Previously only "not within ~15 s".
- **`MACADDR.uap` is not the missing prerequisite** — it ran immediately
  before, bracketed by controls that answered, and `SYS_CONFIGURE` wedged
  anyway. And `bss_num=0` is what mlan uses for uAP
  (`mlan_glue.c`, `bss_attr[1].bss_num = 0`), so that open question is answered
  from the reference: what we sent is what mlan sends.
- **Incidental, and load-bearing for Phase 1:** the uAP interface reports the
  **same MAC** as the STA interface. No separate uAP address is provisioned.

- **A card reset recovers it — no power cycle, no MCU reboot.** Run 6, after
  the board was repowered: the wedge reproduced for the fifth time, the port
  again never recovered on its own, and then
  `uap_reinit sdio=ok iw416=ok fw=ok hwspec=ok` → **`uap_reinit=recovered`**.
  The M.2 reset GPIO sequence, a fresh SDIO/IW416 bring-up, a fresh blob
  download and `GET_HW_SPEC` all succeed and the command port answers again.
  Two consequences: a uAP driver can **recover in the field** (the wedge is
  terminal for the firmware life, not for the card), and **the bench can
  iterate** — a variant costs a card reset, not a reflash, which makes a
  multi-variant probe in one firmware life the obvious next tool.

### Still open
- **Why the handler dies.** Inside a closed blob, not answerable from outside.
- **Whether a prerequisite other than `MAC_ADDRESS` exists.** Narrowed, not
  closed: mlan's init sends nothing else to `bss_type=1`.

### What Phase 1 inherits

`SYS_CONFIGURE` cannot be used as the configuration path until this is
resolved, and no amount of reshaping the request will do it — three shapes
covering both action directions and both TLV states all fail the same way. The
card-reset question is now settled (it recovers), so the next move is a
**multi-variant probe in one firmware life** — resetting the card between
variants instead of reflashing — hunting a prerequisite outside mlan's init
path: how `wlan.c` reaches `WLAN_BSS_ROLE_UAP`, and whether a `BSS_MODE`/role
command precedes configuration on this firmware.

### One more thing the probe now carries

`M2_UAP_SYSCFG_SET_FIRST` (CMake option, default OFF) puts an `action=SET`
`SYS_CONFIGURE` first in the matrix. It exists because **the first
`SYS_CONFIGURE` of a run is the only one whose reading survives** — it takes
the port with it and every later row is unbracketed — so a variant can only be
tested by being put first, one variant per firmware life. Default OFF so the
gated build keeps the matrix the QEMU gates assert.


## FAULT 1, resolved (2026-08-21)

**A fully populated `SYS_CONFIGURE` is accepted. A minimal one kills the port.**
Both measured in **one firmware life**, on the same card, minutes apart — so
this needs no cross-run comparison:

```
HWSPEC.ctl+.e3   bss=0/1  st=ok                              <- port healthy in
SYSCFG.fullopen  bss=0    st=ok result=0x0000  cs 0x08->0x08
SYSCFG.fullopen  bss=1    st=ok result=0x0000  cs 0x08->0x08
HWSPEC.ctl+.e5   bss=0/1  st=ok                              <- port healthy out
SYSCFG.chantlv   bss=0    st=cmd-timeout       cs 0x08->0x88 <- the wedge
```

`RESULT_OK` on both interfaces, **bracketed** on each side, so it is evidence by
the example's own rule.

### Where the hypothesis came from

Not from guessing at shapes — from asking the reference what a real bring-up
sends. `wifi_uap_start()` runs `wifi_cmd_uap_config()` → (PMF) → (11d) →
`BSS_START`, so the config **is** the first uAP command and there is no missing
prerequisite upstream of it. And its builder refuses to construct the command
without a configuration at all:

```c
if (pioctl_buf == MNULL) { LEAVE(); return MLAN_STATUS_FAILURE; }
```

"How much configuration the request carries" was the one axis the three earlier
shapes never varied, and the only one mlan's own code says is not optional.

### What shipped

`Iw416::uapConfigure(const UapConfig &)` — the nine TLVs mlan emits for an open
AP on a manual 2.4 GHz channel, built by the **driver** so what was tested is
what Phase 1 ships. 82 bytes, verified in QEMU before the bench run. The TLV set
is a **mask** (`UapTlv`), because the boundary between accepted and rejected is
the finding and can only be found by varying the set.

★ Every TLV `len` is the **payload** length. `DTIM` and `BCAST` are **1** byte,
`AUTH` is **3** (auth_type, PWE_derivation, transition_disable). Those are the
easy ones to get wrong, and one wrong length mis-parses everything after it.

### Consequences

- **There is no harmless probe of `SYS_CONFIGURE`.** A GET is exactly the shape
  that wedges; nothing may "just query" the uAP configuration.
- **The minimal rows stay in the matrix.** They are no longer a mystery, they
  are the control that proves the boundary.
- **The probe sends the full configuration by default**, so the QEMU gate covers
  `uapConfigure()`'s construction. The model can't reproduce the silicon
  behaviour, but it can prove the driver still builds the request byte for byte
  — the part that rots silently. Demonstrated RED by moving the DTIM payload
  length from 1 to 2.

### Still open

- Whether **`BSS_START`** actually brings the BSS up. The sequence is **written,
  built and gated** (`-DM2_UAP_PROBE_BSS_START=ON`) but has not run on silicon:
  the MCU-Link debug port dropped again before it could be flashed. It
  configures first, starts, services the link for the card's own
  `EVENT_MICRO_AP_BSS_START`, holds ~60 s so another radio can scan for the
  SSID, reads `STA_LIST`, and **always** sends `BSS_STOP` — including on the
  not-started branch, so a missed branch cannot leave a radio on air. The
  external oracle is prepared: `system_profiler SPAirPortDataType` sees four
  networks here and `RT1176-UAP-TEST` is absent, so a later sighting means
  something.
- **WPA2** — AKMP / cipher / passphrase TLVs on the same builder, and with them
  the repo's standing SSID/PSK rule.
- Why a partial request *kills the port* rather than being rejected. Inside a
  closed blob, and no longer blocking.

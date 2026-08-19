# The IW416 IEEE-PS sleep handshake: NXP's contract vs ours (W12 reference)

Date: 2026-08-19. Source: NXP MCUXpresso SDK `middleware/wifi_nxp/` in
`~/Development/mcuxsdk-ws/` (read-only study; nothing vendored — the SDK is
LA_OPT-licensed and must never enter this repo). Our driver:
`~/Development/M2Radio/iw416/`.

Written during W12's investigation of fault #5. **It documents the firmware
contract, which is true regardless of how fault #5 is finally attributed** —
that attribution is still open at the time of writing.

## Why this matters

Two firmware comments in NXP's tree describe failure modes that look exactly
like the RX/event deaths this project keeps meeting:

* `mlan_scan.c:968-975` — *"Fw will delay all events if handshake is not done
  yet after ps sleep event."* An unresolved sleep handshake **stalls the
  firmware's whole event path**. NXP completes the handshake even between
  scan channels rather than let it hang.
* `wifi.c:635-645` — after a `HostCmd_RESULT_PRE_ASLEEP` rejection, a delay is
  needed *"to ensure the wakeup interrupt is re-enabled by FW before driver
  resends the command."* Mishandling the handshake can leave the firmware's
  **wakeup interrupt disabled**.

## NXP's algorithm (4 states, deferral, re-validation)

```
enum wlan_ps_state { AWAKE, PRE_SLEEP, SLEEP_CFM, SLEEP }   // wlan.h:486-492

EVENT_PS_SLEEP (driver task, NOT an ISR):        // mlan_glue.c:5876-5900
    if ps_state == PRE_SLEEP: warn; return       // idempotent guard
    ps_state = PRE_SLEEP; wakelock_get()
    post WIFI_EVENT_SLEEP to the wlcmgr queue    // deferral: nothing on the bus yet

wlcmgr task, WIFI_EVENT_SLEEP:                   // wlan.c:7815 -> wifi_pwrmgr.c:407-450
    take command_lock                            // gate 1: serialize vs all host cmds
    if ps_state != PRE_SLEEP: put lock; return   // gate 2: RE-VALIDATE -> cancel
    ps_state = SLEEP_CFM
    lock sleep_rwlock.write_mutex                // gate 3: no NEW tx may start
        send 0xE4 action=SLEEP_CONFIRM resp_ctrl=1
        wait for the response
    unlock write_mutex

SLEEP_CONFIRM response:                          // wifi_pwrmgr.c:647-674
    RWLockWriteLock(sleep_rwlock, forever)       // gate 4: BLOCKS until in-flight
                                                 //         tx/cmd have drained
    ps_state = SLEEP                             // "asleep" declared ONLY here

TX/CMD path:                                     // wifi.c:3199-3209, 3494-3504
    read-lock (first reader wakes the card, up to 3 retries)
    if ps_state in {SLEEP_CFM, SLEEP}: unlock, REQUEUE the packet, return

any RX / cmd-resp / non-PS_AWAKE event while SLEEP -> unlock; AWAKE
EVENT_PS_AWAKE while PRE_SLEEP -> unlock; AWAKE   // cancels the queued confirm
```

## Where our driver diverges

| | NXP | M2Radio (`e03d0c5`) |
|---|---|---|
| States | 4 | 2 (`PS_AWAKE`/`PS_SLEEPING`, Iw416.h:263) |
| Confirm context | deferred via task queue | inline in the event demux (Iw416.cpp:1228) |
| Re-validate at send | yes, cancels | none |
| Serialize vs cmds/TX | command_lock + write_mutex | none |
| **"Asleep" declared** | **on the RESPONSE, after TX drains** | **on write success** (Iw416.cpp:1348) |
| TX while confirming | refused + requeued | `wakeCardIfSleeping()` writes HOST_POWER_UP |
| HOST_POWER_UP clear | **never** clears it | clears on PS_AWAKE (Iw416.cpp:1241) |

## What actually applies to us

Our driver is **single-threaded and polled**: `sendDataFrame()` and
`serviceLink()` cannot overlap, so NXP's mutex/rwlock machinery has no direct
analogue need — that concurrency is structurally absent here. The divergences
that DO carry real risk:

1. **We declare `PS_SLEEPING` on write-success, not on the ack.** In the
   window between our confirm write and the firmware's `0x80E4` ack, our own
   `wakeCardIfSleeping()` believes the card is asleep and can write
   `HOST_POWER_UP` into a card mid-handshake. NXP makes this window
   unreachable. This is the sharpest divergence and the first thing to fix if
   fault #5 is confirmed PS-related.
2. **We never re-validate.** A `PS_AWAKE` sitting in the same event batch
   cannot cancel a confirm we have already sent inline.
3. **Our confirm ack is consumed opportunistically** by the next demux pass
   (and `waitCmdResp`'s discard loop can swallow it — the known W10
   clear-hole). Given "fw delays all events until the handshake is done", an
   ack we never drain is a candidate wedge.

## The trap this reference exists to prevent

**Do NOT implement "skip the confirm while TX/RX is pending."** That was
W12's initial hypothesis and it is wrong: NXP has **no** TX/RX-pending gate
on the confirm (`wlan_get_tx_pending`, `wlan_ps_cond_check`: zero hits; the
wr/rd bitmaps are never correlated with `ps_state`). The only path that never
sends is the one where the firmware already timed out and self-woke. Because
the firmware delays all events until the handshake completes, deliberately
withholding a confirm under load would risk *causing* the very event stall we
are chasing. The correct shape is **always confirm — but serialize it, and
declare sleep only on the ack**.

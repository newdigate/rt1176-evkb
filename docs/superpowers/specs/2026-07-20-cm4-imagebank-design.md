# CM4 Image Bank — a small runtime manager over `Multicore::switchImage`

**Date:** 2026-07-20
**Status:** Approved (brainstorming) → ready for `writing-plans`
**Builds on:** the D7 hot-swap milestone (`cm4_hotswap_test`, `cm4_hotswap2_test`)
and `Multicore::switchImage` (cores `1a16acb`). CM4 bring-up Phases 1–4 + D7
are all HW-verified; this is a **new capability** on top of them.
**Skill:** governed by `cm4-bringup` (triangulate → QEMU-gate → silicon probe;
silicon wins; GPL one-way firewall).

---

## 0. Scope & decisions (locked in brainstorming)

A small CM7-side class, **`Cm4ImageBank`**, that turns the raw three-call
`Multicore` surface (`begin`/`switchImage`/`restart`) into a **registry of CM4
images** you switch between by handle. It is pure bookkeeping over the existing
HW-verified boot machinery — **no `Multicore` change, no qemu2 change.**

Three decisions were locked via the brainstorming questions:

1. **Unified residency model.** The bank tracks, per ITCM slot, which image is
   currently staged there. `switchTo(handle)` does a **fast VTOR flip**
   (`Multicore::switchImage`, no copy) when the target is already resident, else
   **stages + boots** it (`Multicore::begin`) and **evicts** any image sharing
   its slot. Distinct-slot images stay co-resident; same-slot images page.
   (Superset of "co-resident only" and "paged bank".)
2. **Hardware-level readiness.** `switchTo` returns `bool` = *the CM4 left
   reset* (`running()` within the boot timeout), exactly like `begin()`. **No MU
   coupling**; images need no ready-token convention. Any app-level "image is
   serving" handshake is the caller's job, using the existing `MU` global.
3. **Uniform ITCM slots.** The 128 KB ITCM is divided into **N fixed slots** of
   `CM4_SLOT_SIZE` (one build-side knob; `N = 0x20000 / CM4_SLOT_SIZE`). Each
   image is linked for its slot's base via a generated linker script; the bank
   consumes the matching backdoor `stageAddr`. The *runtime* bank stays
   **layout-agnostic** (it only receives + validates `stageAddr`); all density
   lives in the build helper.

**The constraint that shapes everything:** CM4 images are **position-dependent**
— each is linked for a fixed ITCM `ORIGIN` and resolves its own absolute
pointers there (`Multicore.h:10-14`, `cm4_a.ld:3-9`), so a `stageAddr` **must**
equal the image's build-time `ORIGIN` backdoor alias. The bank therefore cannot
allocate or relocate; it registers, validates, tracks residency, and switches.

**Non-goals (YAGNI):** app-level MU ready handshake in the bank (caller owns it);
fully-packed variable-size layout (a staged/two-pass build — rejected in favour
of uniform slots); cross-image DTCM persistence (DTCM is time-multiplexed,
re-init per boot); heap/dynamic image registration; un-booting/parking a slot
without booting another image; SD-loaded images (captured as future work, §8).

---

## 1. Goal & success criteria

**Goal:** register several build-fixed CM4 images and switch the running CM4
among them by handle, with the bank transparently choosing a fast no-copy VTOR
flip (resident) or a stage + evict (not resident), reporting hardware-level
readiness.

**v1 success = QEMU gate green (asserted tokens, stable 3×) AND the EVKB
clean-boot probe green with the asserted tokens byte-identical**, via one gate
(`cm4_imagebank_test`) that exercises **all three unified behaviours**:

- **Co-residency + no-copy flip:** with images A/B/C in distinct slots,
  `switchTo(A)` after A→B→C is a *fast flip* (A stayed resident) and re-runs A's
  identity; `isResident(A/B/C)` all true.
- **Eviction / paging:** image D shares A's slot; `switchTo(D)` evicts A
  (`isResident(A)`→false, `isResident(D)`→true), then `switchTo(A)` **re-stages**
  A (not a flip) and re-runs A's identity.
- **The correct image actually ran each time** — each image streams a **distinct
  identity** over the MU (`idA=0xA1A1A1A1`, `idB=0xB2B2B2B2`, `idC=0xC3C3C3C3`,
  `idD=0xD4D4D4D4`); the CM7 reporter asserts the identity *sequence* **and** the
  `isResident()` flag transitions.

Identity + residency together are un-fakeable: a stale/duplicate MU token cannot
satisfy the identity check (distinct identities, not counters — the
`cm4_hotswap2_test` precedent), and a wrong flip-vs-stage decision shows up in
the residency log.

---

## 2. Ground truth (verified this session)

| Fact | Source |
|---|---|
| `Multicore` surface: `begin(image,bytes,stageAddr)→bool` (stages via a word copy, programs `GPR0/1`, `BT_RELEASE_M4` **first call only**, returns true when `STAT_M4CORE.UNDER_RST→0`); `switchImage(stageAddr)` (reprogram `GPR0/1` + pulse `SW_RESET`, **no copy**, no-op unless already released); `restart()`; `running()`; `bootAddress()` | `cores/imxrt1176/Multicore.{h,cpp}` (HW-verified; `switchImage` = `1a16acb`) |
| First `begin()` performs the one-time write-1-only `BT_RELEASE_M4`; `switchImage` presupposes a prior release (guaranteed — residency is only set *after* a `begin()`) | `Multicore.cpp:27-70,93-104` |
| Staged blob = **ITCM span only**: `.isr_vector` + `.text` + `.rodata` + `.data` **LMA** (initialisers appended after `.text` via `AT> ITCM`); `objcopy -O binary` yields exactly those bytes. Current images = **700 B**. | `cm4_hotswap2_test/cm4/cm4_a.ld:24-44`; `build/*.cm4.bin` |
| `.data` runtime copy, `.bss`, heap, and the **stack** (`__StackTop = 0x20020000`, top of DTCM) live in **DTCM `0x20000000`** — *not* in the staged blob, *not* counted against ITCM packing. DTCM is shared/time-multiplexed (one image runs at a time; re-init per boot). | `cm4_a.ld:18,36-53` |
| CM4 ITCM = 128 KB at private `0x1FFE0000`, aliased to the OCRAM-M4 backdoor `0x20200000` (= `CM4_BOOT_ADDRESS`); images are **position-dependent** (linked for a fixed `ORIGIN`, relocate VTOR to it) | `Multicore.h:10-14`; `cm4_a.ld:3-9` |
| Image embedding: `teensy_add_cm4_image(NAME LINKER <ld> SOURCES … [INCLUDE_DIRS…] [DEFINES…])` compiles→links→`objcopy -O binary`→`cm4_bin2header.cmake` → `static const uint32_t NAME[]`; `teensy_target_link_cm4_image(TARGET NAME)` exposes it to the CM7 elf. Called once per image (twice for two). | `teensy-cmake-macros/CMakeLists.include.txt:461,532`; `cm4_bin2header.cmake` |
| qemu2 maps the **full 256 KB** `ocram_m4` backdoor and re-reads `GPR0/1` fresh on each CM4 boot, accepting any in-window `stageAddr` (verified: hotswap2's `0x20210000`) | `rt1176-cm4-boot-mu` memory; `cm4_hotswap2_test` HW-verified |
| SD reads are HW-verified on the CM7 (SdFat/USDHC1, incl. in-ISR file streaming) — the basis for the §8 future SD-loaded images | memories `rt1176-sd-usdhc`, `rt1176-audioplaysdwav` |

---

## 3. Architecture

### 3.1 The bank — public API (`cores/imxrt1176/Cm4ImageBank.{h,cpp}`)

Pure CM7-side bookkeeping over the global `extern MulticoreClass Multicore`. It
never allocates ITCM — each image's `stageAddr` is its build-fixed link base
(backdoor alias), passed at `add()`.

```cpp
/* Cm4ImageBank.h — a tiny registry over Multicore.switchImage(): register
 * several build-fixed CM4 images and switch the running CM4 between them by
 * handle. Distinct stage addresses stay co-resident (fast VTOR flip); images
 * sharing a stage address page (re-stage + evict). CM7-side; uses the global
 * Multicore. Public domain (author: Nicholas Newdigate). */

/* The CM4 ITCM backdoor window (code images only; the DTCM half is data). */
#define CM4_ITCM_BACKDOOR_BASE   CM4_BOOT_ADDRESS   /* 0x20200000 */
#define CM4_ITCM_BACKDOOR_SIZE   0x20000u           /* 128 KB      */

struct Cm4ImageDesc {
    const void *blob;       // embedded image in flash (e.g. cm4_hs2_a[])
    uint32_t    bytes;      // image length
    uint32_t    stageAddr;  // build-fixed backdoor alias = image's link base
    const char *name;       // optional, debug only (may be nullptr)
};

class Cm4ImageBank {
public:
    // Register an image. Returns a handle >= 0, or -1 on error: table full,
    // stageAddr/bytes outside the 128K ITCM backdoor window, or a partial
    // overlap with a *different* slot (same stageAddr = same slot, allowed).
    int  add(const void *blob, uint32_t bytes, uint32_t stageAddr,
             const char *name = nullptr);

    // Boot/switch the CM4 into `handle`. Fast VTOR flip (Multicore.switchImage)
    // if it is already staged in its slot and the CM4 is running; otherwise
    // (re)stage + boot via Multicore.begin(), evicting any same-slot image.
    // Returns true once the CM4 has left reset (hardware-level ready), false
    // on a bad handle or a reset timeout.
    bool switchTo(int handle);

    int  current() const { return current_; }  // running handle, -1 if none
    bool isResident(int handle) const;          // its slot currently holds it
    int  count() const { return n_; }
    const char *name(int handle) const;         // debug label, "" if none

private:
    static constexpr int MAX_IMAGES = 16;       // cap on *registered* images
    Cm4ImageDesc imgs_[MAX_IMAGES];
    bool         resident_[MAX_IMAGES] = { false };
    int          n_ = 0;
    int          current_ = -1;
};
```

`MAX_IMAGES` caps *registered* images and is **independent of the slot count**
(same-slot images page, so registered images can exceed slots); one-line bump.

**Typical use:**

```cpp
Cm4ImageBank bank;
int a = bank.add(cm4_a, sizeof(cm4_a), CM4_SLOT_STAGE(0), "A");
int b = bank.add(cm4_b, sizeof(cm4_b), CM4_SLOT_STAGE(1), "B");
bank.switchTo(a);   // first call → begin() stages+boots A (does BT_RELEASE)
bank.switchTo(b);   // distinct slot → begin() stages+boots B (A stays resident)
bank.switchTo(a);   // A still resident → fast VTOR flip, no copy
```

### 3.2 `add` / `switchTo` / residency (internals)

```cpp
int Cm4ImageBank::add(blob, bytes, stageAddr, name) {
    if (n_ >= MAX_IMAGES)                                          return -1;
    if (stageAddr < CM4_ITCM_BACKDOOR_BASE ||                     // fit window
        stageAddr + bytes > CM4_ITCM_BACKDOOR_BASE + CM4_ITCM_BACKDOOR_SIZE)
                                                                  return -1;
    for (int i = 0; i < n_; i++)                                  // partial overlap
        if (imgs_[i].stageAddr != stageAddr &&                    // with a *different*
            overlap(stageAddr, bytes, imgs_[i].stageAddr, imgs_[i].bytes))
                                                                  return -1; // slot = bug
    imgs_[n_] = {blob, bytes, stageAddr, name}; resident_[n_] = false;
    return n_++;
}

bool Cm4ImageBank::switchTo(int h) {
    if (h < 0 || h >= n_) return false;
    bool ok;
    if (resident_[h] && Multicore.running()) {                    // FAST PATH: VTOR flip
        Multicore.switchImage(imgs_[h].stageAddr); ok = Multicore.running();
    } else {                                                      // STAGE PATH: copy + boot
        ok = Multicore.begin(imgs_[h].blob, imgs_[h].bytes, imgs_[h].stageAddr);
        if (ok) {
            for (int i = 0; i < n_; i++)                          // evict same-slot siblings
                if (imgs_[i].stageAddr == imgs_[h].stageAddr) resident_[i] = false;
            resident_[h] = true;
        }
    }
    if (ok) current_ = h;
    return ok;
}
```

- **Slots are derived from `stageAddr`:** same `stageAddr` ⇒ same slot
  (page/evict); distinct non-overlapping ⇒ co-resident; partial overlap ⇒
  `add()` returns −1.
- **First `switchTo`** (nothing resident) → stage path → the one-time
  `BT_RELEASE_M4`. Thereafter switching to a resident image is a pure flip.
- `switchImage`'s "already released" precondition holds because `resident_[h]`
  is only set true *after* a `begin()`; the `&& Multicore.running()` guard backs
  it up.
- **Eviction is slot-scoped:** staging `h` clears residency for every image
  sharing `h`'s `stageAddr`, then sets `h`. Distinct-slot images keep theirs.
- `switchTo(current)` when resident is a fast flip = reboots it in place
  ("reset this image").
- **Errors never corrupt state:** bad handle / full table / out-of-window /
  overlap → −1 or `false`; a `begin()`/flip reset-timeout → `false`,
  `current_`/`resident_` unchanged (caller may retry).

### 3.3 Build-side uniform slots

- **`CM4_SLOT_SIZE`** — the single knob (a CMake variable, power-of-two; default
  `0x1000` = 4 KB → 32 slots). Floor = the largest image rounded up to the CM4
  vector-table alignment (~`0x400`); today's 700 B images fit any slot ≥ 1 KB.

  | Slots N | `CM4_SLOT_SIZE` | Max image / slot |
  |---|---|---|
  | 2 | 64 KB (`0x10000`) | ~64 KB (today's hotswap2 layout) |
  | 4 | 32 KB (`0x8000`) | ~32 KB |
  | 8 | 16 KB (`0x4000`) | ~16 KB |
  | 16 | 8 KB (`0x2000`) | ~8 KB |
  | 32 | 4 KB (`0x1000`) | ~4 KB |

  All images share one `slotSize`, so N is bounded by the *largest* image; the
  count tracks `128 KB / (largest image rounded to a power-of-two slot)`.

- **`cm4_slot.ld.in`** — today's `cm4_a.ld` with two placeholders (DTCM
  unchanged, shared):
  ```
  ITCM (rx) : ORIGIN = @CM4_ITCM_ORIGIN@, LENGTH = @CM4_SLOT_SIZE@
  DTCM (rw) : ORIGIN = 0x20000000,        LENGTH = 128K
  ```
- **Slot → linker-script generation** — computes
  `ORIGIN = 0x1FFE0000 + slotIndex*CM4_SLOT_SIZE`, `configure_file`s the
  template → `cm4_slot<k>.ld`, links the image with it. Shape is a **plan-time
  choice**: either a thin `teensy_add_cm4_slot_image(NAME SLOT <k> SOURCES …)`
  wrapper, or an optional `SLOT <k>` keyword added to `teensy_add_cm4_image`
  (which today *requires* an explicit `LINKER`). Either way, images that don't
  use slots keep the existing `LINKER` path **byte-identical** (the macro's
  2 B-compare discipline).
- **`CM4_SLOT_STAGE(k)` = `CM4_ITCM_BACKDOOR_BASE + (k)*CM4_SLOT_SIZE`** — the
  `stageAddr` the caller hands `bank.add(...)`. The **same** `CM4_SLOT_SIZE` is
  compiled into the CM7 firmware, so the linker `ORIGIN` and the runtime macro
  share one value; **slot index is the single source of truth** binding linker
  layout ↔ bank `stageAddr`.

---

## 4. The gate — `cm4_imagebank_test`

One artifact in the `cm4_*_test` house shape: a CM7 Arduino **reporter**
(`Cm4ImageBank`, drives `switchTo`, reads the MU identity tokens, logs
`isResident` flags, prints `token=HEX` + `IMAGEBANK=PASS/FAIL`, **never touches
a peripheral**), four CM4 images from **one source** (`cm4/main_cm4.c`, identity
via `-DHS_IDENTITY`, reusing `cm4_hotswap2_test`), shared `startup_cm4.S`,
generated per-slot `.ld`, a `run_qemu.sh` (gate-lib), both transcripts checked
in.

**Images:**

| Handle | Slot | `stageAddr` | Identity |
|---|---|---|---|
| A | 0 | `CM4_SLOT_STAGE(0)` = `0x20200000` | `0xA1A1A1A1` |
| B | 1 | `CM4_SLOT_STAGE(1)` | `0xB2B2B2B2` |
| C | 2 | `CM4_SLOT_STAGE(2)` | `0xC3C3C3C3` |
| D | **0** (shares A) | `CM4_SLOT_STAGE(0)` | `0xD4D4D4D4` |

Each image `mu_send`s ready + its identity, then parks in **WFI** (so it isn't
fetching its own code when the CM7 overwrites the backdoor just before a reset —
the `cm4_hotswap_test` pattern).

**CM7 sequence + assertions** (identities via MU + `isResident()` via CM7 log):

1. `switchTo(A)`→idA (stage; the `BT_RELEASE` edge), `switchTo(B)`→idB,
   `switchTo(C)`→idC.
2. `switchTo(A)` → **fast flip** (A resident across B/C — distinct slots) → idA.
   *Proves co-residency + no-copy switch.*
3. `switchTo(D)` → slot 0 → **evicts A** (`isResident(A)`→false,
   `isResident(D)`→true) → idD.
4. `switchTo(A)` → not resident → **re-stage** → idA. *Proves eviction/paging.*

`IMAGEBANK=PASS` ⇔ the full identity sequence matches **and** the residency-flag
transitions match (B/C stay resident throughout; A resident → evicted-by-D →
re-staged).

**No qemu2 change** — uses the existing `Multicore`/backdoor/`GPR0-1`/MU models;
the full 128 KB ITCM backdoor is mapped and `GPR0/1` re-read fresh per boot. GPL
firewall trivially clean (no qemu2-side code at all).

---

## 5. License & firewall

- **`Cm4ImageBank.{h,cpp}`** — clean-room, author-original, **public domain**,
  matching `Multicore`/`MessagingUnit` ("Public domain, author: Nicholas
  Newdigate"). No adapted third-party logic.
- **CM4 test image + startup + linker template** — reuse the HW-verified
  `cm4_hotswap2_test` sources (this project's own public-domain code).
- **GPL one-way firewall:** no qemu2 change, so trivially clean.
- Extend `evkb/tools/license-audit.sh` `GATES` with `cm4_imagebank_test` in the
  **same change**; require `LICENSE-AUDIT: PASS`. Macro-built CM4 sources are
  covered by the existing `-MMD` depfile walk.

---

## 6. Verification sequence

1. **QEMU red:** stub the bank's decision (e.g. always stage, or invert the
   residency check) so the co-residency/eviction assertions fail → no
   `IMAGEBANK=PASS`; runner exit 1.
2. **QEMU green:** full bank → identity sequence + residency transitions
   correct; save `transcript_qemu.txt`; stable 3×.
3. **Repo regression:** the `Multicore`-consuming gates still build/pass
   (`cm4_hotswap_test`, `cm4_hotswap2_test`); no `Multicore`/qemu2 delta to
   regress.
4. **License audit:** `LICENSE-AUDIT: PASS` with `cm4_imagebank_test` covered.
5. **Hardware (final arbiter):** `clean_boot.scp` (CM4 held: `SCR=0`/
   `STAT_M4=1`), flash, dispatch, capture VCOM — **wiring-free**. Confirm
   `IMAGEBANK=PASS`, identities byte-identical to QEMU, residency transitions as
   asserted; save `transcript_hw_evkb.txt`.
6. **Docs + commit:** README, roadmap "new capability" entry + session log,
   memory update (`rt1176-cm4-boot-mu`, or a new `rt1176-cm4-imagebank`), per
   the session-log discipline.

**Probe trigger (why HW still runs despite low risk):** this reuses the
**already-HW-verified** boot/switch/backdoor mechanics — no new register
dependency, no qemu2 model, no new clock or memory-alias fact — so the silicon
risk is low. But "co-resident + evict across ≥3 slots" is a **new usage
pattern** of the backdoor (more slots than hotswap2's two), so the clean-boot
probe runs: only silicon confirms the whole 128 KB window pages as modelled.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Stale/duplicate MU token false-passes an identity check | Distinct per-image identities (not counters); the reporter asserts the full sequence (`cm4_hotswap2_test` precedent) |
| A wrong flip-vs-stage decision passes silently | Residency logged and asserted alongside identities; the eviction case (D shares A's slot) forces a re-stage that a flip-only bug would fail |
| Position-dependence: a `stageAddr` ≠ the image's link `ORIGIN` boots garbage | Slot index is the single source of truth (linker `ORIGIN` ↔ `CM4_SLOT_STAGE`); the bank's window + overlap validation catches layout errors; the gate boots each image and checks its identity |
| More slots than hotswap2 — a higher backdoor address pages wrong on silicon | The wiring-free clean-boot probe boots images in slots 0/1/2 and asserts their identities; silicon is the arbiter |
| Blob/buffer lifetime with lazy staging | v1 blobs are in-flash `static const` arrays (always valid); documented as a constraint for the future SD path (§8) |
| `MAX_IMAGES` too small | Compile-time constant, one-line bump; `add()` returns −1 (never overflows) |

---

## 8. Deferred / future

### Future: SD-loaded CM4 images (natural next milestone)

The bank is deliberately **source-agnostic** — `add(blob,…)` takes a
`const void*` and `begin()` copies from it, so `blob` may point at flash, OCRAM,
or SDRAM. With SD already HW-verified on the CM7, a thin **loader** layer becomes
possible: read a CM4 `.bin` from the card into a RAM buffer (or directly into the
slot's ITCM backdoor) and register it. Three hooks make it clean, the first of
which already exists:

1. **Source-agnostic `blob`** — present in this design; nothing to change.
2. **Optional "boot-in-place" `Multicore` entry** — to read straight into the
   backdoor and boot the *first* time without a copy (`switchImage` already
   covers *subsequent* boots but presupposes a prior release). A small future
   `Multicore` addition; avoids a double copy and the lazy-staging
   buffer-lifetime concern.
3. **Image-header convention** — a raw `.bin` is position-dependent but carries
   no record of its slot; loading *arbitrary* firmware wants a small header
   (`magic + intendedStageAddr + length + CRC`) so the loader verifies
   origin-match + integrity before releasing the core (a garbage vector table
   faults the CM4). The build already emits the exact `.bin`; the header is a
   thin wrapper.

Payoff: field-update the CM4 without re-flashing the CM7; a runtime-selectable
"app store" of CM4 workloads; combined with paging, effectively unlimited images
on SD with the working set paged into ITCM slots. **Explicitly out of scope for
this spec** — the bank should land and get HW-verified on its own first.

### Other deferred (YAGNI)

- App-level MU ready handshake in the bank (caller owns it via `MU`).
- Fully-packed variable-size ITCM layout (a staged/two-pass build).
- Cross-image DTCM persistence (a `NOINIT` shared region across boots).
- Un-booting / parking a slot without booting another image (no re-hold on
  silicon; `BT_RELEASE_M4` is write-1-only).

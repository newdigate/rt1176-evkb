# CM4 Two-Resident-Images Hot-Swap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** Two CM4 programs **co-resident** in ITCM at once; the CM7 switches the CM4's boot VTOR between them by reprogramming `IOMUXC_LPSR_GPR0/1` + re-pulsing `SW_RESET` — **without re-staging**. Proves the literal D7 "reboot at a *new* VTOR" and delivers fast switching between resident CM4 images.

**Builds on:** `cm4_hotswap_test` (swap-in-place, D7 core resolved). Same clean-boot probe discipline. **No qemu2 change** — the machine maps the full 256 K `ocram_m4` backdoor and `fsl_imxrt1170_cm4_boot()` re-reads `GPR0/1` fresh and accepts `0x20210000` (it's the system backdoor, not a rejected CM4-private-TCM VTOR; the reject range is `[0x1FFE0000, 0x20020000)`).

**Memory map (confirmed):** CM4 ITCM `0x1FFE0000` (128 K) is aliased to `ocram_m4[0]` = system `0x20200000`. Split it into two 64 K halves:
| Image | ITCM link base | Backdoor stage addr |
|-------|----------------|---------------------|
| A | `0x1FFE0000` (64 K) | `0x20200000` |
| B | `0x1FFF0000` (64 K) | `0x20210000` (= ITCM+0x10000) |

DTCM (`0x20000000`, 128 K) is **shared** — only one image runs at a time, so each boot re-inits its `.data`/`.bss`/stack there.

**Tech stack:** bare-metal C (CM4), a new `Multicore::switchImage()` core method, two CM4 linker scripts, teensy-cmake-macros (`DEFINES` + per-image `LINKER`), MU, gate-lib, LinkServer + `clean_boot.scp`.

**Repos:** `~/Development/rt1170/evkb` (the test + the `cores/imxrt1176` Multicore change — cores is a nested git repo under evkb; commit there).

---

### Task 1: `Multicore::switchImage()` + `cm4_hotswap2_test` (QEMU GREEN)

**Files:**
- Modify: `cores/imxrt1176/Multicore.h`, `cores/imxrt1176/Multicore.cpp`
- Create: `cm4_hotswap2_test/{CMakeLists.txt, toolchain/, cm4/main_cm4.c, cm4/startup_cm4.S, cm4/cm4_a.ld, cm4/cm4_b.ld, cm4_hotswap2_test.cpp, run_qemu.sh}`

- [ ] **Step 1: Add `switchImage` to `Multicore.h`** (after `restart()`):

```cpp
    /* Reboot the CM4 into an ALREADY-STAGED image at a (possibly different)
     * backdoor address by reprogramming GPR0/1 and re-pulsing SW_RESET WITHOUT
     * re-copying the image. begin() must have released the CM4 first (no-op
     * otherwise). Enables switching the boot VTOR between two co-resident CM4
     * images with no re-staging. */
    void switchImage(uint32_t stageAddr);
```

- [ ] **Step 2: Add the body to `Multicore.cpp`** (mirror `begin()` steps 2+4, minus staging and `BT_RELEASE`; place after `restart()`):

```cpp
void MulticoreClass::switchImage(uint32_t stageAddr)
{
    if (!released_) {
        return;
    }
    /* Re-point the CM4 boot VTOR (GPR0 = VTOR[15:3], GPR1 = VTOR[31:16]) at an
     * already-staged image, then reboot it -- no image copy. */
    IOMUXC_LPSR_GPR0 = stageAddr & 0xFFF8u;
    IOMUXC_LPSR_GPR1 = (stageAddr >> 16) & 0xFFFFu;
    boot_addr_ = stageAddr;
    multicore_barrier();
    SRC_CTRL_M4CORE = SRC_CTRL_M4CORE_SW_RESET;
    for (volatile uint32_t n = MULTICORE_SETTLE_SPINS; n; n--) {
        if (running()) {
            break;
        }
    }
}
```

- [ ] **Step 3: Verify the core change is non-breaking.** `switchImage` is additive (new method). Rebuild + re-run an existing CM4 gate that links the core (`cd ~/Development/rt1170/evkb/cm4_hotswap_test && cmake --build build && ./run_qemu.sh` — must still PASS) to confirm the `Multicore` compilation unit still builds/links.

- [ ] **Step 4: `cm4/main_cm4.c`** — identical to `cm4_hotswap_test/cm4/main_cm4.c` (copy verbatim: `mu_send(0, 0xCAFE0001u)` ready + `mu_send(0, HS_IDENTITY)` + WFI park; `SysTick_Handler`/`MU_IRQHandler` stubs). Position-independent w.r.t. the ITCM base (all symbolic).

- [ ] **Step 5: `cm4/startup_cm4.S`** — copy `cm4_hotswap_test/cm4/startup_cm4.S` verbatim. It relocates VTOR to `__isr_vector` (= `ORIGIN(ITCM)`, which the linker sets per-image to `0x1FFE0000` or `0x1FFF0000`), copies `.data` from its LMA, zeros `.bss`, calls main — so ONE startup source works for both images; the linker script differentiates them.

- [ ] **Step 6: Two linker scripts.** Copy `cm4_hotswap_test/cm4/cm4.ld` to `cm4/cm4_a.ld` and `cm4/cm4_b.ld`. In BOTH, change the ITCM `MEMORY` line to a 64 K half:
  - `cm4_a.ld`: `ITCM (rx) : ORIGIN = 0x1FFE0000, LENGTH = 64K`
  - `cm4_b.ld`: `ITCM (rx) : ORIGIN = 0x1FFF0000, LENGTH = 64K`
  Leave `DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K` and `__StackTop = ORIGIN(DTCM) + LENGTH(DTCM)` unchanged in both (shared DTCM). Everything else (`.text`/`.data`/`.bss` sections, `> ITCM`/`> DTCM AT> ITCM`) stays identical — the `.data` LMA follows `.text` within each image's own ITCM half.

- [ ] **Step 7: `CMakeLists.txt`** — two images from one source, different linker + identity:

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_hotswap2_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

teensy_add_cm4_image(cm4_hs2_a
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4_a.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
    DEFINES HS_IDENTITY=0xA1A1A1A1u)
teensy_add_cm4_image(cm4_hs2_b
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4_b.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
    DEFINES HS_IDENTITY=0xB2B2B2B2u)

teensy_add_executable(cm4_hotswap2_test cm4_hotswap2_test.cpp)
teensy_target_link_libraries(cm4_hotswap2_test cores)
target_link_libraries(cm4_hotswap2_test.elf stdc++)
teensy_target_link_cm4_image(cm4_hotswap2_test cm4_hs2_a)
teensy_target_link_cm4_image(cm4_hotswap2_test cm4_hs2_b)
```

- [ ] **Step 8: `cm4_hotswap2_test.cpp`** — the CM7 reporter:

```cpp
/*
 * cm4_hotswap2_test: two CM4 images co-resident in ITCM (A @ 0x1FFE0000 staged
 * at backdoor 0x20200000; B @ 0x1FFF0000 staged at 0x20210000). The CM7 boots
 * A, boots B (both now resident), then SWITCHES the boot VTOR back and forth
 * with Multicore.switchImage() -- reprogram GPR0/1 + re-pulse SW_RESET, NO
 * re-staging. idA==idA2 and idB==idB2 prove both images stayed resident and the
 * new-VTOR reboot works bidirectionally. The literal D7 + two-resident hot-swap.
 * Tokens per boot: ready=CAFE0001 then identity. PASS requires
 * idA==A1A1A1A1 && idB==B2B2B2B2 && idA2==A1A1A1A1 && idB2==B2B2B2B2.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_hs2_a.h"
#include "cm4_hs2_b.h"

#define STAGE_A 0x20200000u
#define STAGE_B 0x20210000u
#define WAIT_LONG 3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k); Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4)
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    Serial1.println();
}
static void ptimeout(const char *k) { Serial1.print(k); Serial1.println("=TIMEOUT"); }
static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--)
        if (MU.tryReceive(ch, out)) return true;
    return false;
}
/* Read a boot's {ready, identity}; return the identity (0xFFFFFFFF on timeout). */
static uint32_t read_identity(const char *tag, bool *ok)
{
    uint32_t r = 0, id = 0xFFFFFFFFu;
    if (wait_recv(0, &r)) { /* ready */ } else { ptimeout(tag); *ok = false; return id; }
    if (wait_recv(0, &id)) phex(tag, id); else { ptimeout(tag); *ok = false; }
    return id;
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4HOTSWAP2-GATE v1");
    MU.begin();
    bool ok = true;

    Multicore.begin(cm4_hs2_a, sizeof(cm4_hs2_a), STAGE_A);   /* stage+boot A */
    uint32_t idA = read_identity("idA", &ok);

    Multicore.begin(cm4_hs2_b, sizeof(cm4_hs2_b), STAGE_B);   /* stage+boot B (A stays resident) */
    uint32_t idB = read_identity("idB", &ok);

    Multicore.switchImage(STAGE_A);                          /* reboot A -- no re-stage */
    uint32_t idA2 = read_identity("idA2", &ok);

    Multicore.switchImage(STAGE_B);                          /* reboot B -- no re-stage */
    uint32_t idB2 = read_identity("idB2", &ok);

    bool pass = ok && idA == 0xA1A1A1A1u && idB == 0xB2B2B2B2u
                   && idA2 == 0xA1A1A1A1u && idB2 == 0xB2B2B2B2u;
    Serial1.println(pass ? "HOTSWAP2=PASS" : "HOTSWAP2=FAIL");
    Serial1.println("CM4HOTSWAP2-DONE");
}
void loop() {}
```

- [ ] **Step 9: `run_qemu.sh`** — copy `cm4_hotswap_test/run_qemu.sh`, rename to cm4_hotswap2, banner `CM4HOTSWAP2-GATE v1`, checks `idA=A1A1A1A1`, `idB=B2B2B2B2`, `idA2=A1A1A1A1`, `idB2=B2B2B2B2`, `HOTSWAP2=PASS`, `CM4HOTSWAP2-DONE`; stop-grep `CM4HOTSWAP2-DONE`; `chmod +x`.

- [ ] **Step 10: Build + gate GREEN.** `cmake -B build ... && cmake --build build && ./run_qemu.sh`. Expect all four identities correct. **If RED**: systematic-debugging — check the two images landed at their distinct ITCM halves (objdump the two `.cm4.elf` — A's vectors at 0x1FFE0000, B's at 0x1FFF0000), and that `begin(B, STAGE_B)` staged at 0x20210000 (not the default). Do NOT change qemu2 without a proven root cause.

- [ ] **Step 11: 3× stability; commit.** `cp cm4_hotswap2.uart transcript_qemu.txt`. Commit the cores change (in `cores/imxrt1176`) AND the test (in evkb): `Multicore: add switchImage() (reboot a resident CM4 image at a new VTOR, no re-stage)` and `cm4_hotswap2_test: two-resident CM4 hot-swap GREEN in QEMU`.

---

### Task 2: Clean HW probe + license + docs

- [ ] **Step 1: Clean HW probe (wiring-free).** Same discipline as `cm4_hotswap_test`: `clean_boot.scp` (CM4 held), flash `cm4_hotswap2_test.elf`, dispatch, capture VCOM. Expect `idA=A1A1A1A1`, `idB=B2B2B2B2`, `idA2=A1A1A1A1`, `idB2=B2B2B2B2`, `HOTSWAP2=PASS` — proving both images stayed resident and the VTOR switched between them on silicon. Copy `clean_boot.scp` into the test dir; save `transcript_hw_evkb.txt`. If it fails, systematic-debugging (does silicon boot from 0x20210000? objdump the images; read SRC/GPR state).
- [ ] **Step 2: license-audit.** Add `cm4_hotswap2_test:cm4_hotswap2_test` to GATES; run; PASS; commit.
- [ ] **Step 3: docs.** Roadmap: two-resident hot-swap HW-VERIFIED + the `switchImage` API + session-log entry. Memory: update `rt1176-cm4-boot-mu` (two-resident variant done; `switchImage`). README in the test dir. Commit.

---

## Self-review
- Reuses the proven swap-in-place pieces (main_cm4.c, startup, clean-boot probe) + the existing `begin(image,bytes,stageAddr)` staging-at-a-nonstandard-address. Only genuinely new: `switchImage()` (a 6-line method) + two 64 K ITCM linker halves. No qemu2 change (backdoor RAM + fresh-GPR read already model it). Both `begin(B,STAGE_B)` (new-VTOR boot) and the two `switchImage` calls (resident switch, no re-copy) are asserted; `idA==idA2`/`idB==idB2` proves residency, not just a reboot. DTCM shared safely (serial execution). Open detail: confirm both `.cm4.elf` vectors land at 0x1FFE0000 / 0x1FFF0000 (objdump) before trusting a QEMU pass.

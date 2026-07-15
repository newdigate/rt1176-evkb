# RT1176-EVKB — USB Host Mass Storage (read/write files on a USB flash drive)

**Date:** 2026-07-15
**Status:** design approved, spec under review
**Sub-project B of the USB host work** — `rt1176-usb-host-hid` brought up the EHCI
host controller (kbd/mouse/MIDI, all HW-verified) and explicitly deferred mass storage.
The controller is done; this session is the **MSC class driver + the FAT mount** on top.

Builds directly on:
- `rt1176-usb-host-hid` (EHCI host on USB_OTG2, bus `usbhost.0`, DMAMEM discipline,
  ISR-no-print) — spec `2026-07-08-rt1176-usb-host-hid-design.md`.
- `rt1176-usb-midi` (a host class driver that links **without** `bluetooth.cpp`) —
  spec `2026-07-09-rt1176-usb-midi-host-design.md`.
- `rt1176-sd-usdhc` (the **SdFat/FAT** stack reused as-is; MBR card-image recipe) —
  spec `2026-07-07-rt1176-sd-card-usdhc-design.md`.

---

## 1. Goal & scope

Enumerate a **USB flash drive** on **USB_OTG2** (EHCI host, already up) and
**read/write files** on a FAT-formatted drive.

**In scope**
- The BOT+SCSI block driver (`USBDrive`): raw 512-byte sector read/write.
- The FAT mount (`USBFilesystem`): auto-mount an existing FAT16/FAT32/exFAT volume
  (SdFat auto-detects), file write → read-back → dir-list.

**Out of scope (YAGNI — confirmed with the user)**
- Formatting a blank drive (`USBFilesystemFormatter`). We mount **pre-formatted**
  drives only. `USBFilesystemFormatter.cpp` is compiled **only if** the linker cannot
  `--gc-sections`-drop the unused `USBFilesystem::format()` path (a build detail, §4).
- Multi-LUN, UAS.
- Hub-attached MSC as a *gated* path. Flash drives are HS/FS → direct attach; the hub
  path stays HW-only (like HID's low-speed mouse), exercised opportunistically at most.

---

## 2. Reference map (verified against the working tree, 2026-07-15)

### 2.1 The driver is a three-file unit + a hard SdFat dependency
- `~/Development/USBHost_t36/MassStorageDriver.cpp` (1550 lines) — the driver + partition
  logic + `USBFilesystem` glue.
- Class declarations in `~/Development/USBHost_t36/USBHost_t36.h`
  (`#include <SdFat.h>` at `:2451`, `#include <FS.h>` at `:2459`, **unconditional**).
- `~/Development/USBHost_t36/utility/msc.h` — CBW/CSW + SCSI wire structs & constants.
- `USBFilesystemFormatter.cpp` lives at the **repo root** (not `utility/`);
  `utility/USBFilesystemFormatter.h` is the header.

### 2.2 Class structure
- `class USBDrive : public USBDriver, public FsBlockDeviceInterface` (`USBHost_t36.h:2508`).
  Dual inheritance is the crux: a `USBDrive*` is directly acceptable to SdFat's
  `FsVolume::begin(FsBlockDevice*, …)`. Overrides `readSector(s)`/`writeSector(s)` →
  `msReadBlocks`/`msWriteBlocks`, `sectorCount()`, `syncDevice()`, `isBusy()`, etc.
- `class USBFilesystem : public USBFSBase (: public FS)` (`:2807`). Holds an SdFat
  `FsVolume mscfs` (`:2883`) — the FAT/exFAT engine, **including SdFat's own 512-byte
  volume cache buffer**. Mount is automatic: `USBDrive::Task()` → `startFilesystems()`
  → `claimPartition()` → `mscfs.begin(pdevice, true, firstSector, numSectors)`
  (`MassStorageDriver.cpp:1485-1504`).
- `class MSCFile : public FileImpl` (`:2683`) wraps an SdFat `FsFile` behind the
  Arduino `File`/`FS` API.

### 2.3 Transport — Bulk-Only Transport + SCSI over the existing EHCI pipes
- `claim()` (`MassStorageDriver.cpp:73`) matches **interface** class 0x08 / subclass
  0x06 (SCSI) / protocol 0x50 (BOT); discovers bulk IN/OUT endpoints; creates two bulk
  pipes with `new_Pipe(...)`. **No new transfer abstraction** (`new_msc_transfer` does
  not exist — grep-confirmed): it reuses `queue_Data_Transfer`/`queue_Control_Transfer`.
- `msDoCommand(CBW, buffer)` (`:375`) — the 3-stage BOT engine: (1) command = queue the
  **31-byte CBW** on the bulk-OUT pipe; (2) data = queue on bulk IN/OUT per `CBW->Flags`;
  (3) status = `msGetCSW()` reads the **13-byte CSW**. Each stage **busy-waits on
  `volatile` flags** (`msOutCompleted`/`msInCompleted`) set by the EHCI ISR callbacks
  → from the sketch's view MSC is **synchronous/blocking with `yield()`**.
- SCSI set present: TEST UNIT READY (0x00), REQUEST SENSE (0x03), INQUIRY (0x12),
  START/STOP UNIT (0x1B), READ CAPACITY(10) (0x25), READ(10) (0x28), WRITE(10) (0x2A),
  REPORT LUNS (0xA0). `mscInit()` (`:261`) runs connect → StartStopUnit → wait-media →
  INQUIRY → READ CAPACITY. `msReset` is deliberately **not** called during init (breaks
  some SanDisk drives).
- Streaming READ path (`msReadSectorsWithCB`, used by the WAV player) invokes its
  callback from **ISR context** with a 250 ms guard — not needed by our gates.

### 2.4 DMA-relevant members of `USBDrive` (must be DMA-reachable → object is `DMAMEM`)
`Pipe_t mypipes[3]` (32-aligned, EHCI qH), `Transfer_t mytransfers[7]` (32-aligned, qTD),
`msSCSICapacity_t msCapacity` (8B), `msInquiryResponse_t msInquiry` (36B),
`msRequestSenseResponse_t msSense` (252B), `setup_t setup` (8B), `uint8_t report[8]`,
`uint8_t _read_sector_buffer1[512]`, `uint8_t _read_sector_buffer2[512]`.
**`USBFilesystem`** additionally holds SdFat's volume cache inside `mscfs` — also a bulk
DMA target during FAT ops → the `USBFilesystem` object must be `DMAMEM` too.

### 2.5 No `BluetoothController` references
`grep BluetoothController MassStorageDriver.cpp` → 0 (also `USBFilesystemFormatter.cpp`
→ 0). Link **without** `bluetooth.cpp`/`BluetoothConnection.cpp` (the MIDI-gate
simplification the HID gate couldn't take).

### 2.6 QEMU already ships the device — no new model
`usb-storage` (`TYPE_USB_STORAGE` core `hw/usb/dev-storage.c` + concrete device
`hw/usb/dev-storage-classic.c` + `usb_msd_scsi_info_storage` → `hw/scsi/scsi-disk.c`
over a `BlockBackend`) is **already compiled into the current
`~/Development/qemu2/build/qemu-system-arm`** (objects present; `strings` shows
`usb-storage`). `CONFIG_USB_STORAGE_CLASSIC/BOT/CORE=y` and `CONFIG_SCSI=y` are all
already set (the machine `select USB_CHIPIDEA` chains to `USB`, and CLASSIC is
`default y`). It handles all six SCSI commands the driver issues, enumerates
**high-speed direct** (`bcdUSB 0x0200`, bulk IN 0x81 / OUT 0x02), and has **no
`usb_version` knob** (can't force full-speed — not needed). No config flip, no rebuild
required *for the device*.

Bus id: `fsl-imxrt1170.c:999-1001` gives OTG2 (`i==1`) the device id `"usbhost"`, so its
child USB bus is `usbhost.0`; the single root port (`chipidea.c:295 portnr=1`) makes
`port=1` resolve and avoids QEMU's auto-hub.

---

## 3. Two hardware facts that shape the design

### 3.1 The M7 D-cache is OFF → no cache-maintenance branch needed
`startup.c` never enables the D-cache (no `SCB_EnableDCache`/`CCR` writes), and
`imxrt1176.h:782-783` defines `arm_dcache_delete`/`arm_dcache_flush_delete` as **no-op
stubs**. The `#if defined(__IMXRT1062__)` cache-maintenance branches in
`MassStorageDriver.cpp:593-595, 719-721` are correctly **skipped** for `__IMXRT1176__`;
**no `__IMXRT1176__` cache branch is required**. (Confirmed empirically: `usb_keyboard.c`,
`usb_serial.c`, `eeprom.c` all call these no-op stubs and are HW-verified.)

### 3.2 The stack lives in DTCM → the stack-resident CBW/CSW are the key trap
`imxrt1176.ld`: stack top `_estack = 0x20040000` (top of the 256 KB DTCM at
`0x20000000`); DMAMEM (`.bss.dma`) is in **OCRAM at `0x20240000`**. `msDoCommand`/
`msGetCSW` build the CBW/CSW as **stack locals** (DTCM) and hand their addresses to the
EHCI DMA. Every peripheral DMA master on this chip reaches only OCRAM/SDRAM, **not TCM**
(the whole DMAMEM discipline; SD SDMA proved it). So on silicon the OTG2 EHCI cannot read
the stack-resident CBW → the first SCSI command fails. **QEMU currently models DTCM as
DMA-reachable** (EHCI gets `&address_space_memory`), so the gate would **false-pass**.

Two coordinated fixes (below): patch the driver to stage CBW/CSW through DMAMEM, **and**
refine QEMU so the block gate actually tests that patch (red→green).

---

## 4. Components

### 4.1 Driver port (library-side, `~/Development/USBHost_t36`; **no core changes**)
- Compile `ehci enumeration hub memory MassStorageDriver` into each gate `.elf`. No
  `bluetooth.cpp`.
- **The one deliberate change — CBW/CSW-in-DMAMEM patch (`__IMXRT1176__`):** add two
  members to `USBDrive` — `msCommandBlockWrapper_t _cbw_dma;` and
  `msCommandStatusWrapper_t _csw_dma;` (they ride the already-DMAMEM object) — and in
  `msDoCommand()` `memcpy` the caller's stack CBW → `_cbw_dma` and queue `&_cbw_dma`; in
  `msGetCSW()` queue the CSW receive into `&_csw_dma` and validate its signature/tag from
  the member directly. ~6 lines across the two transport functions, guarded
  `#if defined(__IMXRT1176__)` so Teensy stays byte-identical. The **data stage** already targets a DMA-safe caller buffer (the SdFat
  cache in the FS gate, or the firmware's DMAMEM buffer in the block gate) — untouched.
- **`USBFilesystemFormatter.cpp`:** try to build **without** it first (mount+R/W never
  calls `format()`). If the linker reports an undefined `USBFilesystemFormatter` symbol
  (i.e. `USBFilesystem::format()` isn't gc-dropped), add `USBFilesystemFormatter.cpp` to
  the source list — it is self-contained (SdFat + `USBDrive` only) and pulls in no
  Bluetooth/EEPROM. Decide at link, not by guessing.

### 4.2 QEMU refinement — DTCM DMA-unreachable for OTG2 ("reflect silicon")
- Build a machine-local **"USB DMA view"** MemoryRegion in `fsl-imxrt1170.c`: an alias of
  system memory with the **TCM windows punched out** as read-0/write-nop "hole" regions
  at higher priority — **ITCM** `0x00000000`+256 KB and **DTCM** `0x20000000`+256 KB —
  each logging `LOG_GUEST_ERROR` on access (so `-d guest_errors` shows any DMA-to-TCM).
- Point the **OTG2 chipidea EHCI's `ehci->as`** at an `AddressSpace` over that view. The
  chokepoint is `hcd-ehci-sysbus.c:69` (`s->as = &address_space_memory;`) — all EHCI
  DMA (`dma_memory_read/write`, `qemu_sglist_init` in `hcd-ehci.c`) flows through
  `ehci->as`. Preferred mechanism: add an optional `dma_mr` link to the sysbus EHCI (or
  set it in `chipidea_realize`), and if present do `address_space_init(&s->dma_as,
  dma_mr, …); s->as = &s->dma_as;` (mirrors how the PCI EHCI already supports a `dma`
  MemoryRegion). Scope to **OTG2 only** (`i==1`); leave OTG1 on `&address_space_memory`.
- **Not circular:** the model reflects an **independently-established** silicon fact (TCM
  is not DMA-reachable by peripherals — SD SDMA/audio DMA all require DMAMEM). It is not
  built to match the driver patch, and **HW round-trip remains the final arbiter** (§5).
- **Regression:** OTG1 device gadget untouched → CDC/HID-device gates unaffected. HID/MIDI
  host gates DMA only from DMAMEM/OCRAM (`periodictable`, object pipes/buffers, `enumbuf`)
  → stay green. **Re-run** the HID, MIDI, and a device-mode (CDC/`usb_enum`) gate to
  confirm no regression. Generalizing the TCM hole to eDMA/USDHC/SAI is **out of scope**
  (bigger blast radius; each already uses DMAMEM so it is HW-correct regardless) — noted
  as future fidelity work.

### 4.3 Gate A — `usb_msc_block_test` (BOT/SCSI/`USBDrive`; the QEMU red→green)
- **Firmware:** `USBHost myusb; DMAMEM USBDrive myDrive(myusb); DMAMEM USBHub
  hub1/hub2(myusb);`. `setup()`: `Serial1.begin(115200); myusb.begin(); print
  MSC_BLOCK_BEGIN`. `loop()`: `myusb.Task()`; on first `myDrive` truthy print
  `MSC_CONNECT=<vid>:<pid>` + `MSC_CAP=<blocks>x<blksz>` (from `msDriveInfo`/READ
  CAPACITY); then the round-trip on a scratch LBA using `DMAMEM __attribute__((aligned(32)))
  uint8_t buf[512]`: **read original → write distinct pattern (`writeSectors`) → read
  back (`readSectors`) + memcmp → write original back (restore)** → print
  `MSC_BLOCK_WRITE=PASS`/`MSC_BLOCK_READ=PASS`. Save+restore keeps it **non-destructive**
  on real hardware. All prints from `loop()` context (MSC is blocking/polled).
- **Image:** bare raw, `qemu-img create -f raw usb.img 64M` (or `mkfile -n` sparse).
- **Attach:** `-drive if=none,id=stick,file=usb.img,format=raw -device
  usb-storage,drive=stick,bus=usbhost.0,port=1,removable=on -icount shift=auto -display
  none -serial file:"$OUT" -d guest_errors -D "$DBG"`.
- **Expected without the §4.1 patch (with §4.2 fidelity):** `MSC_CONNECT` prints
  (enumeration uses DMAMEM enum buffers) but block R/W fails → **RED**. **With** the patch
  → **GREEN**. That transition is the proof.

### 4.4 Gate B — `usb_msc_fs_test` (FAT mount + File API)
- **Firmware:** `DMAMEM USBDrive myDrive(myusb); DMAMEM USBFilesystem
  firstPartition(myusb); DMAMEM USBHub hub1/hub2(myusb);`. `loop()`: `myusb.Task()`; wait
  `while(!firstPartition)`; print `MSC_MOUNT=<label/type>`; then
  `firstPartition.open("/rttest.txt", FILE_WRITE)` → write a known payload → close →
  reopen `FILE_READ` → read back + memcmp → `open("/")` + `openNextFile()` dir walk →
  `MSC_FS_WRITE=PASS`/`MSC_FS_READ=PASS`/`MSC_FS_DIR=PASS`. Print `MSC_FS_BEGIN` after
  `begin()`.
- **Image:** **MBR-partitioned FAT16** via the `sd_fs_test` recipe verbatim — `mkfile -n
  512m img`; `DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage img
  | head -1 | awk '{print $1}')`; `diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT16"
  RTTEST 100%`; `hdiutil detach "$DISK"`. SdFat mounts **only MBR partition 1** (no
  superfloppy fallback) — a bare boot sector at LBA 0 will not mount.
- **Attach:** same `usb-storage` line as Gate A, with the MBR-FAT image.
- **Optional host interop (non-fatal):** re-attach the image after the run and `ls`/`cat`
  `rttest.txt` to show the firmware-written file really landed in the FAT.

### 4.5 Gate build/runner conventions (both gates, in `evkb/`)
- **CMake** = MIDI gate shape (drop `bluetooth.cpp`, keep `SdFat/src` + `SPI` include)
  **+** `sd_fs` SdFat shape (now **compile+link** SdFat: `import_arduino_library(SdFat
  <root> src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib src/iostream
  src/SpiDriver)` + `import_arduino_library(SPI ~/Development/SPI)`). USBHost sources via
  `target_sources(<name>.elf PRIVATE …)`. **No** PaulS_SD/`SD`. Per-gate
  `toolchain/rt1170-evkb.toolchain.cmake`; configure with
  `-DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake`. Start `build/` **clean**
  (`file(GLOB)`, no `CONFIGURE_DEPENDS`).
- **Runner** = MIDI shape: `QEMU=~/Development/rt1170/evkb/tools/qrun`, source
  `gate-lib.sh`, `gate_init` first, `gate_pid $P`, single QEMU run + a **bounded python
  poll** of the UART/dbg files, then a checker (inline `grep -q` like the SD gates, or a
  `check_*.py` like MIDI). `-icount shift=auto` for timer determinism. `.gitignore`:
  `build/`, `*.uart`, `*.dbg`, `usb.img`/`card.img`.

---

## 5. Hardware validation (the real arbiter)

- **Setup:** a real USB flash drive on OTG2 via an **OTG adapter that grounds the ID pin**
  (OTG2 VBUS is ID-gated hardware — R160/R162 DNP, no firmware hook; per
  `rt1176-usb-host-hid`). Flash via `LinkServer run MIMXRT1176:MIMXRT1170-EVKB <elf>`;
  markers over Serial1 (LPUART1) read with **pyserial** @115200 (never `cat`).
- **Block gate:** enumerate the stick; the save+restore round-trip proves raw R/W
  non-destructively.
- **FS gate — the money test:** write `/rttest.txt` on the board → read it back on the
  board (gate PASS) → **verify the file byte-exact on a PC** (mount the stick). Then the
  reverse: create a file on a PC, read it on the board — interop both ways. The PC
  round-trip is the truth an MSC gate cannot fully fake.
- **Reuse host lessons:** DMAMEM the driver **objects** (not just statics); **never
  `Serial1.print` from the USB ISR** (deadlock — MSC is blocking/polled so this is
  natural: all markers print from `loop()`); a real flash drive is HS/FS → direct attach;
  an LS device or bus-powered hub would need a `USBHub`.

---

## 6. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Stack-resident CBW/CSW unreachable by OTG2 DMA on silicon | **High** (near-certain) | §4.1 DMAMEM-scratch patch + §4.2 QEMU fidelity makes it a gate-tested red→green; HW confirms |
| `usb-storage` bulk multi-sector edge cases (residue, multi-qTD) not modeled | Low | scsi-disk is mature; MIDI proved bulk IN/OUT on this EHCI; contingency = QEMU model fix |
| `USBFilesystem::format()` drags `USBFilesystemFormatter` into the link | Low | build without it first; add the .cpp only if the symbol is undefined (§4.1) |
| QEMU TCM-hole regresses an existing gate that silently DMA'd from DTCM | Low | re-run HID/MIDI/device gates; a newly-failing gate = a **found latent bug** (desirable) |
| exFAT vs FAT | Low | test FAT16 (image recipe) + FAT32; exFAT works via SdFat opportunistically on HW |

---

## 7. Sequencing (phases → `writing-plans` → `subagent-driven-development`)

1. **Phase 1 — block layer + QEMU fidelity.** Port `MassStorageDriver.cpp` (no bluetooth);
   apply the CBW/CSW-in-DMAMEM patch; add the OTG2 DTCM-DMA-hole to QEMU; build
   `usb_msc_block_test`. Demonstrate **red** (patch reverted) → **green** (patch applied)
   under the QEMU fidelity change. Re-run HID/MIDI/device gates for no-regression.
2. **Phase 2 — FS mount.** Build `usb_msc_fs_test` (auto-mount, file write/read-back/dir
   on the MBR-FAT16 image). Green in QEMU.
3. **Phase 3 — hardware.** Block (save/restore) + FS round-trip verified on a PC.

Each phase must be **gate-green before HW**. Commit core/qemu/evkb changes to their own
repos (core from inside `cores/imxrt1176`, qemu2, evkb) — **push only when the user asks**.
Update `evkb/docs/NEXT-SESSION-PROMPT-USB-HOST-MSC.md` with progress; delete it from the
index when complete and all loose ends are tied.

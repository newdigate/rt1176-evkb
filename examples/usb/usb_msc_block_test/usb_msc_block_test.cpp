#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

USBHost myusb;
DMAMEM USBHub       hub1(myusb);
DMAMEM USBHub       hub2(myusb);
DMAMEM USBDrive     myDrive(myusb);   // carries mypipes/mytransfers + SCSI buffers -> DMAMEM

// Raw-sector round-trip buffers must be DMA-reachable (OCRAM) and 32-byte aligned.
DMAMEM static uint8_t orig[512] __attribute__((aligned(32)));
DMAMEM static uint8_t wbuf[512] __attribute__((aligned(32)));
DMAMEM static uint8_t rbuf[512] __attribute__((aligned(32)));

static const uint32_t SCRATCH_LBA = 2048;   // safe scratch sector

void setup() {
  Serial1.begin(115200);
  delay(10);
  myusb.begin();
  Serial1.println("MSC_BLOCK_BEGIN");

  // Wait for the drive to enumerate (bounded).
  elapsedMillis em = 0;
  while (!myDrive.available() && em < 8000) myusb.Task();
  if (!myDrive.available()) { Serial1.println("MSC_BLOCK_FAIL:no_drive"); return; }
  Serial1.printf("MSC_CONNECT=%04x:%04x\n", myDrive.getIDVendor(), myDrive.getIDProduct());

  // Force SCSI init (INQUIRY + READ CAPACITY etc.); bounded.  On silicon (and under
  // the later QEMU fidelity change) an unpatched driver fails here: the CBW is on
  // the stack (DTCM), unreachable by the OTG2 EHCI DMA master.
  em = 0;
  while (myDrive.checkConnectedInitialized() != 0 && em < 5000) myusb.Task();
  uint32_t blocks = myDrive.sectorCount();
  uint32_t bsz    = myDrive.msDriveInfo.capacity.BlockSize;
  Serial1.printf("MSC_CAP=%lux%lu\n", (unsigned long)blocks, (unsigned long)bsz);
  if (blocks == 0) { Serial1.println("MSC_BLOCK_FAIL:no_capacity"); return; }

  // Save original, write pattern, read back, verify, restore (non-destructive).
  if (!myDrive.readSector(SCRATCH_LBA, orig)) { Serial1.println("MSC_BLOCK_FAIL:read_orig"); return; }
  for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i ^ 0xA5);
  if (!myDrive.writeSector(SCRATCH_LBA, wbuf)) { Serial1.println("MSC_BLOCK_FAIL:write"); return; }
  Serial1.println("MSC_BLOCK_WRITE=PASS");
  memset(rbuf, 0, sizeof(rbuf));
  if (!myDrive.readSector(SCRATCH_LBA, rbuf)) { Serial1.println("MSC_BLOCK_FAIL:readback"); myDrive.writeSector(SCRATCH_LBA, orig); return; }
  if (memcmp(rbuf, wbuf, 512) != 0) { Serial1.println("MSC_BLOCK_FAIL:mismatch"); myDrive.writeSector(SCRATCH_LBA, orig); return; }
  Serial1.println("MSC_BLOCK_READ=PASS");
  myDrive.writeSector(SCRATCH_LBA, orig);   // restore
  Serial1.println("MSC_BLOCK_DONE");
}

void loop() {}

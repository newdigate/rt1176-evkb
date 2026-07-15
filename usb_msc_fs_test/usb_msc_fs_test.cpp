#include "Arduino.h"
#include "HardwareSerial.h"
#include "USBHost_t36.h"

USBHost myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBHub        hub2(myusb);
DMAMEM USBDrive      myDrive(myusb);
DMAMEM USBFilesystem firstPartition(myusb);   // holds SdFat's volume cache -> DMAMEM

static const char *PATH = "/RTTEST.TXT";
static const char  PAYLOAD[] = "RT1176 USB MSC round-trip 0123456789";
static char rbuf[64];

void setup() {
  Serial1.begin(115200);
  delay(10);
  myusb.begin();
  Serial1.println("MSC_FS_BEGIN");

  // Auto-mount happens inside myusb.Task() -> USBDrive::Task() -> startFilesystems()
  // -> USBFilesystem::claimPartition() -> mscfs.begin(...).  Wait (bounded) for it.
  elapsedMillis em = 0;
  while (!firstPartition && em < 10000) myusb.Task();
  if (!firstPartition) { Serial1.println("MSC_FS_FAIL:no_mount"); return; }
  Serial1.printf("MSC_MOUNT=FAT%d\n", firstPartition.mscfs.fatType());

  // Write.
  firstPartition.remove(PATH);   // ignore result; start clean
  File f = firstPartition.open(PATH, FILE_WRITE);
  if (!f) { Serial1.println("MSC_FS_FAIL:open_write"); return; }
  size_t n = f.write((const uint8_t *)PAYLOAD, sizeof(PAYLOAD) - 1);
  f.close();
  if (n != sizeof(PAYLOAD) - 1) { Serial1.println("MSC_FS_FAIL:short_write"); return; }
  Serial1.println("MSC_FS_WRITE=PASS");

  // Read back + verify byte-exact.
  File g = firstPartition.open(PATH, FILE_READ);
  if (!g) { Serial1.println("MSC_FS_FAIL:open_read"); return; }
  memset(rbuf, 0, sizeof(rbuf));
  int r = g.read(rbuf, sizeof(PAYLOAD) - 1);
  g.close();
  if (r != (int)(sizeof(PAYLOAD) - 1) || memcmp(rbuf, PAYLOAD, sizeof(PAYLOAD) - 1) != 0) {
    Serial1.println("MSC_FS_FAIL:mismatch"); return;
  }
  Serial1.println("MSC_FS_READ=PASS");

  // Dir-list the root; the written file must appear.
  File root = firstPartition.open("/");
  if (!root) { Serial1.println("MSC_FS_FAIL:open_root"); return; }
  bool found = false;
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    const char *nm = e.name();
    Serial1.printf("MSC_DIR_ENTRY=%s,%lu\n", nm ? nm : "?", (unsigned long)e.size());
    if (nm && (strcmp(nm, "RTTEST.TXT") == 0 || strcasecmp(nm, "RTTEST.TXT") == 0)) found = true;
    e.close();
  }
  root.close();
  Serial1.println(found ? "MSC_FS_DIR=PASS" : "MSC_FS_FAIL:dir_missing");
  Serial1.println("MSC_FS_DONE");
}

void loop() {}

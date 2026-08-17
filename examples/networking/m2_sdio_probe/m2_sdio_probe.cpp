// M.2 (J54) SDIO probe -- brings uSDHC1 up in SDIO mode and reads the card's
// identity off the wire.  Board map: docs/m2-evkb-revc3.md.
//
// WARNING: uSDHC1 carries BOTH the M.2 socket and the microSD slot J15 on this
// board.  Remove any microSD card before running this on hardware.
//
// A GREEN QEMU GATE DOES NOT MEAN THE MODULE WORKS.  QEMU has no SDIO function
// model -- it attaches an SD *memory* card, which by spec ignores CMD5, so the
// gate asserts the NO-IO-FUNCTION fallback.  That is a real test of the failure
// path, and nothing more.  Positive evidence lives in transcript_hw_evkb.txt.
#include <Arduino.h>
#include <SdioHost.h>

static SdioHost sdio;

static const char *statusName(SdioHost::Status s) {
    switch (s) {
        case SdioHost::OK:              return "ok";
        case SdioHost::NO_IO_FUNCTION:  return "no-io-function";
        case SdioHost::CMD_TIMEOUT:     return "cmd-timeout";
        case SdioHost::CMD_CRC:         return "cmd-crc";
        case SdioHost::CLOCK_UNSTABLE:  return "clock-unstable";
        case SdioHost::BAD_CIS:         return "bad-cis";
    }
    return "unknown";
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("RT1176 M.2 SDIO probe up");

    SdioHost::Status st = sdio.begin();
    // Always print a reason code, never a bare absence.  "Nothing found" is
    // also what a dead image produces, so the gate needs a positive token
    // proving we arrived here deliberately.
    Serial1.print("sdio_begin=");
    Serial1.print(statusName(st));
    Serial1.print(" rc=");
    Serial1.println((int)st);

    if (st == SdioHost::OK) {
        Serial1.print("io_functions=");
        Serial1.println(sdio.ioFunctionCount());
        Serial1.print("rca=0x");
        Serial1.println(sdio.rca(), HEX);
        Serial1.print("cccr_rev=0x");
        Serial1.println(sdio.cccrRevision(), HEX);

        uint16_t manf = 0, card = 0;
        SdioHost::Status cs = sdio.readManfId(&manf, &card);
        if (cs == SdioHost::OK) {
            // These come off the wire from the card's CIS.  The firmware has no
            // knowledge of them -- that is what makes this assertion real.
            Serial1.print("manfid=0x");
            Serial1.println(manf, HEX);
            Serial1.print("cardid=0x");
            Serial1.println(card, HEX);
        } else {
            Serial1.print("cis_error=");
            Serial1.println(statusName(cs));
        }
    }
    Serial1.println("probe_done");
}

void loop() {
    static uint32_t n = 0;
    // Heartbeat: proves the image is still running after the probe rather than
    // having wedged in it.  A fallback gate without this cannot tell "took the
    // fallback" from "died".
    Serial1.print("alive=");
    Serial1.println(n++);
    delay(200);
}

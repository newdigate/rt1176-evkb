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

// ---------------------------------------------------------------------------
// M.2 module power-up.  This is board/module knowledge, NOT SDIO-host
// knowledge, so it lives here rather than in M2Radio's generic sdio/ layer.
//
// GPIO_AD_16 -> J54 pin 23 (WIFI_RST_B), via level shifter U354 -- which is
// enabled (OE# tied to GND) and pointed A->B (DIR pulled to WL_3V3 through
// R1797), so whatever this pad does reaches the module.
//
// It matters that the firmware drives it: the net has exactly two nodes
// (R835.2 and U354.3) and NO pull resistor anywhere, on either the 3V3 or the
// 1V8 side.  At POR the pad is a high-Z input feeding the shifter, so pin 23
// is genuinely indeterminate until someone drives it.  The first hardware run
// on 2026-08-17 left it undriven and CMD5 timed out (int_status=0x18000,
// ERR|CTOE, with CC never setting).
//
// Pad defines are local because the core header stops at GPIO_AD_14.  Offsets
// are the RM's: SW_MUX_CTL_PAD_GPIO_AD_16 at 14Ch, IOMUXC base 0x400E8000.
// ALT5 = GPIO_MUX3_IO15 -> GPIO3_IO15 (RM 12.x pad table, line 20107).
#define M2_WIFI_RST_MUX (*(volatile uint32_t *)0x400E814Cu)
#define M2_WIFI_RST_BIT 15

static void m2ReleaseWifiReset() {
    M2_WIFI_RST_MUX = 5u;                          // ALT5 = GPIO3_IO15
    GPIO3_GDIR |= (1u << M2_WIFI_RST_BIT);         // output
    GPIO3_DR_CLEAR = (1u << M2_WIFI_RST_BIT);      // assert reset (active low)
    delay(10);
    GPIO3_DR_SET = (1u << M2_WIFI_RST_BIT);        // release
    delay(100);                                    // let the module boot
}

static const char *statusName(SdioHost::Status s) {
    switch (s) {
        case SdioHost::OK:               return "ok";
        case SdioHost::NO_IO_FUNCTION:   return "no-io-function";
        case SdioHost::CMD_TIMEOUT:      return "cmd-timeout";
        case SdioHost::CMD_CRC:          return "cmd-crc";
        case SdioHost::CLOCK_UNSTABLE:   return "clock-unstable";
        case SdioHost::BAD_CIS:          return "bad-cis";
        case SdioHost::CMD5_NO_RESPONSE: return "cmd5-no-response";
        case SdioHost::INIT_CLK_STUCK:   return "init-clk-stuck";
    }
    return "unknown";
}

static SdioHost::Status g_status = SdioHost::CMD5_NO_RESPONSE;

// Re-emitted periodically from loop().  The probe result is produced once in
// setup(), but on a board shared with another session you rarely get to attach
// a serial reader before boot -- and chasing the reset with LinkServer is a
// race that is simply not worth running.  Print it again on a timer instead.
static void reportProbe() {
    SdioHost::Status st = g_status;
    Serial1.print("sdio_begin=");
    Serial1.print(statusName(st));
    Serial1.print(" rc=");
    Serial1.println((int)st);

    Serial1.print("int_status=0x");
    Serial1.print(sdio.lastIntStatus(), HEX);
    Serial1.print(" r4=0x");
    Serial1.println(sdio.lastR4(), HEX);

    // Live bus levels sampled just before CMD5.  CMD has a 10K pull-up (R373
    // to NVCC_SD), so an idle healthy bus reads cmd_high=1.  cmd_high=0 means
    // something is holding CMD down -- a dead rail or an unpowered module --
    // and no amount of protocol work will help.
    uint32_t ps = sdio.lastPresState();
    Serial1.print("pres_state=0x");
    Serial1.print(ps, HEX);
    Serial1.print(" cmd_high=");
    Serial1.print((ps >> 23) & 1);
    Serial1.print(" dat_levels=0x");
    Serial1.print((ps >> 24) & 0xF, HEX);
    Serial1.print(" card_inserted=");
    Serial1.println((ps >> 16) & 1);

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
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("RT1176 M.2 SDIO probe up");

    m2ReleaseWifiReset();
    Serial1.println("m2_wifi_reset=released");

    SdioHost::Status st = sdio.begin();
    g_status = st;
    reportProbe();
    Serial1.println("probe_done");
}

void loop() {
    static uint32_t n = 0;
    // Heartbeat: proves the image is still running after the probe rather than
    // having wedged in it.  A fallback gate without this cannot tell "took the
    // fallback" from "died".
    Serial1.print("alive=");
    Serial1.println(n++);
    // Re-report every 5 s so a reader attached at any moment sees the result,
    // without having to win a race against the boot.
    if (n % 25 == 0) reportProbe();
    delay(200);
}

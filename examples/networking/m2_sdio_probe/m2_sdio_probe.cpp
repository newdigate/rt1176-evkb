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
// This now mirrors NXP's own BOARD_InitPinsM2() + BOARD_WIFI_BT_Enable(true)
// for evkbmimxrt1170 (mcuxsdk examples/_boards/evkbmimxrt1170/wifi_bt_config.c),
// which is the authoritative working reference for this board and card class:
//
//   SDIO_RST = GPIO_AD_16 (ball N17) as GPIO9_IO15   <- IOMUXC_GPIO_AD_16_GPIO9_IO15
//   WL_RST   = GPIO_AD_31 (ball J17) as GPIO9_IO30   <- IOMUXC_GPIO_AD_31_GPIO9_IO30
//   both initialised as outputs driven LOW, then:
//     SDIO_RST = 1; wait 100 ms; WL_RST = 1; wait 100 ms
//
// Two things this corrects from the first attempt: the pad is muxed to GPIO9
// (ALT10), not GPIO3 (ALT5) -- NXP drives the fast alias -- and BOTH lines are
// sequenced, 100 ms apart, rather than one.
//
// WL_RST is driven even though R404 is DNP on RevC3 (so GPIO_AD_31 does not
// reach J54 pin 56, which sits high on R829 regardless).  Matching the
// reference exactly is worth more than saving one register write, and the pad
// is otherwise idle -- though note it is also Arduino D12/MISO.
//
// Pad mux registers are local defines because the core header stops at
// GPIO_AD_14.  RM offsets: GPIO_AD_16 at 14Ch, GPIO_AD_31 at 188h, IOMUXC base
// 0x400E8000.  ALT10 = 0xA.
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30

static void m2ReleaseWifiReset() {
    // SION (bit 4) forces the input path on so GPIO9_PSR reflects the actual
    // pin.  NXP passes 0 here because it never reads these back; we do, and
    // without SION a PSR read of 0 says nothing about the pad's real level.
    M2_SDIO_RST_MUX = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO15
    M2_WL_RST_MUX   = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO30
    GPIO9_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    GPIO9_DR_SET = (1u << M2_SDIO_RST_BIT);         // SDIO_RST high
    delay(100);
    GPIO9_DR_SET = (1u << M2_WL_RST_BIT);           // then WL_RST high
    delay(100);
}

// Read the pads back through GPIO9_PSR.  The first attempt drove GPIO3 and had
// no way to tell whether the level actually reached the pin -- which is exactly
// the thing that needed proving.
static uint32_t m2ResetPadLevels() {
    return ((GPIO9_PSR >> M2_SDIO_RST_BIT) & 1u) |
           (((GPIO9_PSR >> M2_WL_RST_BIT) & 1u) << 1);
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
    // DR is what we asked for; PSR is what the pin is doing.  If DR reads 1 and
    // PSR reads 0, GPIO9 is not the instance that owns this pad and the drive
    // is going nowhere -- which is a completely different problem from a module
    // that is out of reset and simply not answering.
    uint32_t pads = m2ResetPadLevels();
    Serial1.print("rst_pads: sdio_rst dr=");
    Serial1.print((GPIO9_DR >> M2_SDIO_RST_BIT) & 1u);
    Serial1.print(" psr=");
    Serial1.print(pads & 1u);
    Serial1.print(" | wl_rst dr=");
    Serial1.print((GPIO9_DR >> M2_WL_RST_BIT) & 1u);
    Serial1.print(" psr=");
    Serial1.print((pads >> 1) & 1u);
    Serial1.print(" | gdir=0x");
    Serial1.println(GPIO9_GDIR, HEX);

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

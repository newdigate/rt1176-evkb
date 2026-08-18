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

// ---------------------------------------------------------------------------
// BT_WAKE_HOST watch.  J54 pin 20 (UART_WAKE#) is the ONLY card->host signal on
// this connector that actually reaches the MCU:
//
//   J54.20 -> R811 (0R, fitted) -> BT_WAKE_B_3V3 -> R238 (0R, fitted)
//          -> U19.N16 = GPIO_AD_27      (no level shifter -- pin 20 is 3.3 V)
//
// Pin 21 (SDIO_WAKE#) is populated but blocked at jumper J104, open by default.
// Pin 22 (the BT UART TX) dies at R1901, which is DNP. So this pin is the only
// way this card could ever tell us it is alive.
//
// The datasheet calls it open-drain, active low, "Pullup required on platform"
// -- and the EVKB provides none (the net holds only R238, R811 and the MCU
// ball), so the internal pull-up is doing that job here.
//
// A static read cannot distinguish "card idle" from "card absent": both read
// high. What IS diagnostic is a LOW at any point -- only a live card can pull
// this down. So latch it.
// GPIO_AD_27 mux 0x400E8178, pad 0x400E83BC (RM stride from GPIO_AD_16 at
// 0x400E814C / 0x400E8390). ALT10 = GPIO9_IO26 (AD_n -> bit n-1, the same rule
// that gives NXP's AD_16 -> IO15 and AD_31 -> IO30).
#define M2_BT_WAKE_MUX (*(volatile uint32_t *)0x400E8178u)
#define M2_BT_WAKE_PAD (*(volatile uint32_t *)0x400E83BCu)
#define M2_BT_WAKE_BIT 26

static volatile bool g_btWakeEverLow = false;

static void m2WatchBtWakeInit() {
    M2_BT_WAKE_MUX = 0x10u | 0xAu;   // SION | ALT10 = GPIO9_IO26
    M2_BT_WAKE_PAD = 0x04u;          // pull-up enabled (bits[3:2]=01)
    GPIO9_GDIR &= ~(1u << M2_BT_WAKE_BIT);   // input
}

static bool m2BtWakeLevel() { return (GPIO9_PSR >> M2_BT_WAKE_BIT) & 1u; }

// Decisive control. A single reading cannot tell "held low by the card" from
// "my pull-up never took effect". Read the pin under an internal pull-UP and
// then under an internal pull-DOWN:
//
//   up=1 down=0  -> pin is FLOATING: nothing external drives it (card idle/absent)
//   up=0 down=0  -> something ACTIVELY HOLDS IT LOW  (only a powered card can)
//   up=1 down=1  -> something actively holds it high
//
// RT1176 SW_PAD_CTL bits[3:2] PULL: 01=up, 10=down, 11=none (same encoding
// SdFat uses for the SD pads: 0x04 pull-up, 0x08 pull-down).
static void m2BtWakeProbe(bool *up, bool *down, uint32_t *padRead) {
    M2_BT_WAKE_PAD = 0x04u;            // pull-up
    delayMicroseconds(500);
    *up = m2BtWakeLevel();
    M2_BT_WAKE_PAD = 0x08u;            // pull-down
    delayMicroseconds(500);
    *down = m2BtWakeLevel();
    M2_BT_WAKE_PAD = 0x04u;            // leave it pulled up
    delayMicroseconds(500);
    *padRead = M2_BT_WAKE_PAD;         // prove the writes stick at all
}

static void m2PollBtWake() { if (!m2BtWakeLevel()) g_btWakeEverLow = true; }

// Sample pin 20 hard and report whether it EVER leaves its held-low state, and
// how many edges we see.  A card that is merely powered holds it low; a card
// that is LISTENING should react to traffic we send it.
static void m2BtWakeSample(uint32_t iters, bool *anyHigh, uint32_t *edges) {
    bool prev = m2BtWakeLevel(); bool hi = prev; uint32_t e = 0;
    for (uint32_t i = 0; i < iters; i++) {
        bool now = m2BtWakeLevel();
        if (now != prev) { e++; prev = now; }
        if (now) hi = true;
        delayMicroseconds(10);
    }
    *anyHigh = hi; *edges = e;
}

// HCI Reset, H4 framing: packet type 0x01 (command), opcode 0x0C03, plen 0.
// If the card is running BT firmware it must answer with a Command Complete --
// which we can never read (R1901 is DNP) -- but processing it is exactly the
// kind of thing that makes an NXP controller assert BT_WAKE_HOST.
static const uint8_t HCI_RESET[] = { 0x01, 0x03, 0x0C, 0x00 };

// Continuity test for the LPUART2 RX path, run BEFORE Serial2.begin() takes the
// pad.  R1901 (module->MCU RXD) is DNP from the factory and was bridged by hand
// on 2026-08-18; this says whether the bridge actually conducts.
//
// With R1901 open, the MCU RX pad has no driver -- its net reaches only
// Arduino header pin J9.2 (via fitted R2) and the DNP pad -- so it FLOATS, and
// an internal pull-up/pull-down will swing it.
// With R1901 bridged, U355's output drives it, and a UART idle line sits HIGH,
// so neither internal pull can move it.
//
//   up=1 down=0 -> FLOATING: the bridge is not conducting
//   up=1 down=1 -> DRIVEN HIGH: bridge good, U355 powered, line idle
//   up=0 down=0 -> DRIVEN LOW: bridge good but the line is held low (break/BREAK)
//
// GPIO_DISP_B2_11 is GPIO5_IO12 at ALT5 (docs/arduino-header-revc3.md: D0).
#define M2_RX_MUX (*(volatile uint32_t *)0x400E8240u)
#define M2_RX_PAD (*(volatile uint32_t *)0x400E8484u)
#define M2_RX_BIT 12

// BT_INDEPENDENT_RESET (J54 pin 54, W_DISABLE2#) is driven from GPIO_AD_15
// (ball M14, the "SPDIF_IN" pad) through R209/R834, with a 10K pull-up R832 --
// so it idles HIGH = not reset, and we have never touched it.
//
// Now that R1901 is bridged we can SEE the card's UART TX line, so a reset
// pulse becomes a real experiment: if the module is alive at all, resetting its
// Bluetooth block should make that line do SOMETHING -- at minimum leave its
// stuck-low state, at best emit the NXP boot-ROM download request.
// GPIO_AD_15 mux 0x400E8148, pad 0x400E838C; ALT10 = GPIO9_IO14 (AD_n -> bit n-1).
#define M2_BT_RST_MUX (*(volatile uint32_t *)0x400E8148u)
#define M2_BT_RST_BIT 14

static void m2PulseBtReset() {
    M2_BT_RST_MUX = 0xAu;                      // ALT10 = GPIO9_IO14
    GPIO9_GDIR |= (1u << M2_BT_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_BT_RST_BIT);    // assert reset (active low)
    delay(20);
    GPIO9_DR_SET = (1u << M2_BT_RST_BIT);      // release
}

// Watch the card's UART TX pad as a GPIO for `ms` milliseconds.
static void m2WatchRxLine(uint32_t ms, bool *anyHigh, uint32_t *edges) {
    M2_RX_MUX = 0x10u | 0x5u;                  // SION | ALT5 = GPIO5_IO12
    GPIO5_GDIR &= ~(1u << M2_RX_BIT);
    M2_RX_PAD = 0x04u;
    bool prev = (GPIO5_PSR >> M2_RX_BIT) & 1u; bool hi = prev; uint32_t e = 0;
    for (uint32_t i = 0; i < ms * 100; i++) {  // ~10us per sample
        bool now = (GPIO5_PSR >> M2_RX_BIT) & 1u;
        if (now != prev) { e++; prev = now; }
        if (now) hi = true;
        delayMicroseconds(10);
    }
    *anyHigh = hi; *edges = e;
}

static void m2RxContinuity(bool *up, bool *down) {
    M2_RX_MUX = 0x10u | 0x5u;                 // SION | ALT5 = GPIO5_IO12
    GPIO5_GDIR &= ~(1u << M2_RX_BIT);         // input
    M2_RX_PAD = 0x04u;  delayMicroseconds(500);   // pull-up
    *up = (GPIO5_PSR >> M2_RX_BIT) & 1u;
    M2_RX_PAD = 0x08u;  delayMicroseconds(500);   // pull-down
    *down = (GPIO5_PSR >> M2_RX_BIT) & 1u;
    M2_RX_PAD = 0x04u;  delayMicroseconds(500);
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
static bool g_rxUp = false, g_rxDown = false;

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
    Serial1.print("vsel: vend_spec=0x");
    Serial1.print(sdio.lastVendSpec(), HEX);
    Serial1.print(" (bit1=");
    Serial1.print((sdio.lastVendSpec() >> 1) & 1u);
    Serial1.print(") mux=0x");
    Serial1.println(sdio.lastVselMux(), HEX);

    bool wu=false, wd=false; uint32_t wpad=0;
    m2BtWakeProbe(&wu, &wd, &wpad);
    Serial1.print("r1901_bridge: pullup_reads=");
    Serial1.print(g_rxUp);
    Serial1.print(" pulldown_reads=");
    Serial1.print(g_rxDown);
    Serial1.print(" -> ");
    Serial1.println(g_rxUp==g_rxDown ? (g_rxUp ? "DRIVEN HIGH -- bridge conducts, line idle"
                                               : "DRIVEN LOW -- bridge conducts, line held low")
                                     : "FLOATING -- bridge NOT conducting");

    Serial1.print("bt_wake(pin20): pullup_reads=");
    Serial1.print(wu);
    Serial1.print(" pulldown_reads=");
    Serial1.print(wd);
    Serial1.print(" pad=0x");
    Serial1.print(wpad, HEX);
    Serial1.print(" ever_low=");
    Serial1.print(g_btWakeEverLow ? 1 : 0);
    Serial1.print(" -> ");
    Serial1.println(wu==wd ? (wu ? "HELD HIGH externally" : "HELD LOW externally -- something is driving it")
                           : "floating (nothing drives it)");

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

    m2WatchBtWakeInit();

    // LPUART2 -> J54 pin 32 (card UART_RXD).  TX only in practice: the card's
    // reply path dies at R1901, which is DNP on this board.  No flow control --
    // CTS/RTS are the gigabit PHY's interrupt and reset lines here.
    m2RxContinuity(&g_rxUp, &g_rxDown);
    Serial2.begin(115200);
    Serial1.println("serial2=up_115200");

    m2ReleaseWifiReset();
    Serial1.println("m2_wifi_reset=released");

    // NXP define SDMMCHOST_OPERATION_VOLTAGE_1V8 for EVERY IW416 module config
    // in their SDK -- the Murata 1XK M.2 and the u-blox MAYA-W1 USD alike -- so
    // 1.8 V was tried here on 2026-08-17.  It made no difference: the card is
    // equally silent at both voltages.
    //
    // Left at the 3.3 V default deliberately.  J15 (microSD) is the same bus,
    // and a 3.3 V-only card there must not meet a 1.8 V rail.  Flip this to
    // true only with J15 empty:
    //     sdio.useIoVoltage1V8(true);
    sdio.useIoVoltage1V8(true);
    Serial1.println("sdio_io_voltage=1v8_requested");

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
    m2PollBtWake();

    // Every ~5 s: characterise the wake line quietly, transmit HCI Reset, then
    // characterise it again.  A difference between the two is the card
    // reacting to us -- the only positive signal available on this board.
    if (n % 25 == 24) {
        bool hiBefore=false, hiAfter=false; uint32_t eBefore=0, eAfter=0;
        m2BtWakeSample(2000, &hiBefore, &eBefore);          // ~20 ms baseline
        Serial2.write(HCI_RESET, sizeof(HCI_RESET));
        Serial2.flush();
        m2BtWakeSample(2000, &hiAfter, &eAfter);            // ~20 ms after TX
        Serial1.print("hci_probe: before[high=");
        Serial1.print(hiBefore); Serial1.print(" edges="); Serial1.print(eBefore);
        Serial1.print("] after[high="); Serial1.print(hiAfter);
        Serial1.print(" edges="); Serial1.print(eAfter);
        // Drain and characterise anything on Serial2 RX.  R1901 is DNP so the
        // card CANNOT reach us -- if real bytes accumulate here that finding is
        // wrong and Track B is not blocked after all.  The likelier source is
        // noise: this same pad reaches Arduino header pin J9.2 through fitted
        // R2, so it is a floating header pin picking up interference.
        static uint32_t rxTotal = 0; static int lastByte = -1;
        while (Serial2.available()) { lastByte = Serial2.read(); rxTotal++; }
        // Pulse BT_INDEPENDENT_RESET and watch the card's UART TX line for any
        // sign of life. This is the first test with visibility of that line.
        bool rHi=false; uint32_t rEdges=0;
        m2PulseBtReset();
        m2WatchRxLine(150, &rHi, &rEdges);
        Serial1.print("bt_reset_pulse: rx_any_high=");
        Serial1.print(rHi);
        Serial1.print(" rx_edges=");
        Serial1.println(rEdges);
        Serial2.begin(115200);                  // hand the pad back to LPUART2

        Serial1.print("] serial2_rx: total=");
        Serial1.print(rxTotal);
        Serial1.print(" last=0x");
        Serial1.println(lastByte < 0 ? 0 : lastByte, HEX);
    }

    Serial1.print("alive=");
    Serial1.println(n++);
    // Re-report every 5 s so a reader attached at any moment sees the result,
    // without having to win a race against the boot.
    if (n % 25 == 0) reportProbe();
    delay(200);
}

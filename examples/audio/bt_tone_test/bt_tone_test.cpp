// bt_tone_test -- BT-3 phase 4 of the M.2 Bluetooth programme: a 1 kHz tone
// streamed to a real A2DP sink through AudioOutputBluetooth.
//
// Preamble (board power-up, BT UART firmware download, HCI Reset, identity)
// is copied from examples/networking/m2_hci_probe/m2_hci_probe.cpp -- keep the
// two files in step for that shared portion.  Where the probe then runs
// probeInquiry()/probeConnect(), this example calls A2dpSource::connect()
// instead and, on success, starts AudioOutputBluetooth streaming the tone.
//
// rt1176-only: the M.2 socket (and its BT UART on LPUART2/J54) exists only on
// the MIMXRT1170-EVKB, exactly like every other m2_* example.
//
// Spec: docs/superpowers/specs/2026-08-23-m2-bluetooth-a2dp-programme-design.md
#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>
#include <Audio.h>

#include <Hci.h>
#include <HciEvents.h>
#include <HciTransport.h>
#include <HciPump.h>
#include <BtFwLoader.h>
#include <A2dpSource.h>

#include "AudioOutputBluetooth.h"

#define CONSOLE Serial1            // LPUART1 -> the MCU-Link VCOM

// --- the Bluetooth transport: same objects as m2_hci_probe -----------------
static HciTransport hciIo(Serial2);
static Hci hci(hciIo);
static HciPump pump;
static BtFwLoader btLoader(hciIo);
static BtFwLoader::Error s_btFwSt = BtFwLoader::NO_IMAGE;
static Hci::Error s_hciSt = Hci::TIMEOUT;     // outcome of the Reset step

#if defined(HAVE_IW416_BT_FW)
extern const uint8_t  iw416_bt_fw[];
extern const uint32_t iw416_bt_fw_len;
#endif

// --- board preamble -- copied from m2_hci_probe.cpp (and m2_uap_probe / WiFi.cpp);
// keep in step.  Release SDIO_RST (GPIO_AD_16 = GPIO9.15) then WL_RST/PDn
// (GPIO_AD_31 = GPIO9.30, reaching PDn via the hand-bridged R404), with the 1 s
// ROM-boot wait PDn requires.  Without it the card stays in power-down.  This
// step powers up BOTH radios on the module; the BT core is then brought up
// independently over the UART (btFirmwareDownload() below) -- no SDIO/Wi-Fi
// work is needed for Bluetooth, so this example does not link SdioHost/Iw416.
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30
#define M2_RST_GDIR     GPIO9_GDIR
#define M2_RST_SET      GPIO9_DR_SET
#define M2_RST_CLEAR    GPIO9_DR_CLEAR
#define M2_RST_PSR      GPIO9_PSR
#define M2_RST_ALT      0xAu

static void m2ReleaseWifiReset() {
    M2_SDIO_RST_MUX = 0x10u | M2_RST_ALT;           // SION | GPIO alternate
    M2_WL_RST_MUX   = 0x10u | M2_RST_ALT;
    M2_RST_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    M2_RST_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    M2_RST_SET = (1u << M2_SDIO_RST_BIT);           // SDIO_RST high
    delay(100);
    M2_RST_SET = (1u << M2_WL_RST_BIT);             // then WL_RST / PDn high
    delay(1000);                                    // PDn exit needs ROM boot time
}

// --- HCI opcodes (Core 5.2 Vol 4 Part E 7.x) --------------------------------
static const uint16_t OP_RESET            = 0x0C03;
static const uint16_t OP_READ_LOCAL_VER   = 0x1001;
static const uint16_t OP_READ_BUFFER_SIZE = 0x1005;
static const uint16_t OP_READ_BD_ADDR     = 0x1009;

static void printHex8(uint8_t v)   { if (v < 0x10) CONSOLE.print('0'); CONSOLE.print(v, HEX); }
static void printHex16(uint16_t v) { printHex8((uint8_t)(v >> 8)); printHex8((uint8_t)v); }
static void printBd(const uint8_t *bd) { char s[18]; hciFormatBd(bd, s); CONSOLE.print(s); }

// Counters are cumulative across begin() -- see m2_hci_probe.cpp for why.
static uint32_t s_toBase = 0, s_frBase = 0, s_stBase = 0, s_qfBase = 0, s_lateBase = 0;
static void hciCountersFold() {
    s_toBase   += hci.timeouts();
    s_frBase   += hci.framing();
    s_stBase   += hci.starved();
    s_qfBase   += hci.queueFull();
    s_lateBase += hci.late();
}
static void printCounters() {
    CONSOLE.print(" timeouts="); CONSOLE.print(s_toBase   + hci.timeouts());
    CONSOLE.print(" framing=");  CONSOLE.print(s_frBase   + hci.framing());
    CONSOLE.print(" starved=");  CONSOLE.print(s_stBase   + hci.starved());
    CONSOLE.print(" qfull=");    CONSOLE.print(s_qfBase   + hci.queueFull());
    CONSOLE.print(" late=");     CONSOLE.print(s_lateBase + hci.late());
}
static void printFail(const char *what, Hci::Error e, const Hci::Reply &r, const char *alt) {
    CONSOLE.print(what); CONSOLE.print("=fail reason=");
    CONSOLE.print(e == Hci::OK ? alt : Hci::errorName(e));
    CONSOLE.print(" status=0x"); printHex8(r.status);
    printCounters(); CONSOLE.println();
}
static void idleMs() { delay(1); }   // yield() inside delay() services the HciPump-attached EventResponder

// L2cap::begin()'s aclCredits argument -- the Total_Num_ACL_Data_Packets field
// from Read_Buffer_Size, captured below in probeIdentity().
static uint8_t s_aclNum = 0;

// --- identity ---------------------------------------------------------------
static void probeIdentity() {
    Hci::Reply r;
    Hci::Error e = hci.run(OP_READ_LOCAL_VER, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 8) {
        CONSOLE.print("hci_version: hci_ver="); CONSOLE.print(r.params[0]);
        CONSOLE.print(" hci_rev=0x");     printHex16((uint16_t)(r.params[1] | (r.params[2] << 8)));
        CONSOLE.print(" lmp_ver=");       CONSOLE.print(r.params[3]);
        CONSOLE.print(" manufacturer=0x"); printHex16((uint16_t)(r.params[4] | (r.params[5] << 8)));
        CONSOLE.print(" lmp_subver=0x");  printHex16((uint16_t)(r.params[6] | (r.params[7] << 8)));
        CONSOLE.println();
    } else printFail("hci_version", e, r, "short_reply");

    e = hci.run(OP_READ_BD_ADDR, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 6) { CONSOLE.print("bd_addr="); printBd(r.params); CONSOLE.println(); }
    else printFail("bd_addr", e, r, "short_reply");

    e = hci.run(OP_READ_BUFFER_SIZE, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 7) {
        uint16_t aclLen = (uint16_t)(r.params[0] | (r.params[1] << 8));
        uint16_t aclNum = (uint16_t)(r.params[3] | (r.params[4] << 8));
        CONSOLE.print("hci_buffer: acl_len="); CONSOLE.print(aclLen);
        CONSOLE.print(" acl_num="); CONSOLE.print(aclNum);
        CONSOLE.print(" sco_len="); CONSOLE.print(r.params[2]);
        CONSOLE.print(" sco_num="); CONSOLE.println(r.params[5] | (r.params[6] << 8));
        hci.setAclMax(aclLen);
        s_aclNum = (uint8_t)(aclNum > 255 ? 255 : aclNum);
    } else printFail("hci_buffer", e, r, "short_reply");
}

#if defined(M2_BT_FAST_BAUD)
static const uint16_t OP_VS_SET_BAUD = 0xFC09;
// Phase 0: vendor set-baud (uint32 LE), then re-baud the port and re-validate
// with a fresh Reset + identity.  Copied from m2_hci_probe.cpp's probeFastBaud().
static void probeFastBaud() {
    uint32_t rate = M2_BT_FAST_BAUD;
    uint8_t cmd[8] = { 0x01, (uint8_t)(OP_VS_SET_BAUD & 0xFF), (uint8_t)(OP_VS_SET_BAUD >> 8), 4,
                       (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24) };
    hciIo.write(cmd, sizeof cmd);
    hciIo.rebaud(rate);                                   // end() drains the 8 bytes at 115200, then rewrites BAUD
    hciCountersFold(); hci.begin();                        // fresh HCI state at the new rate
    delay(20);                                             // let the controller finish switching its own UART
    Hci::Reply r;
    Hci::Error e = hci.run(OP_RESET, nullptr, 0, &r, 1000, idleMs);
    if (e != Hci::OK) {
        CONSOLE.print("bt_baud_switch=fail rate="); CONSOLE.print(rate);
        CONSOLE.print(" reason="); CONSOLE.println(Hci::errorName(e));
        hciIo.rebaud(115200);
        hciCountersFold(); hci.begin();
        CONSOLE.println("bt_baud_switch=reverted rate=115200");
        return;
    }
    CONSOLE.print("bt_baud_switch=ok rate="); CONSOLE.println(rate);
    probeIdentity();
}
#endif

// ---------------------------------------------------------------------------
// Assert the card's CTS input, so it is permanently CLEAR TO SEND.  Copied
// from m2_hci_probe.cpp -- see that file for the full rationale (required for
// HCI to answer on the Murata 1XK / IW416; holds the 1G ENET PHY in reset).
// GPIO_DISP_B2_13: mux 0x400E8248, pad 0x400E848C, ALT5 = GPIO5_IO14.
#define M2_BT_CTS_MUX (*(volatile uint32_t *)0x400E8248u)
#define M2_BT_CTS_PAD (*(volatile uint32_t *)0x400E848Cu)
#define M2_BT_CTS_BIT 14
#define M2_BT_CTS_GDIR   GPIO5_GDIR
#define M2_BT_CTS_CLEAR  GPIO5_DR_CLEAR
#define M2_BT_CTS_GPIO_ALT 0x5u

// Wake the controller from BOOT SLEEP -- NXP's uart_fw_download() step, run
// before the image goes across.  Copied from m2_hci_probe.cpp.  A 10 ms LOW
// pulse on GPIO_DISP_B2_13, mux returned to LPUART2_RTS_B afterwards.
static void m2WakeFromBootSleep() {
    M2_BT_CTS_MUX = M2_BT_CTS_GPIO_ALT;         // the GPIO alternate (no SION)
    M2_BT_CTS_PAD = 0x02u;                      // NXP's pad config for this pin
    M2_BT_CTS_GDIR |= (1u << M2_BT_CTS_BIT);    // output
    M2_BT_CTS_CLEAR = (1u << M2_BT_CTS_BIT);    // drive LOW
    delay(10);                                  // NXP hold 10 ms
    M2_BT_CTS_MUX = 0x3u;                       // revert: ALT3 = LPUART2_RTS_B
    M2_BT_CTS_PAD = 0x02u;
}

static void m2AssertBtCts() {
    M2_BT_CTS_MUX = 0x10u | M2_BT_CTS_GPIO_ALT; // SION | the GPIO alternate
    M2_BT_CTS_PAD = 0x0Cu;                      // no pull; we drive it
    M2_BT_CTS_GDIR |= (1u << M2_BT_CTS_BIT);    // output
    M2_BT_CTS_CLEAR = (1u << M2_BT_CTS_BIT);    // LOW = asserted = clear to send
}

// Dump whatever Serial2 receives in the next `ms`, as HEX.  Copied from
// m2_hci_probe.cpp -- used only on the combo-over-SDIO (M2_BT_NO_UART_DNLD) path.
static void m2DumpSerial2(const char *label, uint32_t ms) {
    uint8_t buf[64]; uint32_t n = 0; const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        while (Serial2.available()) { int c = Serial2.read(); if (n < sizeof buf) buf[n] = (uint8_t)c; n++; }
        delay(1);
    }
    const uint32_t kept = n < sizeof buf ? n : (uint32_t)sizeof buf;
    CONSOLE.print(label); CONSOLE.print(" n="); CONSOLE.print(n); CONSOLE.print(" hex=");
    if (!kept) CONSOLE.print("none");
    else for (uint32_t i = 0; i < kept; i++) printHex8(buf[i]);
    CONSOLE.println();
}

// The BT-only UART firmware download.  Called immediately after the card is
// powered up and BEFORE any HCI work.  Copied from m2_hci_probe.cpp's
// btFirmwareDownload(), trimmed of the SDIO-combo diagnostic hex dumps that
// example runs (bt_post_dnld/bt_raw_reset probes) -- not needed here since
// this example never touches SDIO.
static void btFirmwareDownload() {
#if defined(M2_BT_NO_UART_DNLD)
    CONSOLE.println("bt_fw_source=combo_over_sdio");
    CONSOLE.println("bt_fw_download=skipped (combo-over-SDIO path)");
    m2DumpSerial2("bt_uart_preboot:", 300);
    s_btFwSt = BtFwLoader::NO_IMAGE;
#else
#if defined(HAVE_IW416_BT_FW)
    btLoader.setImage(iw416_bt_fw, iw416_bt_fw_len);
    s_btFwSt = btLoader.run(3000, 500, 30000, idleMs);
#if defined(BT_FW_IS_SYNTHETIC)
    CONSOLE.println("bt_fw_source=synthetic");
#else
    CONSOLE.println("bt_fw_source=nxp");
#endif
    CONSOLE.print("bt_fw_download=");
    CONSOLE.print(BtFwLoader::errorName(s_btFwSt));
    CONSOLE.print(" chip_id=0x");   printHex16(btLoader.chipId());
    CONSOLE.print(" start_inds=");  CONSOLE.print(btLoader.startInds());
    CONSOLE.print(" sent=");        CONSOLE.print(btLoader.bytesSent());
    CONSOLE.print("/");             CONSOLE.println(iw416_bt_fw_len);
#else
    CONSOLE.println("bt_fw_source=none");
    CONSOLE.println("bt_fw_download=skipped (no image compiled in)");
#endif
#endif
}

// --- application -------------------------------------------------------------
static A2dpSource src(hci, hciIo);
static AudioSynthWaveformSine toneGen;                // "tone" collides with core_pins.h's tone(pin,freq,ms)
static AudioOutputBluetooth   btout;
static AudioConnection pc0(toneGen, 0, btout, 0);
static AudioConnection pc1(toneGen, 0, btout, 1);      // same tone to L and R

static uint32_t nowMs() { return millis(); }
static void btLog(void *, const char *s) { CONSOLE.println(s); }
static void onEvt(void *, uint8_t c, const uint8_t *p, uint8_t l) { src.onEvent(c, p, l); }
static void onAclThunk(void *, uint16_t h, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l); }

void setup() {
    CONSOLE.begin(115200);
    delay(50);
    CONSOLE.println("RT1176 BT tone test up");

    hciIo.begin(115200);
    CONSOLE.println("serial2=up_115200");

    m2ReleaseWifiReset();
    CONSOLE.println("m2_wifi_reset=released");

#if defined(M2_BT_WAKE_PULSE)
    m2WakeFromBootSleep();
    CONSOLE.println("bt_wake=pulsed_10ms_low (GPIO_DISP_B2_13, mux returned to LPUART2_RTS_B)");
#else
    CONSOLE.println("bt_wake=off");
#endif

#if defined(M2_BT_RTS_FLOW)
    // Hardware RXRTSE: the receiver deasserts LPUART2_RTS_B as the RX FIFO nears
    // full, pausing the card before overrun (the fix for the phase-4 media stall).
    // Idles asserted (clear to send) like the old static assert; the core re-applies
    // it across the 3 Mbaud rebaud.  ★ Holds/toggles the ENET PHY reset (R1866).
    bool rts = Serial2.attachRts((uint8_t)M2_BT_RTS_WATER);
    CONSOLE.print("bt_flow=rxrtse rtswater="); CONSOLE.print((int)M2_BT_RTS_WATER);
    CONSOLE.print(" attached="); CONSOLE.println(rts ? 1 : 0);
#elif defined(M2_BT_ASSERT_CTS)
    m2AssertBtCts();
    CONSOLE.println("bt_cts=asserted_after_reset (PHY held in reset -- see m2_hci_probe.cpp)");
#else
    CONSOLE.println("bt_cts=undriven");
#endif

    btFirmwareDownload();

    hci.begin();
    pump.attach(hci);

    Hci::Reply r;
    uint8_t attempts = 0;
    for (attempts = 1; attempts <= 10; attempts++) {
        s_hciSt = hci.run(OP_RESET, nullptr, 0, &r, 500, idleMs);
        if (s_hciSt == Hci::OK) break;
    }
    if (s_hciSt == Hci::OK) {
        CONSOLE.print("hci_reset=ok attempts="); CONSOLE.print(attempts);
        printCounters(); CONSOLE.println();
        probeIdentity();
#if defined(M2_BT_FAST_BAUD)
        probeFastBaud();
#endif
    } else if (s_hciSt == Hci::TIMEOUT) {
        CONSOLE.print("hci_reset=timeout reason=no_response attempts=10");
        printCounters(); CONSOLE.println();
    } else {
        CONSOLE.print("hci_reset=fail reason="); CONSOLE.print(Hci::errorName(s_hciSt));
        CONSOLE.print(" attempts=10"); printCounters(); CONSOLE.println();
    }

    AudioMemory(24);
    toneGen.frequency(1000); toneGen.amplitude(0.5f);
    hci.onEvent(onEvt, nullptr);
    hci.onAcl(onAclThunk, nullptr);                    // A2dpSource does NOT seize this
    src.setLog(btLog, nullptr); src.setPin("1234");
#if defined(M2_BT_LEGACY_PIN)
    src.setLegacyPin(true);
#endif
#if defined(M2_BT_TARGET_NAME)
    A2dpSource::Result r2 = src.connect(M2_BT_TARGET_NAME, s_aclNum, nowMs, idleMs);
#else
    A2dpSource::Result r2 = src.connect(nullptr, s_aclNum, nowMs, idleMs);
#endif
    CONSOLE.print("a2dp="); CONSOLE.println(A2dpSource::resultName(r2));
    if (r2 == A2dpSource::OK) {
        btout.begin(src);
        CONSOLE.print("streaming frames_per_pkt="); CONSOLE.print(btout.framesPerPacket());
        CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
    }
}

// Every pass, no delay: btout.poll() has to run far more often than once a
// second to keep up with the audio ISR (a 64-frame/~190ms ring against a
// 1000ms service interval drops the vast majority of frames -- measured
// directly during BT-3 phase 4 task 6's diagnosis). The heartbeat is now
// throttled by millis() instead of being the thing that paces the loop.
void loop() {
    // ★ Drive the HciPump every pass: it is attached to yield()'s EventResponder,
    // and it is what parses incoming HCI -- including Number_Of_Completed_Packets,
    // which RETURNS the ACL credits L2cap needs to keep sending. Without this the
    // continuous-poll loop (no delay(), so no implicit yield()) starves the pump
    // and media send stalls after the first credit pool (silicon: packets froze
    // at 43 while blocks/drops climbed). yield() is non-blocking.
    yield();
    src.l2().service(); src.avdtp().service(); btout.poll();
    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        static uint32_t n = 0;
        CONSOLE.print("hb streaming="); CONSOLE.print(src.started() ? 1 : 0);
        CONSOLE.print(" blocks="); CONSOLE.print(btout.blocks());
        CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
        CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
        CONSOLE.print(" hw="); CONSOLE.print(btout.queueHighWater());
        CONSOLE.print(" n="); CONSOLE.println(n++);
    }
}

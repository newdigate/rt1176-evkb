// bt_sink_test -- NEW-41: the EVKB as an A2DP SINK.  A phone (or any A2DP source) pages this board, pairs, drives
// AVDTP at us and streams SBC; AudioInputBluetooth decodes the RTP/SBC media into a PCM ring, the SAI DMA ISR walks
// the audio graph at the codec's rate, and the audio PLL is trimmed by a servo on the ring's fill so the local clock
// follows the source's.  AVRCP absolute volume drives the WM8962's headphone output.  The mirror of bt_tone_test.
//
// Preamble (board power-up, BT UART firmware download, HCI Reset, identity) is copied VERBATIM from
// examples/audio/bt_tone_test/bt_tone_test.cpp (itself copied from examples/networking/m2_hci_probe) -- keep the
// three files in step for that shared portion.  Where bt_tone_test then runs BtSession/A2dpSource outbound, this
// example runs BtSinkSession/A2dpSink: no inquiry, no paging, no retry timer -- it listens.
//
// rt1176-only: the M.2 socket (and its BT UART on LPUART2/J54) exists only on
// the MIMXRT1170-EVKB, exactly like every other m2_* example.
//
// Spec: docs/superpowers/specs/2026-09-08-bt-a2dp-sink-design.md
#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>
#include <Audio.h>
// Audio.h pulls in every codec driver EXCEPT this one (control_wm8960.h is in its include list and
// control_wm8962.h is not), so the WM8962 header must be named explicitly -- same as acid_box.
#include "control_wm8962.h"
#include <Wire.h>                  // Wire2 = LPI2C5, the codec's bus (AudioControlWM8962::enable() begins it)

#include <Hci.h>
#include <HciEvents.h>
#include <HciTransport.h>
#include <HciPump.h>
#include <BtFwLoader.h>
#include <A2dpSink.h>
#include <BtSinkSession.h>
#include <Avrcp.h>
#include <BondTable.h>
#include <BondStoreEeprom.h>

#include "AudioInputBluetooth.h"

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


// --- the sink ---------------------------------------------------------------
// A2dpSink is ONE inbound attempt; BtSinkSession is the policy around it (discoverable + connectable while idle,
// take over each accepted incoming link, scanning off while a link is up).  The audio graph is
// AudioInputBluetooth -> AudioOutputI2S: the SAI DMA ISR walks it, so the node needs no clock of its own (the
// mirror of AudioOutputBluetooth, which has to self-clock because nothing else drives a source graph).
static A2dpSink      sink(hci, hciIo);
static BtSinkSession session(sink);
static BondTable bonds;                                // bonded devices, persisted in the EEPROM emulation (BondStoreEeprom, offset 4000)
static AudioInputBluetooth btin;
static AudioOutputI2S      i2sOut;
static AudioConnection     c1(btin, 0, i2sOut, 0), c2(btin, 1, i2sOut, 1);
static AudioControlWM8962  codec;

static void btLog(void *, const char *s) { CONSOLE.println(s); }
// A2dpSink's media callback: main context (the L2cap RX path), NOT an ISR -- the decode happens here, out of the
// SAI ISR, per the 2026-09-04 livelock lesson.
static void onMediaCb(void *, const uint8_t *p, uint16_t n) { btin.onMedia(p, n); }
// AVRCP absolute volume (0..127) straight onto the codec's headphone output.
static void onVolumeCb(void *, uint8_t v) { codec.headphoneVolume(v); CONSOLE.print("volume="); CONSOLE.println(v); }

static void onStreamCb(void *, bool streaming, uint8_t reason) {
    if (streaming) {
        btin.begin();
        CONSOLE.print("streaming by=incoming bitpool="); CONSOLE.print(sink.sbcParams().bitpool);
        CONSOLE.print(" mode="); CONSOLE.println((int)sink.sbcParams().mode);
    } else {
        btin.end();
        CONSOLE.print("stream_lost reason=0x"); printHex8(reason); CONSOLE.println();
    }
}
static void onAttemptCb(void *, A2dpSink::Result r, const char *pairedBy) {
    BondStoreEeprom::save(bonds);                      // save after EVERY attempt: an erased stale bond must persist too
    CONSOLE.print("a2dp_sink="); CONSOLE.print(A2dpSink::resultName(r));
    CONSOLE.print(" bonds="); CONSOLE.print(bonds.count());
    CONSOLE.print(" paired_by="); CONSOLE.println(pairedBy);
}

#if defined(M2_BT_ACL_TRACE)
static void aclTrace(void *, bool out, uint16_t handle, const uint8_t *pdu, uint16_t len) {
    // ★ SKIP RTP media packets (payload starts with RTP V2/PT96 = 0x80 0x60), same rule as bt_tone_test: one
    // media packet is ~439 chars of hex, ~38 ms at 115200, and CONSOLE.print BLOCKS when its TX buffer fills --
    // tracing the media firehose throttles the very stream being diagnosed.  Signalling is low-rate.
    if (len >= 6 && pdu[4] == 0x80 && pdu[5] == 0x60) return;
    CONSOLE.print("acl_trace dir="); CONSOLE.print(out ? "out" : "in");
    CONSOLE.print(" h=0x"); printHex16(handle);
    CONSOLE.print(" t="); CONSOLE.print(micros());
    CONSOLE.print(" hex=");
    for (uint16_t i = 0; i < len; i++) { printHex8(pdu[i]); if (i + 1 < len) CONSOLE.print(' '); }
    CONSOLE.println();
}
#endif

static void onEvt(void *, uint8_t c, const uint8_t *p, uint8_t l) { sink.onEvent(c, p, l); }
// Hci::AclFn puts pb BEFORE the data; A2dpSink/L2cap take it LAST.  Forwarding it is load-bearing: without it
// L2cap cannot reassemble a fragmented PDU and the ACL-reassembly fix of 2026-09-07 is silently disabled.
static void onAclThunk(void *, uint16_t h, uint8_t pb, const uint8_t *d, uint16_t l) { sink.onAcl(h, d, l, pb); }

void setup() {
    CONSOLE.begin(115200);
    // Bench 2026-09-09: with the core's small TX ring the once-a-second heartbeat (~6 lines, ~30 ms at 115200) BLOCKED
    // loop() inside CONSOLE.print, so onMedia() stalled longer than the ring's ~10 blocks of cover and the sink took ONE
    // underrun (+ a paired overrun from the queued burst) every ~10 s -- the same observer effect the Bose session hit in
    // the source's self-clock.  A 4 KB TX extension lets a whole heartbeat leave without waiting for the wire.
    static uint8_t s_consoleTx[4096]; CONSOLE.addMemoryForWrite(s_consoleTx, sizeof s_consoleTx);
    delay(50);
    CONSOLE.println("RT1176 BT sink test up");

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
    codec.enable();
    codec.headphoneVolume(100);

    hci.onEvent(onEvt, nullptr);
    hci.onAcl(onAclThunk, nullptr);
    sink.setLog(btLog, nullptr);
    // Class of device 0x240414: service class Audio + Rendering, major class Audio/Video, minor Loudspeaker --
    // what makes a phone list this board as a speaker rather than an unknown device.  `name` is BORROWED by
    // BtLink (not copied), so it must be a literal/static and must be set BEFORE session.begin() runs PREPARE.
    sink.setIdentity(0x240414, M2_BT_SINK_NAME);
    sink.onMedia(onMediaCb, nullptr);
    Avrcp::setVolumeCallback(onVolumeCb, nullptr);     // file-scope hook: Avrcp::respond() is static by design

#if defined(M2_BT_FORGET_BONDS)
    // load() first so `before` reflects what was really persisted before the wipe (bt_tone_test's note).
    BondStoreEeprom::load(bonds);
    { uint8_t before = bonds.count(); (void)BondStoreEeprom::wipe(bonds);
      CONSOLE.print("bonds_forgotten="); CONSOLE.println(before); }
#else
    BondStoreEeprom::load(bonds);
#endif
    sink.setBonds(&bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
#if defined(M2_BT_ACL_TRACE)
    sink.l2().onAclTrace(aclTrace, nullptr);
#endif
#if defined(M2_BT_SUPERVISION_MS)
    sink.link().setSupervisionSlots((uint16_t)((uint32_t)M2_BT_SUPERVISION_MS * 1000u / 625u));  // ms -> 0.625 ms slots
#endif
    // What we tell the source its media is running ahead of playback by (AVDTP 1.3 s8.19, 0.1 ms units): the
    // ring target IS that latency, so derive it rather than carrying a second number that can drift from it.
    // TARGET blocks x 128 samples at 44100 Hz = 8 * 128 * 10000 / 44100 = 232 (23.2 ms).  A2dpSink's own default
    // (460) was a standing guess; this is the figure the servo actually holds the ring at.
    sink.setDelayTenthMs((uint16_t)((uint32_t)AudioInputBluetooth::TARGET * AUDIO_BLOCK_SAMPLES * 10000u / 44100u));
    session.onStream(onStreamCb, nullptr);
    session.onAttempt(onAttemptCb, nullptr);
#if defined(M2_BT_SINK_ALWAYS_DISCOVERABLE)
    session.setAlwaysDiscoverable(true);
#endif
    // Begin the session only if HCI came up; with no card it never listens and the heartbeat stays vacuous.
    if (s_hciSt == Hci::OK) {
        session.begin(&bonds, s_aclNum, millis());
        CONSOLE.print("sink=listening name=\""); CONSOLE.print(M2_BT_SINK_NAME); CONSOLE.println("\" cod=0x240414");
    } else {
        CONSOLE.println("a2dp_sink=deferred (no HCI: card absent)");
    }
}

// Every pass, no delay.  yield() drives the HciPump (attached to its EventResponder), which is what parses
// incoming HCI -- including the ACL data that IS the audio here.  The heartbeat is throttled by millis()
// rather than being the thing that paces the loop.  Nothing in this loop drains audio: the SAI DMA ISR walks
// the graph and pops the ring; onMedia() (main context, from sink.service()'s L2cap RX) fills it.
void loop() {
    yield();
    session.tick(millis());
    sink.service();
    // AVDTP SUSPEND: the source stopped sending on purpose and the stream stays configured, so the node's
    // m_live is still true and its ring simply runs dry.  Told nothing, the servo would integrate fill=0 to its
    // -200 ppm clamp and `under` would climb at 344 Hz on a link doing exactly what it was asked.  suspended()
    // is a state read (Avdtp::state() == SUSPENDED), so this costs nothing to call every pass.
    btin.hold(sink.suspended());
    sink.l2().tickClock(millis());     // ms reference for the credit-starve fingerprint
    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        const BtSinkSession::Stats &st = session.stats();
        CONSOLE.print("hb streaming="); CONSOLE.print(session.state() == BtSinkSession::STREAMING ? 1 : 0);
        CONSOLE.print(" pkts="); CONSOLE.print(btin.pkts());
        CONSOLE.print(" frames="); CONSOLE.print(btin.frames());
        CONSOLE.print(" seqgaps="); CONSOLE.print(btin.seqGaps());
        CONSOLE.print(" under="); CONSOLE.print(btin.underruns());
        CONSOLE.print(" over="); CONSOLE.print(btin.overruns());
        CONSOLE.print(" bad="); CONSOLE.println(btin.badFrames());
        // The ring/servo line: fill is the servo's input, trim_ppm its output, rms the audio-clock-referenced
        // "is there really audio in here" measure (rmsAcc accumulates the per-block MEAN |L| -- divide by
        // blocks alone, see AudioInputBluetooth.h), crc200 a golden over the first 200 decoded blocks.
        CONSOLE.print("bt_sink fill="); CONSOLE.print(btin.fill());
        CONSOLE.print(" trim_ppm="); CONSOLE.print(btin.trimPpm());
        CONSOLE.print(" rms="); CONSOLE.print(btin.rmsBlocks() ? btin.rmsAcc() / btin.rmsBlocks() : 0);
        CONSOLE.print(" rms_blocks="); CONSOLE.print(btin.rmsBlocks());
        CONSOLE.print(" crc200=0x"); CONSOLE.println(btin.crc(), HEX);
        CONSOLE.print("bt_link links="); CONSOLE.print(st.links);
        CONSOLE.print(" lost="); CONSOLE.print(st.lost);
        CONSOLE.print(" closed="); CONSOLE.print(st.closed);
        CONSOLE.print(" reason=0x"); printHex8(st.lastReason);
        CONSOLE.print(" state="); CONSOLE.println(BtSinkSession::stateName(session.state()));
        CONSOLE.print("bt_hci ncmd="); CONSOLE.print(hci.ncmd());
        CONSOLE.print(" timeouts="); CONSOLE.print(hci.timeouts());
        CONSOLE.print(" starved="); CONSOLE.print(hci.starved());
        CONSOLE.print(" l2drop="); CONSOLE.print(sink.l2().dropped());
        CONSOLE.print(" l2frag="); CONSOLE.print(sink.l2().reasmFrags());        // ACL continuation fragments reassembled
        CONSOLE.print(" l2fragdrop="); CONSOLE.print(sink.l2().reasmDrops());    // partial PDUs discarded
        CONSOLE.print(" credmin="); CONSOLE.println(sink.l2().creditsMin());
        // AVRCP, LAST so no gate assertion above it moves.  avctp= is the only thing that says whether the peer
        // ever opened the control channel at all -- with the counters all zero, avctp=0 (never opened) and
        // avctp=1 (opened, silent) are the same reading from the bench, and they mean opposite things.
        CONSOLE.print("bt_avrcp avctp="); CONSOLE.print(sink.l2().byPsm(Avrcp::PSM) != nullptr ? 1 : 0);
        CONSOLE.print(" notif="); CONSOLE.print(sink.avrcp().notifications());
        CONSOLE.print(" unsup="); CONSOLE.print(sink.avrcp().unsupported());
        CONSOLE.print(" drop="); CONSOLE.print(sink.avrcp().dropped());
        CONSOLE.print(" vol="); CONSOLE.println(sink.avrcp().volume());
    }
}

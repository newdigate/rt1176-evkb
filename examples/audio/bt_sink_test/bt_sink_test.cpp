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

// The green User LED (LED_BUILTIN = pin 3 = GPIO_AD_04, D3) blinks at 1 Hz while a pairing window is open
// (pairingIndicator(), below); setup() drives it OFF at the top, card-absent included.  Here with the other
// build-time configuration rather than beside its users, because it is a knob and not a graph object: the
// matching cache variable lives in CMakeLists.txt (`-DBT_SINK_LED_ON=LOW`) and this #ifndef is only the
// fallback for a build that does not go through it.
// ** POLARITY: MEASURED on the bench 2026-09-11 (RUN 9) -- HIGH lights it.  The RevC3 header audit names the
// pad but not its active level and the only "active low" note in the core is the 1060's D8, a different
// board, so this was a GUESS until a person watched the LED blink through a boot window; it is no longer.
// QEMU cannot see it -- a silicon-only witness, which is why it took a bench run and not a gate. **
#ifndef BT_SINK_LED_ON
#define BT_SINK_LED_ON HIGH
#endif

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
// Set immediately after EACH BondStoreEeprom::load(bonds) in setup(), and required by the `forget` command
// (runCommand(), far below) before it is allowed to destroy the table.  It lives HERE, beside `bonds`,
// rather than with its reader, because setup() sets it and setup() is ~120 lines above that block -- the
// same placement lesson BT_SINK_LED_ON carries at the top of this file.
// Not belt-and-braces: serialEvent1() is reachable from the FIRST delay(10) in m2ReleaseWifiReset(),
// hundreds of lines before load() runs, because every delay() dispatches it.  `forget` is refused there
// TODAY only by accident of ordering -- canPair() is false because session.begin() has not run -- and
// moving begin() earlier for any reason would arm a wipe over bonds that were never READ:
// BondStoreEeprom::wipe() is `t.clear(); save(t)`, and BondTable::clear() sets dirty UNCONDITIONALLY
// (BondTable.cpp:28), so save() writes the canonical EMPTY image over the real persisted one.  A later
// load() then returns empty, `bonds_boot=0` reads perfectly normal, and the loss leaves no trace in the log.
static bool s_bondsLoaded = false;
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
    // The pairing LED, driven OFF FIRST -- before anything that can block.  btFirmwareDownload() plus up to ten
    // 500 ms HCI Reset attempts is ~20 s of setup(), and an unconfigured pad floats for all of it, which at the
    // bench reads as a dim or random glow on the very indicator being watched.  This write is also the whole of
    // the LED's card-absent behaviour: with no HCI the session never begins, so no window ever opens.
    pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, !BT_SINK_LED_ON);

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

    // s_bondsLoaded is set on BOTH arms, right where load() returns: the `forget` command reads it before it
    // is allowed to destroy the table, and the flag is per CALL SITE rather than one line after the #if so
    // that a future third arm cannot acquire the permission without doing the read.
#if defined(M2_BT_FORGET_BONDS)
    // load() first so `before` reflects what was really persisted before the wipe (bt_tone_test's note).
    BondStoreEeprom::load(bonds); s_bondsLoaded = true;
    { uint8_t before = bonds.count(); (void)BondStoreEeprom::wipe(bonds);
      CONSOLE.print("bonds_forgotten="); CONSOLE.println(before); }
#else
    BondStoreEeprom::load(bonds); s_bondsLoaded = true;
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
    // TARGET blocks x 128 samples at 44100 Hz: 16 * 128 * 10000 / 44100 = 464 (46.4 ms) at the NEW-42 default,
    // 232 at the old 8.  The phone uses this for lip sync, so a resized target is reported, not hidden.
    // A2dpSink's own default (460) was a standing guess; this is the figure the servo actually holds the ring at.
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

// NEW-46: the pairing window's two enums as console words.  `off` covers PAIR_NONE, which is what
// pairingReason() returns whenever the window is shut -- so the heartbeat field reads `pairing=off` with no
// separate open/closed test at the call site.
// Every enumerator is named and there is NO `default:`, deliberately: `default:` would suppress -Wswitch, and a
// future PairingReason added to BtSinkSession.h would then print `pairing=off` for an OPEN window -- silently
// breaking the `off` <-> shut equivalence the heartbeat comment and the edge prints below both rely on.  The
// return after the switch is the unreachable-value fallback the compiler still needs.
static const char *pairingReasonName(BtSinkSession::PairingReason r) {
    switch (r) { case BtSinkSession::PAIR_NONE: return "off";  case BtSinkSession::PAIR_BOOT: return "boot";
                 case BtSinkSession::PAIR_DROP: return "drop"; case BtSinkSession::PAIR_CMD:  return "cmd"; }
    return "off";
}
static const char *pairingEndName(BtSinkSession::PairingEnd e) {
    switch (e) { case BtSinkSession::PAIR_END_NONE:   return "none";   case BtSinkSession::PAIR_END_PAIRED: return "paired";
                 case BtSinkSession::PAIR_END_TIMEOUT: return "timeout"; case BtSinkSession::PAIR_END_CANCELLED: return "cancelled"; }
    return "none";
}

// The once-a-second heartbeat, and also what `status` prints on demand (Task 3): one body, two callers, so the
// console command and the timer can never disagree about what a heartbeat contains.
static void printHeartbeat() {
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
    // NEW-42 instrument, cumulative since START.  Placed right after bt_sink and before bt_link so the gate's
    // heartbeat-completeness check (a bt_hci line for every bt_sink line) covers it: a torn final block can
    // never leave a bt_jit line half-read.  gap*: the SOURCE's delivery cadence (main context, micros() per
    // accepted packet) -- the half that is REAL in QEMU, since the fake peer paces it in wall time.  ONE
    // CAVEAT, measured 2026-09-09 over two gate runs: the guest's micros() takes a single ~3.14 s step
    // relative to millis() mid-run, so gapmax_ms reads 3142/3141 and gbig 4/3 on a peer whose own log
    // (PEER-SOURCE-PROGRESS elapsed=5.2/10.2/15.2 for packets 50/100/150) proves it never paused.  The
    // COUNT is trustworthy there and the magnitudes are not: the buckets summed to exactly 149 -- one
    // interval per packet after the first -- in both runs.  fillmin/fillmax/trimlo/trimhi/overev/
    // reprimes/primed/prime_ms: the CONSUME side -- SILICON claims, exactly as under= is, because QEMU
    // walks update() on its own schedule and not at 44100/128 Hz.
    // ★ Two readings here do not mean what their neighbours mean, and a bench reader meets them at this
    // line rather than in the header:
    //   * fillmin reads RING and fillmax reads 0 UNTIL THE FIRST POP -- out-of-band sentinels, and legible
    //     as such because fill() sampled at a pop can be neither (one slot is the SPSC sentinel, so RING is
    //     unreachable, and a pop only happens with a block to pop, so 0 is too).  fillmin=RING means "no
    //     block has been popped yet", NOT "the ring stayed brim-full".
    //   * prime_ms is PER-STREAM while every other field on this line is LIFETIME: begin() re-arms it at
    //     each stream START, so after a reconnect it times the NEW stream's prime.  reprimes counts the
    //     mid-stream rebuilds and deliberately does NOT move it.  Diff two heartbeats across a drop and
    //     this one field goes backwards while its neighbours only climb.
    // prime_ms converts the START prime's length in blocks on the BLOCK clock (x 128 / 44100), so it needs no
    // wall clock in the ISR.  The 64-bit intermediate is not decoration: a prime that never reaches TARGET
    // never ends (AudioInputBluetooth.h's hazard note), primeBlocks() then climbs at 344/s, and in uint32 the
    // product wraps after ~98 s -- turning the one field that would expose a stalled source into a small,
    // healthy-looking number.
    CONSOLE.print("bt_jit gapmax_ms="); CONSOLE.print(btin.gapMaxUs() / 1000u);
    CONSOLE.print(" g30=");  CONSOLE.print(btin.gapBucket(0));
    CONSOLE.print(" g50=");  CONSOLE.print(btin.gapBucket(1));
    CONSOLE.print(" g80=");  CONSOLE.print(btin.gapBucket(2));
    CONSOLE.print(" g120="); CONSOLE.print(btin.gapBucket(3));
    CONSOLE.print(" gbig="); CONSOLE.print(btin.gapBucket(4));
    CONSOLE.print(" fillmin="); CONSOLE.print(btin.fillMin());
    CONSOLE.print(" fillmax="); CONSOLE.print(btin.fillMax());
    CONSOLE.print(" trimlo="); CONSOLE.print(btin.trimLo());
    CONSOLE.print(" trimhi="); CONSOLE.print(btin.trimHi());
    CONSOLE.print(" overev="); CONSOLE.print(btin.overEvents());
    CONSOLE.print(" reprimes="); CONSOLE.print(btin.reprimes());
    CONSOLE.print(" primed="); CONSOLE.print(btin.primed() ? 1 : 0);
    CONSOLE.print(" prime_ms="); CONSOLE.print((uint32_t)((uint64_t)btin.primeBlocks() * AUDIO_BLOCK_SAMPLES * 1000u / 44100u));
    // dry*: the DRY-SPELL instrument (spec s9) -- consecutive update()s on an EMPTY ring, which is what
    // sizes the threshold re-prime's N.  CONSUME SIDE, so like fillmin/fillmax/reprimes above these are
    // SILICON claims and the gate asserts none of them: QEMU walks update() on its own schedule, not at
    // 44100/128 Hz, so a dry spell measured there counts host scheduling and not the source.
    // Three readings, TWO populations (the header has the full note): drytot is every empty block;
    // d4..dbig and drymax are the spells that ENDED because a block arrived, which is the only ending
    // whose length measures the source.  A spell cut short by a SUSPEND or a stream loss is in drytot
    // alone, and a spell still running is in neither -- so drymax lags a dropout in progress.
    // ★ `drymax=0 drytot=0` beside `primed=0` means NOT MEASURED, not "the ring never ran dry": the
    // instrument starts at the END of the START prime, and a BT_SINK_PREFILL=0 build (the bench's
    // CONTROL arm) never completes one.  Same shape as the fillmin=RING sentinel above.
    // The seven fields are ~65 more characters on a heartbeat block that now runs 520 bytes at the first
    // beat rising to ~550 by the last (the spread is counter digits, not a before/after) -- ~47 ms of wire
    // time at 115200, 8N1.  The figure this comment carried until 2026-09-10 was "~345 (~30 ms)", true when
    // NEW-42 wrote it and false by the time NEW-46 added the pairing field: a live measurement in a comment
    // goes stale as lines are added, so it is re-derived here rather than carried.  They still fit the 4 KB
    // console TX extension setup() installs (~7 whole blocks of headroom), so the bench's
    // print-stalls-loop() observer effect (NEW-41, fixed by that extension) does not come back.
    CONSOLE.print(" drymax="); CONSOLE.print(btin.dryMax());
    CONSOLE.print(" drytot="); CONSOLE.print(btin.dryTotal());
    CONSOLE.print(" d4=");     CONSOLE.print(btin.dryBucket(0));
    CONSOLE.print(" d8=");     CONSOLE.print(btin.dryBucket(1));
    CONSOLE.print(" d16=");    CONSOLE.print(btin.dryBucket(2));
    CONSOLE.print(" d32=");    CONSOLE.print(btin.dryBucket(3));
    CONSOLE.print(" dbig=");   CONSOLE.println(btin.dryBucket(4));
    CONSOLE.print("bt_link links="); CONSOLE.print(st.links);
    CONSOLE.print(" lost="); CONSOLE.print(st.lost);
    CONSOLE.print(" closed="); CONSOLE.print(st.closed);
    CONSOLE.print(" reason=0x"); printHex8(st.lastReason);
    CONSOLE.print(" state="); CONSOLE.print(BtSinkSession::stateName(session.state()));
    // NEW-46: the pairing window, so a 1-in-30 sampled transcript still shows it.  reason names the OPEN window
    // (boot|drop|cmd); off when closed; secs the time left on this heartbeat's clock.  pairingReason() reads
    // PAIR_NONE whenever the window is shut, so `off` needs no separate open/closed test here.
    // COST, measured on the gate's own capture (`./run_qemu.sh`, then
    // `awk '{print length}'` over `grep '^bt_link ' build/sink.uart`) against the PRE-CHANGE shape, which ended
    // at `state=`: this line goes 59 -> 81 chars listening and 60 -> 82 connecting -- +22, the worst case,
    // ` pairing=boot secs=119` -- and 59 -> 78 streaming, +19 for ` pairing=off secs=0`.  The six-line block
    // grows by exactly that same +22/+19, and is 520 BYTES on the wire at the first heartbeat rising to ~550
    // by the last (line length + 2: println emits CR LF and run_qemu.sh strips the CR from the capture).  That
    // spread is COUNTER DIGITS growing over the run -- two heartbeats of the same build differ by more than
    // this field costs -- so take the deltas above as the before/after and never two block totals.
    // 22 chars x 10 bits (8N1: start + 8 + stop) / 115200 = 1.9 ms of WIRE time, which is not loop() time: the
    // 4 KB TX extension setup() installs holds seven whole ~550-byte blocks, so the print still returns without
    // waiting and NEW-41's print-stalls-loop() observer effect stays bought off.
    CONSOLE.print(" pairing="); CONSOLE.print(pairingReasonName(session.pairingReason()));
    CONSOLE.print(" secs="); CONSOLE.println(session.pairingRemainingMs(millis()) / 1000u);
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
    CONSOLE.print(" ans="); CONSOLE.print(sink.avrcp().answered());          // GetCapabilities / SetAbsoluteVolume, answered properly
    CONSOLE.print(" unsup="); CONSOLE.print(sink.avrcp().unsupported());     // NOT IMPLEMENTED (unknown PDU) or IPID
    CONSOLE.print(" drop="); CONSOLE.print(sink.avrcp().dropped());
    CONSOLE.print(" vol="); CONSOLE.println(sink.avrcp().volume());
}

// ONE definition of the `pairing=on` line, TWO callers.  Spec 5 splits WHO prints it deliberately -- the edge
// detector below announces the AUTOMATIC windows, runCommand() announces a commanded one, because extending an
// already-open window is not an edge and the person who typed `pair` deserves an answer either way -- but a
// format spelled twice drifts, and Task 4's gate greps this line by shape.
// `now` is the CALLER'S instant, not millis() taken here: the command handler passes the same `now` it passed
// enterPairing(), so pairingRemainingMs(now) is exactly the window length and the line reads secs=120.  An
// automatic window reads 119 instead -- measured in Task 2's run -- because the edge detector runs on a LATER
// loop pass, leaving ~119,9xx ms, which /1000u truncates.
static void printPairingOn(BtSinkSession::PairingReason r, uint32_t now) {
    CONSOLE.print("pairing=on reason="); CONSOLE.print(pairingReasonName(r));
    CONSOLE.print(" secs="); CONSOLE.println(session.pairingRemainingMs(now) / 1000u);
}

static void pairingIndicator() {
    // Edges are printed here, for the AUTOMATIC windows and for every close.  A commanded window prints its own
    // `pairing=on reason=cmd` from the command (runCommand(), below) -- see printPairingOn() -- so the rising
    // edge is skipped for PAIR_CMD.
    static bool wasOpen = false;
    bool open = session.pairingOpen();
    if (open && !wasOpen && session.pairingReason() != BtSinkSession::PAIR_CMD) {
        printPairingOn(session.pairingReason(), millis());
    }
    if (!open && wasOpen) { CONSOLE.print("pairing=off reason="); CONSOLE.println(pairingEndName(session.pairingEnd())); }
    wasOpen = open;
    bool lit = open && ((millis() / 500u) & 1u);
    digitalWrite(LED_BUILTIN, lit ? BT_SINK_LED_ON : !BT_SINK_LED_ON);
}

// --- console commands (NEW-46) -----------------------------------------------------------------------------
// Read from serialEvent1(), the weak hook the core's yield() dispatches whenever Serial1.available()
// (yield.cpp:40) -- and loop() calls yield() every pass -- so there is no loop() change and no polling.
// ★ ONE COMMAND PER INVOCATION, and that bound is the difference between a handler and a livelock.  yield()
// sets a `running` flag around this call, so the nested delay()->yield() that a blocking print performs is a
// NO-OP -- and the half of that worth knowing is not the re-entry it prevents but the cost it carries:
// EventResponder::runFromYield(), which drives the HciPump and therefore IS the audio path here, does not
// run again until this function RETURNS.  A drain loop that dispatched every queued line would stop
// returning under sustained input: `status` is 7 bytes in and ~550 out, an 80x amplification against an
// 11.5 KB/s console, and a held Enter key (~30 lines/s) already asks 16.5 KB/s of it -- the 4 KB TX ring
// stays full and HardwareSerialIMXRT::write() spins on a yield() that cannot pump.  The board then looks
// alive and is dead on Bluetooth: NEW-41's print-stalls-loop() effect with nothing bounding it.
// Returning after ONE dispatch leaves the rest in the core's 64-byte RX ring and the next top-level yield()
// takes them, with a pump pass in between; nothing is reordered or lost.  A flood that outruns 64 bytes has
// its excess dropped in the RX ISR (HardwareSerial.cpp's head/tail check, silently, no counter), which
// corrupts a line into a `cmd=?` rather than stalling the loop -- the trade this bound buys, and the safe
// direction to fail in given what `forget` does.
// ★ A PARTIAL LINE EXPIRES.  s_cmdLen is static and nothing in loop() clears it, so without a timeout an
// abandoned session SPLICES onto the next one: type `for`, change your mind, Ctrl-C the console, restart it,
// type `get` + Enter -- and the board runs `forget` and wipes the bond store.  A port dropped mid-word does
// the same.  2 s is far longer than any typed or scripted line takes to arrive and far shorter than the gap
// between two console sessions.
// A line is `\n` or `\r` terminated, case-insensitive, EXACT match, at most 15 chars.  A byte outside
// printable ASCII REFUSES THE WHOLE LINE rather than being deleted from it -- deleting was the first version
// and it was not safe, because `for<NUL>get` deletes to `forget`, which still matches and still fires the one
// destructive command.  Refusing REPORTS a dirty line instead of quietly cleaning it.  (The NUL this filter
// is for is host->board: a serial BREAK, or a DTR/RTS transition when a console attaches, presents as a 0x00
// on RX.  The NUL CLAUDE.md records at the head of a bench capture is the other direction, board->host, and
// this filter never sees it.)  Do not "improve" any of this into a prefix or substring match: the whole
// safety argument is that a corrupted `forget` misses rather than fires.
static char     s_cmd[16];
static uint8_t  s_cmdLen   = 0;
static bool     s_cmdOver  = false;   // over-length; takes precedence over dirty, since its report shows the truncation
static bool     s_cmdDirty = false;   // a non-printable byte arrived
static uint32_t s_lastByte = 0;       // millis() of the last byte that touched the line state -- the splice timeout above
// (s_bondsLoaded, the readiness flag `forget` requires, is declared beside `bonds` -- setup() sets it and
// sits ~120 lines above here; the full account of what a wipe over an unread table destroys is there.)
// ONE definition of the refusal line, and TWO reasons -- because canPair() is `LISTENING && !linkUp` and only
// one half of that is a link.  A single `reason=link_up` printed a flatly untrue sentence in the card-absent
// image (IDLE: the session never began), twice, directly under a heartbeat reading `state=idle links=0`, and
// sends a person at a bench hunting a connection that does not exist; MANUAL, CONNECTING and DISCONNECTING
// are the other three.  Two tokens and no trailing field, so the while-streaming case Task 4 greps -- a
// genuine link up -- still matches byte for byte.
static void printPairRefused() {
    const BtLink::LinkState ls = sink.link().linkState();
    CONSOLE.print("pairing=refused reason=");
    CONSOLE.println(ls == BtLink::LINK_UP || ls == BtLink::LINK_SECURE ? "link_up" : "not_listening");
}
// Open or extend the commanded window and say which way it went: the whole of `pair`, and the last step of
// `forget`.  void ON PURPOSE -- it prints both outcomes, so there is no return value for a caller to drop
// and no second `if` whose condition a reader has to re-derive from the statement above it.
static void openCommandedWindow(uint32_t now) {
    if (session.enterPairing(now, BtSinkSession::PAIR_CMD)) printPairingOn(BtSinkSession::PAIR_CMD, now);
    else printPairRefused();
}
static void runCommand(const char *c) {
    if (!strcmp(c, "pair")) {
        // ONE millis() for the open and the print (printPairingOn() takes the caller's instant), so the
        // printed secs is the window this call just set and not one loop pass of drift below it.
        openCommandedWindow(millis());
    } else if (!strcmp(c, "forget")) {
        // The bond table must be the STORE'S before we are allowed to destroy it -- see s_bondsLoaded above.
        if (!s_bondsLoaded) { CONSOLE.println("cmd=? \"forget\" (not ready)"); return; }
        // canPair() FIRST, and wipe ONLY if it holds: a refused forget must leave the bond table exactly as it
        // was.  The ordering is load-bearing, not stylistic -- wipe-then-check would destroy the bond that the
        // live link is using and report a refusal in the same breath.
        if (!session.canPair()) { printPairRefused(); return; }
        // The caller obligation BondStoreEeprom.h:12-23 states and this site had not discussed: the write is
        // IRQ-MASKED once per changed byte, and a filled journal costs a whole 4 KB sector erase -- an erase
        // CLUSTER of up to 59, seconds long, with the HciPump not running for any of it.  Tolerable HERE
        // BECAUSE canPair() excludes STREAMING (indeed any link up), so there is no media to starve -- that is
        // why it is tolerable, not merely why nobody has noticed it.
        // `bonds_forgotten=N` is the same line setup()'s M2_BT_FORGET_BONDS block prints, emitted again here
        // (that one is inside an #if and is not a shared printer); N is the count BEFORE the wipe, as there.
        // wipe() returns TRUE when the empty image reached the store; false only if the table reported no
        // change (impossible -- clear() dirties unconditionally) or the serialiser short-filled its buffer
        // (unreachable by construction), so a false here means a BondTable invariant broke.  It is reported on
        // its own line rather than dropped: the RAM table really is empty, so `bonds_forgotten=N` stays true,
        // but the bonds come back at the next boot and a bench reader must be told which of those two it has.
        const uint8_t before = bonds.count();
        const bool wiped = BondStoreEeprom::wipe(bonds);
        CONSOLE.print("bonds_forgotten="); CONSOLE.println(before);
        if (!wiped) CONSOLE.println("bonds_wipe=fail (RAM table cleared, store NOT written -- the bonds return at the next boot)");
        // millis() RE-READ after the wipe, never carried across it: those masked writes and any erase cluster
        // are tens of ms to seconds, and a `now` taken before them would set a deadline short by exactly that
        // while printing secs=120.
        openCommandedWindow(millis());
    } else if (!strcmp(c, "status")) {
        // `cmd=status` on its own line FIRST.  printHeartbeat() opens with the literal `hb `, so an on-demand
        // block is byte-indistinguishable from a timed one -- and NEW-42's bench derived `over 13.09/min` /
        // `under 16.08/min` by treating heartbeat blocks as a clock, which an untagged `status` would silently
        // inflate.  It is also the token that proves the command landed.  (`last` is a loop() local static, so
        // `status` does NOT re-phase the timer: the timed cadence stays a clock, deliberately.)
        CONSOLE.println("cmd=status");
        printHeartbeat();
    } else {
        // The echo is UNESCAPED, and `"` survives the printable-ASCII filter: `cmd=? "a"b"` is ambiguous, and
        // a 10-char line spelling `pairing=on` fits s_cmd and comes back inside one of these.  The 15-char
        // buffer is what bounds the damage -- nothing longer can be quoted back -- so a grep over this
        // capture should anchor on the tokens the sketch prints ITSELF (`^pairing=on reason=...`,
        // `^bonds_forgotten=N$`) and never accept one that arrives inside a cmd=? echo.
        CONSOLE.print("cmd=? \""); CONSOLE.print(c); CONSOLE.println("\"");
    }
}
void serialEvent1() {
    // ★ The name hard-codes Serial1 and so steps outside the CONSOLE alias the rest of this file uses.
    // Correct HERE -- the example is rt1176-only and CONSOLE is Serial1 there -- but the header above says
    // this file's preamble is kept in step across three sketches, and on the teensy4 core (rt1062) the VCOM
    // is Serial6 (CLAUDE.md): copied across, the handler binds LPUART6 and simply never fires, with
    // everything else in the sketch running perfectly.
    const uint32_t now = millis();
    // The splice timeout, tested BEFORE the drain so a line abandoned in a previous console session cannot
    // have its tail completed by the bytes this call is about to read.  The two refusal flags are cleared
    // with it: a lone stray byte (a DTR transition's NUL as a console attaches) must not go on to refuse a
    // line typed seconds later.
    if ((s_cmdLen || s_cmdOver || s_cmdDirty) && now - s_lastByte > 2000u) {
        s_cmdLen = 0; s_cmdOver = false; s_cmdDirty = false;
    }
    while (CONSOLE.available()) {
        int ch = CONSOLE.read(); if (ch < 0) break;
        s_lastByte = now;
        if (ch == '\n' || ch == '\r') {
            s_cmd[s_cmdLen] = 0;
            if (s_cmdOver)       { CONSOLE.print("cmd=? \""); CONSOLE.print(s_cmd); CONSOLE.println("...\" (too long)"); }
            else if (s_cmdDirty) { CONSOLE.print("cmd=? \""); CONSOLE.print(s_cmd); CONSOLE.println("\" (bad byte)"); }
            else if (s_cmdLen)   { runCommand(s_cmd); }
            s_cmdLen = 0; s_cmdOver = false; s_cmdDirty = false;
            // ONE command per invocation -- the livelock bound at the top of this block.  `return`, not
            // `continue`: whatever else is queued stays in the RX ring and the next top-level yield()
            // dispatches it with an EventResponder (HciPump) pass in between.  A CR LF pair therefore costs
            // two calls, the second on an EMPTY line, which is ignored.
            return;
        }
        if (ch < 0x20 || ch > 0x7E) { s_cmdDirty = true; continue; }   // NUL, control bytes, non-ASCII: the LINE is refused, the byte is not deleted from it
        if (s_cmdLen < sizeof s_cmd - 1) s_cmd[s_cmdLen++] = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
        else s_cmdOver = true;                               // sticky to the terminator: the whole line is refused, not its first 15 chars
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
    pairingIndicator();                // window edge prints + the User LED at 1 Hz
    sink.l2().tickClock(millis());     // ms reference for the credit-starve fingerprint
    static uint32_t last = 0;
    if (millis() - last >= 1000) { last = millis(); printHeartbeat(); }
}

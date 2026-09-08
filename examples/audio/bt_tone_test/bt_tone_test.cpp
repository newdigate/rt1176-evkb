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
#include <BtSession.h>
#include <BondTable.h>
#include <BondStoreEeprom.h>

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
static BtSession  session(src);
static BondTable bonds;                                // NEW-34: bonded devices, persisted in the EEPROM emulation (BondStoreEeprom, offset 4000)
static AudioSynthWaveformSine toneGen;                // "tone" collides with core_pins.h's tone(pin,freq,ms)
static AudioOutputBluetooth   btout;
static AudioConnection pc0(toneGen, 0, btout, 0);
static AudioConnection pc1(toneGen, 0, btout, 1);      // same tone to L and R

#if defined(M2_BT_SOAK)
// --- NEW-34 piece 5: the unattended resilience soak driver ---------------------------------------
// Every M2_BT_SOAK_PERIOD_MS while STREAMING, force a drop with a RAW HCI_Disconnect (0x0406, reason 0x13)
// on the live handle -- deliberately NOT session.disconnect(): that is the MANUAL hook and BtSession would
// treat the loss as intended.  A raw disconnect arrives as Disconnection_Complete with BtLink outside its
// DISCONNECT op, so BtLink goes LINK_LOST, A2dpSource::tick() tears the media path down (m_avdtp.reset();
// m_l2.reset()), BtSession records the loss and goes WAITING -- the AUTOMATIC path a range loss triggers.
// M2_BT_SOAK_RETRY_NOW (default ON) then fires session.retryNow() to reconnect at once, so a bounded gate
// run measures the reconnect MACHINERY inside its budget rather than waiting out BtSession's own retry
// TIMER (that cadence is [lifecycle]'s claim).  The unattended SILICON soak should build with
// M2_BT_SOAK_RETRY_NOW=OFF, so it exercises the session's own field retry policy instead of
// short-circuiting it.  A cycle that does not re-stream within M2_BT_SOAK_RECONNECT_BOUND_MS is COUNTED
// as a failure and the driver moves on -- a soak records failures, it does not stop on the first.
// NOTE: this driver's "reconnect_ms_max" is drop-submit -> re-STREAM (the soak's own measured interval);
// the heartbeat's separate "bt_link ... reconnect_ms=" is BtSession's lostAt -> attempt-OK.  The two are
// DIFFERENT intervals -- the heartbeat's starts when the loss is recorded, this one when the forced
// disconnect was submitted -- so do not expect them to agree.
#include <malloc.h>
extern unsigned long _ebss;
static uint32_t soakHeapUsed()     { struct mallinfo mi = mallinfo(); return (uint32_t)mi.uordblks; }   // newlib live total; the arena is OCRAM (.bss.dma), clear of the DTCM stack
static uint32_t soakStackFreeMin() { static uint32_t fl = 0xFFFFFFFF; register uint32_t sp __asm__("sp");
                                     uint32_t f = sp - (uint32_t)&_ebss; if (f < fl) fl = f; return fl; }   // running floor: stack grows down from DTCM top
static const uint16_t OP_DISCONNECT = 0x0406;
#define M2_BT_SOAK_DROP_BOUND_MS 2000    // a short, separate bound for "the forced disconnect was never even seen as a loss" -- distinct from the reconnect bound
enum SoakState : uint8_t { SOAK_STREAM, SOAK_DROPPING, SOAK_RECONNECTING, SOAK_DONE };
static SoakState s_soakSt = SOAK_STREAM;
static uint32_t s_soakAt = 0;                    // period reference while streaming; bound reference while dropping/reconnecting
static uint32_t s_soakCycles = 0, s_soakReconnects = 0, s_soakFails = 0, s_soakReconnectMsMax = 0;
static uint32_t s_soakSubmitFails = 0;           // hci.submit() for the forced disconnect returned anything but Hci::OK
static bool     s_soakBaseSet = false;
static uint8_t  s_soakL2FreeBase = 0, s_soakL2FreeRestreamMin = 0xFF;   // the slot-leak baseline and the floor seen at re-stream entries
static uint8_t  s_soakL2FreeLossMin = 0xFF;      // the floor seen at LOSS time (see soakOnLoss) -- 0xFF means never sampled
static void soakPrintU8OrNa(uint8_t v) { if (v == 0xFF) CONSOLE.print("n/a"); else CONSOLE.print(v); }
static void soakOnStream() {                     // from onStreamCb(streaming=true): sample the structural baseline
    uint8_t f = src.l2().freeSlots();
    if (!s_soakBaseSet) { s_soakL2FreeBase = f; s_soakBaseSet = true; }
    else if (f < s_soakL2FreeRestreamMin) s_soakL2FreeRestreamMin = f;
}
// From onStreamCb(streaming=false): A2dpSource::tick() calls m_l2.begin() unconditionally on EVERY attempt
// (a memset of the channel table), so freeSlots() sampled at STREAMING entry (soakOnStream above) is always
// a fresh table and can never show a skipped teardown.  At loss time, though, A2dpSource::tick() has just
// run m_avdtp.reset()/m_l2.reset() (BEFORE BtSession fires this callback, in the same tick() call), so this
// is the one point that can actually see the teardown: L2cap::MAX_CHANNELS (5) free on a correct teardown,
// fewer on a skipped one.
static void soakOnLoss() {
    uint8_t f = src.l2().freeSlots();
    if (f < s_soakL2FreeLossMin) s_soakL2FreeLossMin = f;
}
static void soakDiscDone(void *, Hci::Error e, const Hci::Reply *r) {
    // Hci::DoneFn's `reply` is null on failure (Hci.h), so a TIMEOUT/FRAMING/etc. before any reply arrived
    // must not dereference it -- print 0xFF as the "no status available" marker in that case.
    if (e != Hci::OK || (r && r->status != 0)) {
        CONSOLE.print("soak_drop_status=0x"); printHex8(r ? r->status : 0xFF); CONSOLE.println();
    }
}
static void soakPrintDone() {
    CONSOLE.print("soak_done cycles="); CONSOLE.print(s_soakCycles);
    CONSOLE.print(" reconnects="); CONSOLE.print(s_soakReconnects);
    CONSOLE.print(" fails="); CONSOLE.print(s_soakFails);
    CONSOLE.print(" reconnect_ms_max="); CONSOLE.print(s_soakReconnectMsMax);
    CONSOLE.print(" l2_free_base="); CONSOLE.print(s_soakL2FreeBase);
    CONSOLE.print(" l2_free_restream_min="); soakPrintU8OrNa(s_soakL2FreeRestreamMin);
    CONSOLE.print(" l2_free_loss_min="); soakPrintU8OrNa(s_soakL2FreeLossMin);
    CONSOLE.print(" l2_leak=");
    if (s_soakBaseSet && s_soakL2FreeRestreamMin != 0xFF)
        CONSOLE.print(s_soakL2FreeBase > s_soakL2FreeRestreamMin ? s_soakL2FreeBase - s_soakL2FreeRestreamMin : 0);
    else CONSOLE.print("n/a");                    // no reconnect ever streamed -- 0 here would be a VACUOUS pass, not a measurement
    CONSOLE.print(" submit_fails="); CONSOLE.print(s_soakSubmitFails);
    CONSOLE.print(" bonds="); CONSOLE.println(bonds.count());
}
static void soakTick(uint32_t now) {
    switch (s_soakSt) {
    case SOAK_STREAM:
        // Checked BEFORE the streaming-state gate below: a failure on the LAST cycle leaves the session
        // out of STREAMING when this case is re-entered, and the old order re-armed the period forever
        // instead of ever reaching soak_done.
        if (M2_BT_SOAK_CYCLES && s_soakCycles >= (uint32_t)M2_BT_SOAK_CYCLES) { s_soakSt = SOAK_DONE; soakPrintDone(); break; }
        if (session.state() != BtSession::STREAMING) { s_soakAt = now; break; }        // not streaming yet: keep re-arming the period
        if (now - s_soakAt >= (uint32_t)M2_BT_SOAK_PERIOD_MS) {
            uint16_t h = src.link().handle();
            uint8_t p[3] = { (uint8_t)h, (uint8_t)(h >> 8), 0x13 };                    // handle, reason 0x13 Remote User Terminated
            if (hci.submit(OP_DISCONNECT, p, 3, soakDiscDone, nullptr) == Hci::OK) {
                s_soakCycles++; s_soakAt = now; s_soakSt = SOAK_DROPPING;
                CONSOLE.print("soak_drop cycle="); CONSOLE.print(s_soakCycles); CONSOLE.print(" handle=0x"); printHex16(h); CONSOLE.println();
            } else {
                s_soakSubmitFails++;
            }
        }
        break;
    case SOAK_DROPPING:                                                                 // wait for BtSession to see the loss
        if (session.state() == BtSession::WAITING) {
#if defined(M2_BT_SOAK_RETRY_NOW)
            session.retryNow();
#endif
            s_soakSt = SOAK_RECONNECTING;
        } else if (now - s_soakAt >= (uint32_t)M2_BT_SOAK_DROP_BOUND_MS) {
            s_soakFails++; s_soakSt = SOAK_STREAM; s_soakAt = now; CONSOLE.println("soak_fail stage=drop-not-seen"); }
        break;
    case SOAK_RECONNECTING:
        if (session.state() == BtSession::STREAMING) {
            uint32_t ms = now - s_soakAt; if (ms > s_soakReconnectMsMax) s_soakReconnectMsMax = ms;
            s_soakReconnects++; s_soakSt = SOAK_STREAM; s_soakAt = now;
        } else if (now - s_soakAt >= (uint32_t)M2_BT_SOAK_RECONNECT_BOUND_MS) {
            s_soakFails++; s_soakSt = SOAK_STREAM; s_soakAt = now; CONSOLE.println("soak_fail stage=reconnect-timeout"); }
        break;
    case SOAK_DONE: break;
    }
}
static void soakPrintLine() {                    // every 2 s (NEW-8 cadence): the health signature
    CONSOLE.print("bt_soak cycles="); CONSOLE.print(s_soakCycles);
    CONSOLE.print(" reconnects="); CONSOLE.print(s_soakReconnects);
    CONSOLE.print(" fails="); CONSOLE.print(s_soakFails);
    CONSOLE.print(" reconnect_ms_max="); CONSOLE.print(s_soakReconnectMsMax);
    CONSOLE.print(" l2_free="); CONSOLE.print(src.l2().freeSlots());
    CONSOLE.print(" l2_free_base="); CONSOLE.print(s_soakL2FreeBase);
    CONSOLE.print(" l2_free_min="); soakPrintU8OrNa(s_soakL2FreeRestreamMin);
    CONSOLE.print(" l2_free_loss_min="); soakPrintU8OrNa(s_soakL2FreeLossMin);
    CONSOLE.print(" l2_leak=");
    if (s_soakBaseSet && s_soakL2FreeRestreamMin != 0xFF)
        CONSOLE.print(s_soakL2FreeBase > s_soakL2FreeRestreamMin ? s_soakL2FreeBase - s_soakL2FreeRestreamMin : 0);
    else CONSOLE.print("n/a");
    CONSOLE.print(" handle=0x"); printHex16(src.link().handle());
    CONSOLE.print(" bonds="); CONSOLE.print(bonds.count());
    CONSOLE.print(" heap="); CONSOLE.print(soakHeapUsed());
    CONSOLE.print(" stack_free_min="); CONSOLE.println(soakStackFreeMin());
}
#endif /* M2_BT_SOAK */

static uint32_t nowMs() { return millis(); }
static void btLog(void *, const char *s) { CONSOLE.println(s); }
static void onStreamCb(void *, bool streaming, uint8_t reason, BtSession::By by) {
    if (streaming) {
        btout.begin(src);
        src.l2().resetCreditStats();   // NEW-34 piece 4: zero the credit-leak counters at each STREAMING entry
#if defined(M2_BT_SOAK)
        soakOnStream();                // NEW-34 piece 5: sample the slot-leak baseline at every STREAMING entry
#endif
        const char *bs = by == BtSession::BY_PAGED ? "paged" : by == BtSession::BY_INQUIRY ? "inquiry" : by == BtSession::BY_INCOMING ? "incoming" : "none";
        CONSOLE.print("streaming by="); CONSOLE.print(bs);
        CONSOLE.print(" bitpool="); CONSOLE.print(src.sbcParams().bitpool);
        CONSOLE.print(" frames_per_pkt="); CONSOLE.print(btout.framesPerPacket());
        CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
    } else {
        btout.end();
#if defined(M2_BT_SOAK)
        soakOnLoss();                   // NEW-34 piece 5: sample the slot-leak floor at LOSS (teardown already ran)
#endif
        CONSOLE.print("bt_dropped reason=0x"); printHex8(reason);
        CONSOLE.print(" links="); CONSOLE.println(session.stats().links);
    }
}
static void onAttemptCb(void *, A2dpSource::Result r, const char *pairedBy) {
    BondStoreEeprom::save(bonds);
    CONSOLE.print("a2dp="); CONSOLE.print(A2dpSource::resultName(r));
    CONSOLE.print(" bonds="); CONSOLE.print(bonds.count());
    CONSOLE.print(" paired_by="); CONSOLE.println(pairedBy);
}
#if defined(M2_BT_ACL_TRACE)
static void aclTrace(void *, bool out, uint16_t handle, const uint8_t *pdu, uint16_t len) {
    // ★ SKIP RTP media packets (A2DP media payload starts with RTP V2/PT96 = 0x80 0x60).
    // The trace exists for the AVDTP DISCOVER/SDP SIGNALLING diagnosis, which is pre-streaming.
    // Tracing the media firehose is not just noise -- it THROTTLES the audio: one 132-byte media
    // packet is ~439 chars of hex, which takes ~38 ms to print at 115200 baud, and CONSOLE.print
    // BLOCKS when its TX buffer fills.  That print runs inside L2cap's send loop, so it paced media
    // to ~26 packets/s (11520 chars/s / 439) -> a periodic crackle instead of a tone.  Measured on
    // silicon 2026-09-04: with media traced, 26 fps + crackle; skipping it (or the default no-trace
    // build), 344 fps + drops=0 + a clean tone.  Signalling is low-rate, so it never throttles.
    // pdu = [l2cap len(2)][cid(2)][payload]; the payload starts at pdu[4].
    if (len >= 6 && pdu[4] == 0x80 && pdu[5] == 0x60) return;
    CONSOLE.print("acl_trace dir="); CONSOLE.print(out ? "out" : "in");
    CONSOLE.print(" h=0x"); printHex16(handle);
    CONSOLE.print(" t="); CONSOLE.print(micros());
    CONSOLE.print(" hex=");
    for (uint16_t i = 0; i < len; i++) { printHex8(pdu[i]); if (i + 1 < len) CONSOLE.print(' '); }
    CONSOLE.println();
}
#endif
static void onEvt(void *, uint8_t c, const uint8_t *p, uint8_t l) { src.onEvent(c, p, l); }
static void onAclThunk(void *, uint16_t h, uint8_t pb, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l, pb); }   // Hci::AclFn puts pb before data; A2dpSource/L2cap take it last

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
    // NEW-34: the bond store.  load() once, after Hci::begin(); save() after EVERY connect() return
    // (an erased stale bond must persist too).  M2_BT_FORGET_BONDS wipes it instead -- the control
    // arm that proves a headset out of pairing mode refuses us WITHOUT a bond.
#if defined(M2_BT_FORGET_BONDS)
    // NOTE (deviation from the plan): the plan's snippet read bonds.count() here without a prior
    // load(), which always reads 0 (a freshly-constructed BondTable) -- it would not actually print
    // "the pre-wipe count", just always print 0 (the same class of bug as the original always-1).
    // load() first so `before` reflects what was really persisted before it is wiped.
    BondStoreEeprom::load(bonds);
    { uint8_t before = bonds.count(); (void)BondStoreEeprom::wipe(bonds);
      CONSOLE.print("bonds_forgotten="); CONSOLE.println(before); }
#else
    BondStoreEeprom::load(bonds);
#endif
    src.setBonds(&bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
#if defined(M2_BT_SOAK)
    CONSOLE.print("soak period_ms="); CONSOLE.print(M2_BT_SOAK_PERIOD_MS); CONSOLE.print(" cycles="); CONSOLE.print(M2_BT_SOAK_CYCLES);
    CONSOLE.print(" bound_ms="); CONSOLE.print(M2_BT_SOAK_RECONNECT_BOUND_MS);
#if defined(M2_BT_SOAK_RETRY_NOW)
    CONSOLE.println(" retry_now=1");
#else
    CONSOLE.println(" retry_now=0");
#endif
#endif
#if defined(M2_BT_ACL_TRACE)
    src.l2().onAclTrace(aclTrace, nullptr);
#endif
#if defined(M2_BT_LEGACY_PIN)
    src.setLegacyPin(true);
#endif
#if defined(M2_BT_INQUIRY_LIAC)
    src.link().setInquiryLap(0x9E8B00);                   // limited inquiry: bench knob for sinks that hide from GIAC in pairing mode
    CONSOLE.println("inquiry_lap=LIAC");
#endif
#if defined(M2_BT_SUPERVISION_MS)
    src.link().setSupervisionSlots((uint16_t)((uint32_t)M2_BT_SUPERVISION_MS * 1000u / 625u));  // ms -> 0.625 ms slots
#endif
    session.onStream(onStreamCb, nullptr);
    session.onAttempt(onAttemptCb, nullptr);
#if defined(M2_BT_RETRY_MS)
    session.setRetryMs(M2_BT_RETRY_MS);
#endif
    // begin the session only if HCI came up; with no card it never reaches STREAMING and the heartbeat stays vacuous
    if (s_hciSt == Hci::OK) {
#if defined(M2_BT_TARGET_NAME)
        session.begin(&bonds, M2_BT_TARGET_NAME, s_aclNum, millis());
#else
        session.begin(&bonds, nullptr, s_aclNum, millis());
#endif
    } else {
        CONSOLE.println("a2dp=deferred (no HCI: card absent)");
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
    session.tick(millis());
    src.service();
    src.l2().tickClock(millis());     // NEW-34 piece 4: ms reference for the credit-starve fingerprint
    btout.poll();
#if defined(M2_BT_SOAK)
    soakTick(millis());
    { static uint32_t lastSoak = 0; if (millis() - lastSoak >= 2000) { lastSoak = millis(); soakPrintLine(); } }
#endif
    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        const BtSession::Stats &st = session.stats();
        const char *bs = st.by == BtSession::BY_PAGED ? "paged" : st.by == BtSession::BY_INQUIRY ? "inquiry" : st.by == BtSession::BY_INCOMING ? "incoming" : "none";
        CONSOLE.print("hb streaming="); CONSOLE.print(src.started() ? 1 : 0);
        CONSOLE.print(" blocks="); CONSOLE.print(btout.blocks());
        CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
        CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
        CONSOLE.print(" hw="); CONSOLE.println(btout.queueHighWater());
        // self-clock health (its own line: the card-absent gate anchors the whole hb line above)
        CONSOLE.print("bt_clock resyncs="); CONSOLE.print(btout.resyncs()); CONSOLE.print(" burst_max="); CONSOLE.println(btout.burstMax());
        CONSOLE.print("bt_link links="); CONSOLE.print(st.links);
        CONSOLE.print(" lost="); CONSOLE.print(st.lost);
        CONSOLE.print(" reason=0x"); printHex8(st.lastReason);
        CONSOLE.print(" by="); CONSOLE.print(bs);
        CONSOLE.print(" reconnect_ms="); CONSOLE.print(st.reconnectMs);
        CONSOLE.print(" scan="); CONSOLE.print(session.wantPageScan() ? 1 : 0);
        CONSOLE.print(" role="); CONSOLE.println(src.link().role() ? 's' : (src.link().linkState() >= BtLink::LINK_UP ? 'm' : '-'));
        CONSOLE.print("bt_hci ncmd="); CONSOLE.print(hci.ncmd());
        CONSOLE.print(" timeouts="); CONSOLE.print(hci.timeouts());
        CONSOLE.print(" starved="); CONSOLE.print(hci.starved());
        CONSOLE.print(" l2drop="); CONSOLE.print(src.l2().dropped());
        CONSOLE.print(" l2frag="); CONSOLE.print(src.l2().reasmFrags());        // ACL continuation fragments reassembled
        CONSOLE.print(" l2fragdrop="); CONSOLE.print(src.l2().reasmDrops());    // partial PDUs discarded
        CONSOLE.print(" credmin="); CONSOLE.println(src.l2().creditsMin());
        // NEW-34 piece 4: the credit-leak soak record.  outstanding == sent-returned (<= maxCredits when healthy);
        // a lost NCP shows as starve_max_ms growing run-over-run with credmin pinned at 0.
        CONSOLE.print("bt_cred sent="); CONSOLE.print(src.l2().pktsSent());
        CONSOLE.print(" returned="); CONSOLE.print(src.l2().creditsReturned());
        CONSOLE.print(" starves="); CONSOLE.print(src.l2().starves());
        CONSOLE.print(" starve_max_ms="); CONSOLE.print(src.l2().starveMaxMs());
        CONSOLE.print(" clamp="); CONSOLE.println(src.l2().clampHits());
        CONSOLE.print("bt_mem idle_blocks="); CONSOLE.print(btout.idleBlocks());
        CONSOLE.print(" paused_blocks="); CONSOLE.println(btout.pausedBlocks());
    }
}

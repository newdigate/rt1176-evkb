/* acid_box - the audio+display integration capstone.
 * Spec: docs/superpowers/specs/2026-08-17-acid-box-capstone-design.md
 * Landscape (layout C): docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Audio core (previous commit) + layout-C (landscape) UI and the glue between
 * them.  The glue is deliberately THIN: every callback reads the widget, maps
 * it, and writes the engine; no UI state mirrors engine state except the three
 * `shown*` caches, which exist only to stop LVGL repainting unchanged text.
 *
 * BOOT STATE IS STOPPED AND SILENT, AND THAT IS A CONTRACT, NOT AN OMISSION.
 * The transport is configured (tempo, loop, looping) but never played, so
 * AudioStepSequencer::currentStep() stays at its -1 "nothing has played yet"
 * sentinel, the note pump has nothing to drain and audio_probe_poll() returns
 * before it can print.  The ABSENCE of ACIDBOX_BAR lines is therefore the
 * assertion that the box came up quiet; a build that hums on power-up shows up
 * as bars appearing where the gate expects none, not as a subjective judgement
 * about a transcript.
 */
#include <Arduino.h>
#include <string.h>               // memset -- named explicitly, as the sibling
                                  // audio examples name <math.h>, rather than
                                  // relying on Arduino.h to drag it in.
#include <math.h>                 // powf/logf/roundf/lroundf for the knob maps
#include <stdio.h>                // snprintf -- INTEGER conversions only, below
#include <Audio.h>
// Audio.h pulls in every codec driver EXCEPT this one -- control_wm8960.h is
// in its include list and control_wm8962.h is not, so the WM8962 header must be
// named explicitly, exactly as acid_bass_test and audiooutput_i2s_test do.
#include "control_wm8962.h"
#if defined(M2_BT_OUT)
#include <HardwareSerial.h>
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
#endif
#include <Wire.h>                 // Wire2 = LPI2C5: codec AND touch controller
#include "Display.h"
#include "gt911.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "lvgl_gt911_indev.h"
#include "synthui_rotary_knob.h"
#include "synthui_rotary_knob_gpu.h"
#include "synthui_step.h"
#if defined(ACIDBOX_LOOPSTAT)
#include "loopstat_pct.h"
#endif

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

// rt1176-only example: LPUART1 console, which the imxrt1176 core names Serial1.
#define CONSOLE Serial1
#define ACIDBOX_VERSION 1

#if defined(M2_BT_OUT)
// =============================================================================
// Bluetooth A2DP output (M2_BT_OUT) -- preamble.
//
// Board power-up, BT UART firmware download, HCI Reset and identity are COPIED
// VERBATIM from examples/audio/bt_tone_test/bt_tone_test.cpp (itself copied
// from m2_hci_probe.cpp) -- that sequence is proven on silicon; see BT-3 phase
// 4 in the M.2 Bluetooth programme notes.  Keep the two files in step for this
// shared portion.
//
// ★ ONE RENAME FROM THE SOURCE: bt_tone_test.cpp names its HciPump object
// `pump`, which collides with acid_box's own `IntervalTimer pump` (the
// note-event pump declared after the audio graph below) -- so the HciPump
// object here is `btPump` instead.  Every other identifier is unchanged.
// =============================================================================

// --- the Bluetooth transport: same objects as m2_hci_probe -----------------
static HciTransport hciIo(Serial2);
static Hci hci(hciIo);
static HciPump btPump;
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
// The idle callback every blocking BT wait takes (BtFwLoader::run, Hci::run,
// BtLink/A2dpSource::connect).  bt_tone_test passes delay(1); acid_box passes
// idleUi() -- defined after audio_probe_poll() below -- which runs ONE pass of
// the main loop's UI half (a yield for the HciPump, the LVGL loop, the audio
// probe) so the panel stays live and touchable through the ~12 s firmware
// download and the ~17 s connect.  Measured before this existed (NEW-33
// baseline, 2026-09-04): the first loop iteration after boot was 16.8 s long
// with the UI dead for all of it, and every 5 s connect retry blocks the same
// way whenever no headset answers -- the "noticeably less responsive" of the
// issue, which the steady-state numbers never showed.
static void idleUi();

// L2cap::begin()'s aclCredits argument -- the Total_Num_ACL_Data_Packets field
// from Read_Buffer_Size, captured below in probeIdentity().
static uint8_t s_aclNum = 0;

// --- identity ---------------------------------------------------------------
static void probeIdentity() {
    Hci::Reply r;
    Hci::Error e = hci.run(OP_READ_LOCAL_VER, nullptr, 0, &r, 1000, idleUi);
    if (e == Hci::OK && r.len >= 8) {
        CONSOLE.print("hci_version: hci_ver="); CONSOLE.print(r.params[0]);
        CONSOLE.print(" hci_rev=0x");     printHex16((uint16_t)(r.params[1] | (r.params[2] << 8)));
        CONSOLE.print(" lmp_ver=");       CONSOLE.print(r.params[3]);
        CONSOLE.print(" manufacturer=0x"); printHex16((uint16_t)(r.params[4] | (r.params[5] << 8)));
        CONSOLE.print(" lmp_subver=0x");  printHex16((uint16_t)(r.params[6] | (r.params[7] << 8)));
        CONSOLE.println();
    } else printFail("hci_version", e, r, "short_reply");

    e = hci.run(OP_READ_BD_ADDR, nullptr, 0, &r, 1000, idleUi);
    if (e == Hci::OK && r.len >= 6) { CONSOLE.print("bd_addr="); printBd(r.params); CONSOLE.println(); }
    else printFail("bd_addr", e, r, "short_reply");

    e = hci.run(OP_READ_BUFFER_SIZE, nullptr, 0, &r, 1000, idleUi);
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
    Hci::Error e = hci.run(OP_RESET, nullptr, 0, &r, 1000, idleUi);
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
    s_btFwSt = btLoader.run(3000, 500, 30000, idleUi);
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
#endif // M2_BT_OUT

/* GC355 working pool -- synthui_knob_test's siting and reasoning verbatim:
 * EXTMEM (SDRAM), not DMAMEM (a 2 MB pool overflows the 512K OCRAM at link
 * time), zeroed before vg_lite_init_mem because startup never zeroes EXTMEM
 * (the RT1062 DMAMEM lesson, GPU edition).  In QEMU the chip-ID probe reads 0
 * and nothing here is touched past that read. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256
static bool s_gpu = false;

/* --- audio graph ---------------------------------------------------------- *
 * ★ DECLARATION ORDER IS UPDATE ORDER and both the transport and the
 * sequencer depend on it (AudioStream.h appends at the tail; software_isr
 * walks head-first).  `transport` must precede `seq` -- the constructor
 * signature makes that structural rather than advisory -- and `acid` must
 * precede `rms` and `out` so the analyzer and the codec see the block the
 * voice just produced rather than the previous one. */
AudioTransport      transport;
AudioStepSequencer  seq(transport);
AudioSynthAcidBass  acid;
AudioAnalyzeRMS     rms;
AudioOutputI2S      out;
AudioControlWM8962  wm;
AudioConnection     cRms(acid, 0, rms, 0);
AudioConnection     cL(acid, 0, out, 0);
AudioConnection     cR(acid, 0, out, 1);

#if defined(M2_BT_OUT)
// A second sink on the same voice: the local WM8962 path above and this one
// both read `acid` every block, so the headset plays exactly what the
// speaker/line-out plays.  AudioOutputBluetooth is externally clocked here
// (setSelfClock(false) in setup()) -- the I2S SAI ISR already walks the graph
// via AudioOutputI2S `out`'s DMA completion, so btout.poll() only drains.
static A2dpSource        src(hci, hciIo);
static BtSession         session(src);
static AudioOutputBluetooth btout;
static BondTable bonds;   // NEW-34: bonded devices, persisted in the EEPROM emulation (BondStoreEeprom, offset 4000)
static AudioConnection   cBtL(acid, 0, btout, 0);
static AudioConnection   cBtR(acid, 0, btout, 1);   // mono acid duplicated to L+R
static void btLog(void *, const char *s) { CONSOLE.println(s); }
static void onEvt(void *, uint8_t c, const uint8_t *p, uint8_t l) { src.onEvent(c, p, l); }
static void onAclThunk(void *, uint16_t h, uint8_t pb, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l, pb); }   // Hci::AclFn puts pb before data; A2dpSource/L2cap take it last
static bool s_btBegun = false;
// NEW-34 piece 2: BtSession callbacks -- session.tick() drives A2dpSource's attempt state machine
// (boot walk + inquiry, lost-peer retry forever, page-scan-when-idle) and fires these at a link's
// start/end.  acid_box is externally clocked (setSelfClock(false)): the I2S SAI ISR already walks the
// graph via AudioOutputI2S `out`'s DMA completion, so btout.poll() only drains -- same shape as
// bt_tone_test.cpp's onStreamCb/onAttemptCb (Task 8), the reference pattern for this wiring.
static void onStreamCb(void *, bool streaming, uint8_t reason, BtSession::By by) {
    if (streaming) { btout.setSelfClock(false); btout.begin(src); s_btBegun = true; src.l2().resetCreditStats();
        CONSOLE.print("bt_streaming by="); CONSOLE.print(by == BtSession::BY_INCOMING ? "incoming" : by == BtSession::BY_INQUIRY ? "inquiry" : "paged");
        CONSOLE.print(" bitpool="); CONSOLE.print(src.sbcParams().bitpool);
        CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
    } else { btout.end(); s_btBegun = false; CONSOLE.print("bt_dropped reason=0x"); CONSOLE.println(reason, HEX); }
}
static void onAttemptCb(void *, A2dpSource::Result r, const char *pairedBy) {
    BondStoreEeprom::save(bonds);
    CONSOLE.print("a2dp="); CONSOLE.print(A2dpSource::resultName(r)); CONSOLE.print(" paired_by="); CONSOLE.println(pairedBy);
}
// NEW-33 fix 1: the transport's TX ring must cover the IW416's 7-credit ACL
// window (hci_buffer acl_num=7) so L2cap::service()'s write never spins on the
// core's 64-byte Serial2 ring.  64 is the core's built-in ring (HardwareSerial2.cpp
// tx_buffer2[64]); the hci_txring line at connect reads the real total instead.
static_assert(HciTransport::TX_EXTRA + 64u >= 7u * (9u + L2cap::MAX_PAYLOAD),
              "HciTransport::TX_EXTRA must cover 7 x (9 + L2cap::MAX_PAYLOAD) (NEW-33)");
#endif

#if defined(ACIDBOX_LOOPSTAT)
/* --- ACIDBOX_LOOPSTAT: where does the main loop spend its time? ---------- *
 * Bench instrument for NEW-33 (spec 2026-09-04-acid-box-bt-ui-responsiveness-
 * design.md §1).  Never in the gated build: -DACIDBOX_LOOPSTAT=1 is a CMake
 * option that defaults OFF, and OFF leaves the loadable image byte-identical.
 *
 * Three lines, once a second, beside bt_hb:
 *   loopstat  loops= max_us= yield= svc= poll= enc= drain= txb= print= lvgl= probe= wiggle=
 *   framestat frames= med_us= max_us= flips=+ wait_us=+
 *   touchstat n= p50_us= p95_us= max_us=          (only when samples arrived)
 *
 * ★ EVERYTHING HERE LIVES IN FLASH.  The M2_BT_OUT bench build has ~1 KB of
 * ITCM left (.text.itcm 0x3FBD0 of 0x40000), so every function below carries
 * LOOPSTAT_FN: section .progmem.loopstat, collected by the core's *(.progmem*)
 * rule into .text.progmem, which is XIP and already AX.  Only the micros()
 * laps in loop() are inline.  A print that runs once a second does not need
 * ITCM; a lap that runs per iteration is a handful of instructions.
 *
 * ★ The slots are laps, not nested timers: each LS_LAP charges the time since
 * the previous lap to one slot, so the sum of the slots IS the iteration.  The
 * summary's own print time lands in the NEXT window's `print` (the counters
 * are reset inside the summary, before its lap) -- honest, one window late.
 *
 * ★ The loopstat line is printed in TWO printf calls: Print::printf formats
 * into a 128-byte stack buffer (Print.cpp PRINTF_BUF_SIZE) and CLAMPS, and the
 * whole line can exceed that.  Two calls, one line, no newline in between. */
#define LOOPSTAT_FN __attribute__((section(".progmem.loopstat"), noinline))

enum { LS_YIELD, LS_SVC, LS_POLL, LS_PRINT, LS_LVGL, LS_PROBE, LS_SLOTS };
static uint32_t ls_slotUs[LS_SLOTS];        /* cumulative us per slot, this window */
static uint32_t ls_loops = 0, ls_maxUs = 0;
static uint32_t ls_windowMs = 0;            /* millis() at the last summary */
static inline __attribute__((always_inline)) uint32_t ls_lap(int slot, uint32_t t0)
{
    const uint32_t t = micros();
    ls_slotUs[slot] += t - t0;
    return t;
}
#define LS_LAP(slot) (ls_t = ls_lap(slot, ls_t))

/* frames: LVGL display events.  A frame counts at REFR_READY only if
 * RENDER_READY fired since REFR_START (an empty refresh cycle is not a frame). */
static uint32_t        ls_frameBuf[64];
static loopstat_ring_t ls_frameRing;
static uint32_t ls_frames = 0;              /* rendered frames this window */
static uint32_t ls_lastFrameUs = 0;         /* REFR_READY of the previous rendered frame, 0 = none */
static uint32_t ls_refrStartUs = 0;
static bool     ls_rendered = false;
static uint32_t ls_flips0 = 0, ls_wait0 = 0;

/* touch: the age, at presentation, of the OLDEST input change a frame carries.
 * The stamp is taken when LVGL's indev read returns a changed (state, x, y)
 * and no stamp is pending; it closes at the first rendered REFR_READY whose
 * REFR_START came AFTER the stamp (so the frame's render began after the
 * input was processed and its invalidation queued).  Ring of the 256 most
 * recent samples; the line reports over the ring, so the last touchstat of a
 * drag is the drag's distribution. */
static uint32_t        ls_touchBuf[256];
static loopstat_ring_t ls_touchRing;
static uint32_t ls_touchN = 0, ls_touchNew = 0;
static uint32_t ls_touchStampUs = 0;
static bool     ls_touchPending = false;
static lv_indev_read_cb_t ls_origRead = nullptr;
static lv_indev_state_t   ls_prevState = LV_INDEV_STATE_RELEASED;
static lv_point_t         ls_prevPoint = {0, 0};

LOOPSTAT_FN static void ls_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:   ls_refrStartUs = micros(); ls_rendered = false; break;
    case LV_EVENT_RENDER_READY: ls_rendered = true; break;
    case LV_EVENT_REFR_READY: {
        if (!ls_rendered) break;
        const uint32_t now = micros();
        ls_frames++;
        if (ls_lastFrameUs) loopstat_ring_push(&ls_frameRing, now - ls_lastFrameUs);
        ls_lastFrameUs = now;
        if (ls_touchPending && (int32_t)(ls_refrStartUs - ls_touchStampUs) >= 0) {
            loopstat_ring_push(&ls_touchRing, now - ls_touchStampUs);
            ls_touchPending = false;
            ls_touchN++; ls_touchNew++;
        }
        break;
    }
    default: break;
    }
}

LOOPSTAT_FN static void ls_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ls_origRead(indev, data);
    if (data->state != ls_prevState ||
        data->point.x != ls_prevPoint.x || data->point.y != ls_prevPoint.y) {
        ls_prevState = data->state; ls_prevPoint = data->point;
        if (!ls_touchPending) { ls_touchStampUs = micros(); ls_touchPending = true; }
    }
}

/* wiggle: a 15 ms LVGL timer sweeping all eight sound knobs through a triangle
 * over the full ±140° bounded range -- 100 steps per half-sweep (1.5 s), knob k
 * offset by 12 steps so the rotors are never in phase.  set_angle ONLY: the
 * widget sends VALUE_CHANGED from its input path (synthui_rotary_knob.cpp),
 * never from set_angle, so the synth parameters do not move -- the panel
 * animates flat-out while the sound is unchanged.  Boot/current angles are
 * saved at wiggle-on and restored at wiggle-off so the picture matches the
 * engine again afterwards. */
static lv_obj_t   *ls_knob[8];
static int         ls_nKnob = 0;
static float       ls_saved[8];
static lv_timer_t *ls_wiggleTimer = nullptr;
static uint32_t    ls_wiggleStep = 0;
static bool        ls_wiggle = false;
#define LS_KNOB(x) (ls_knob[ls_nKnob++] = (x))

LOOPSTAT_FN static float ls_tri(uint32_t step)
{
    const uint32_t s = step % 200u;
    const float u = (s < 100u) ? (float)s / 100.0f : (float)(200u - s) / 100.0f;
    return -140.0f + 280.0f * u;
}
LOOPSTAT_FN static void ls_wiggle_cb(lv_timer_t *t)
{
    (void)t;
    ls_wiggleStep++;
    for (int k = 0; k < ls_nKnob; k++)
        synthui_rotary_knob_set_angle(ls_knob[k], ls_tri(ls_wiggleStep + 12u * (uint32_t)k));
}
LOOPSTAT_FN static void ls_title_cb(lv_event_t *e)
{
    (void)e;
    if (!ls_wiggle) {
        for (int k = 0; k < ls_nKnob; k++) ls_saved[k] = synthui_rotary_knob_get_angle(ls_knob[k]);
        ls_wiggleTimer = lv_timer_create(ls_wiggle_cb, 15, NULL);
        ls_wiggle = true;
    } else {
        lv_timer_delete(ls_wiggleTimer); ls_wiggleTimer = nullptr;
        for (int k = 0; k < ls_nKnob; k++) synthui_rotary_knob_set_angle(ls_knob[k], ls_saved[k]);
        ls_wiggle = false;
    }
    CONSOLE.printf("wiggle=%d\n", ls_wiggle ? 1 : 0);
}
LOOPSTAT_FN static void ls_attach_title(lv_obj_t *title)
{
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(title, 24);          /* a finger-sized target around the small label */
    lv_obj_add_event_cb(title, ls_title_cb, LV_EVENT_CLICKED, NULL);
}
LOOPSTAT_FN static void ls_attach_display(lv_display_t *disp)
{
    loopstat_ring_init(&ls_frameRing, ls_frameBuf, 64);
    loopstat_ring_init(&ls_touchRing, ls_touchBuf, 256);
    lv_display_add_event_cb(disp, ls_refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, ls_refr_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, ls_refr_cb, LV_EVENT_REFR_READY, NULL);
    ls_flips0 = lvgl_mipi_panel_flips();
    ls_wait0  = lvgl_mipi_panel_wait_us();
    ls_windowMs = millis();
}
LOOPSTAT_FN static void ls_attach_touch(lv_indev_t *indev)
{
    ls_origRead = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, ls_read_cb);
}

LOOPSTAT_FN static void ls_summary(void)
{
    const uint32_t now = millis();
    if (now - ls_windowMs < 1000u) return;
    ls_windowMs = now;

    uint32_t encUs = 0, drainUs = 0, txb = 0;
#if defined(M2_BT_OUT)
    static uint32_t enc0 = 0, drn0 = 0, txb0 = 0;
    const uint32_t enc1 = btout.encodeUs(), drn1 = btout.drainUs(), txb1 = btout.txBytes();
    encUs = enc1 - enc0; drainUs = drn1 - drn0; txb = txb1 - txb0;
    enc0 = enc1; drn0 = drn1; txb0 = txb1;
#endif
    /* two calls, one line: Print::printf clamps at 128 bytes (see the header note) */
    CONSOLE.printf("loopstat loops=%lu max_us=%lu yield=%lu svc=%lu poll=%lu enc=%lu",
                   (unsigned long)ls_loops, (unsigned long)ls_maxUs,
                   (unsigned long)ls_slotUs[LS_YIELD], (unsigned long)ls_slotUs[LS_SVC],
                   (unsigned long)ls_slotUs[LS_POLL], (unsigned long)encUs);
    CONSOLE.printf(" drain=%lu txb=%lu print=%lu lvgl=%lu probe=%lu wiggle=%d\n",
                   (unsigned long)drainUs, (unsigned long)txb, (unsigned long)ls_slotUs[LS_PRINT],
                   (unsigned long)ls_slotUs[LS_LVGL], (unsigned long)ls_slotUs[LS_PROBE],
                   ls_wiggle ? 1 : 0);

    uint32_t sorted[256];
    uint32_t n = loopstat_ring_sorted(&ls_frameRing, sorted);
    const uint32_t flips = lvgl_mipi_panel_flips(), wait = lvgl_mipi_panel_wait_us();
    CONSOLE.printf("framestat frames=%lu med_us=%lu max_us=%lu flips=+%lu wait_us=+%lu\n",
                   (unsigned long)ls_frames,
                   (unsigned long)loopstat_pct_sorted(sorted, n, 50),
                   (unsigned long)loopstat_pct_sorted(sorted, n, 100),
                   (unsigned long)(flips - ls_flips0), (unsigned long)(wait - ls_wait0));
    ls_flips0 = flips; ls_wait0 = wait;
    ls_frames = 0; loopstat_ring_reset(&ls_frameRing);

    if (ls_touchNew) {
        n = loopstat_ring_sorted(&ls_touchRing, sorted);
        CONSOLE.printf("touchstat n=%lu p50_us=%lu p95_us=%lu max_us=%lu\n",
                       (unsigned long)ls_touchN,
                       (unsigned long)loopstat_pct_sorted(sorted, n, 50),
                       (unsigned long)loopstat_pct_sorted(sorted, n, 95),
                       (unsigned long)loopstat_pct_sorted(sorted, n, 100));
        ls_touchNew = 0;
    }
    memset(ls_slotUs, 0, sizeof ls_slotUs);
    ls_loops = 0; ls_maxUs = 0;
}
#else
#define LS_LAP(slot) ((void)0)
#define LS_KNOB(x)   (x)
#endif /* ACIDBOX_LOOPSTAT */

#if defined(M2_BT_OUT)
/* bt_mem heap=/stack_free_min= -- NEW-34 piece 2 soak instrument (Task 15 R5 asserts heap flat / the
 * floor stable over a 30 min soak).  heap is newlib's live allocation total via mallinfo(): the malloc
 * arena (_heap_start/_heap_end, imxrt1176.ld) lives in OCRAM (.bss.dma), well clear of the DTCM stack,
 * so this is safe to sample every second regardless of stack depth.  stack_free_min is a running floor
 * of (sp - _ebss): the stack lives in DTCM above .bss, growing down from _estack with nothing else
 * between .bss and the stack, so sp - &_ebss is exactly the untouched headroom below the current frame,
 * and the floor is the closest any pass has come to exhausting it. Placed AFTER the loopstat block (not
 * inside the BT globals above it) so LOOPSTAT_FN -- used below when ACIDBOX_LOOPSTAT is also on, per
 * Task 15's "acid_box witness" bench build -- is already defined (or correctly absent) at this point;
 * the preprocessor is single-pass, so using it any earlier would silently see an undefined token. */
#include <malloc.h>
extern "C" char *_sbrk(int);
extern unsigned long _ebss;
#if defined(ACIDBOX_LOOPSTAT)
LOOPSTAT_FN
#endif
static uint32_t btMemHeapUsed() { struct mallinfo mi = mallinfo(); return (uint32_t)mi.uordblks; }
#if defined(ACIDBOX_LOOPSTAT)
LOOPSTAT_FN
#endif
static uint32_t btMemStackFreeMin() {
    static uint32_t floor = 0xFFFFFFFF;
    register uint32_t sp __asm__("sp");
    uint32_t freeNow = sp - (uint32_t)&_ebss;      // stack grows down from DTCM top; _ebss is DTCM .bss end
    if (freeNow < floor) floor = freeNow;
    return floor;
}
// NEW-34 piece 4: the per-second BT report (bt_hb/bt_link/bt_cred/bt_mem) lives in FLASH -- loop() is
// ITCM-resident and this M2_BT_OUT bench build sits at the ITCM limit, so the print block overflowed ITCM
// inline (measured +28 B when the bt_cred fields were added).  Routed to flash regardless of ACIDBOX_LOOPSTAT
// via .progmem (the core collects *(.progmem*) into XIP flash); loop() keeps only the once-a-second gate + call.
__attribute__((section(".progmem.btreport"), noinline))
static void acidBtReport() {
    const BtSession::Stats &st = session.stats();
    CONSOLE.print("bt_hb blocks="); CONSOLE.print(btout.blocks());
    CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
    CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
    CONSOLE.print(" pcmdrops="); CONSOLE.print(btout.pcmDrops());  // PCM-ring overflow = loop too slow to encode
    CONSOLE.print(" hw="); CONSOLE.println(btout.queueHighWater());
    CONSOLE.print("bt_link links="); CONSOLE.print(st.links);
    CONSOLE.print(" lost="); CONSOLE.print(st.lost);
    CONSOLE.print(" reason=0x"); CONSOLE.print(st.lastReason, HEX);
    CONSOLE.print(" reconnect_ms="); CONSOLE.print(st.reconnectMs);
    CONSOLE.print(" scan="); CONSOLE.println(session.wantPageScan() ? 1 : 0);
    CONSOLE.print("bt_cred sent="); CONSOLE.print(src.l2().pktsSent());   // NEW-34 piece 4 soak record
    CONSOLE.print(" returned="); CONSOLE.print(src.l2().creditsReturned());
    CONSOLE.print(" l2frag="); CONSOLE.print(src.l2().reasmFrags());
    CONSOLE.print(" l2fragdrop="); CONSOLE.print(src.l2().reasmDrops());
    CONSOLE.print(" credmin="); CONSOLE.print(src.l2().creditsMin());
    CONSOLE.print(" starves="); CONSOLE.print(src.l2().starves());
    CONSOLE.print(" starve_max_ms="); CONSOLE.print(src.l2().starveMaxMs());
    CONSOLE.print(" clamp="); CONSOLE.println(src.l2().clampHits());
    CONSOLE.print("bt_mem heap="); CONSOLE.print(btMemHeapUsed());
    CONSOLE.print(" stack_free_min="); CONSOLE.println(btMemStackFreeMin());
}
#endif /* M2_BT_OUT */

/* --- the preset: a classic 16-step acid line (A minor-ish), documented so
 * the first frame and the audio windows are deterministic.  note 0 = rest. */
struct PresetStep { uint8_t note; bool gate, accent, slide; };
static const PresetStep kPreset[16] = {
    { 33, true,  true,  false },   /* 0  A1 accent          */
    { 33, true,  false, false },   /* 1  A1                 */
    {  0, false, false, false },   /* 2  rest  <- the gate's edit target */
    { 45, true,  false, true  },   /* 3  A2 slide into 4    */
    { 36, true,  false, false },   /* 4  C2                 */
    {  0, false, false, false },   /* 5  rest               */
    { 33, true,  false, false },   /* 6  A1                 */
    { 40, true,  true,  false },   /* 7  E2 accent          */
    { 33, true,  false, false },   /* 8  A1                 */
    {  0, false, false, false },   /* 9  rest               */
    { 43, true,  false, true  },   /* 10 G2 slide into 11   */
    { 45, true,  false, false },   /* 11 A2                 */
    { 33, true,  true,  false },   /* 12 A1 accent          */
    { 31, true,  false, false },   /* 13 G1                 */
    {  0, false, false, false },   /* 14 rest               */
    { 33, true,  false, true  },   /* 15 A1 slide into 0    */
};

/* USER CONTEXT ONLY: seq.step() takes __disable_irq() guards internally
 * (seq_step.h), which is only safe from a context the audio ISR can preempt. */
static void load_preset(void)
{
    for (int i = 0; i < 16; i++)
        seq.step(i, kPreset[i].note, kPreset[i].gate,
                 kPreset[i].accent, kPreset[i].slide);
}

/* --- default patch: the boot angles in §4 of the spec map to these -------- */
static void default_patch(void)
{
    acid.waveform(WAVEFORM_SAWTOOTH);
    acid.cutoff(800.0f);
    acid.resonance(0.55f);
    acid.envMod(0.6f);
    acid.decay(0.28f);
    acid.accent(0.7f);
    acid.distortion(0.15f);
    acid.subLevel(0.2f);
    acid.slideTime(0.06f);
    acid.level(0.5f);              /* fixed; no knob (spec §4) */
}

/* --- DIAGNOSTIC SCAFFOLDING, build-diag/ ONLY ---------------------------- *
 * Compiled in only under -DACIDBOX_DIAG=1, which the shipped build/ (and so the
 * gate, and so the golden ELF) never sets.  It exists to answer one question
 * over SWD while the MCU-Link VCOM is dead: WHERE IN setup() IS THE FIRMWARE AT
 * A GIVEN MILLISECOND.  A debugger halt reports a frozen systick that is
 * INDISTINGUISHABLE from a firmware freeze, so the only way to tell "died at t"
 * from "was halted at t" is to know what setup() is legitimately doing at t.
 * Everything here is volatile and read by symbol; nothing prints, because the
 * VCOM is a separate bench fault. */
#ifdef ACIDBOX_DIAG
volatile uint32_t g_diag_t[16];        /* setup() checkpoint timestamps, ms */
volatile uint32_t g_diag_n;            /* how many checkpoints have been passed */
volatile float    g_diag_rms[16];      /* per-step peak RMS, NEVER cleared */
volatile uint32_t g_diag_bars;         /* bars completed since the diag play() */
volatile uint32_t g_diag_played;       /* millis() at the synthetic play()      */
static inline void diag_mark(void)
{ if (g_diag_n < 16) g_diag_t[g_diag_n++] = millis(); }
#else
#define diag_mark() ((void)0)
#endif

/* --- note-event pump: PIT context, immune to UI frame time ---------------- *
 *
 * WHY AN ISR AT ALL: the drain must not be delayed by an LVGL frame, and a
 * full-screen software render on the RK055 is tens of milliseconds -- far
 * longer than the 2.9 ms window in which the sequencer's event queue is valid.
 * Draining from loop() would lose whole steps whenever the UI redrew.
 *
 * ★ PRIORITY 224, BELOW THE AUDIO SOFTWARE ISR'S 208, AND THAT IS THE WHOLE
 * DESIGN.  IntervalTimer defaults to 128, which is HIGHER priority than
 * AudioStream's software_isr (AudioStream.cpp sets IRQ_SOFTWARE to 208), so a
 * default-priority pump can land in the middle of one update_all() pass --
 * specifically between transport.update() and seq.update(), which run
 * adjacently in construction order.  In that window samples() has already
 * advanced while the queue still holds the PREVIOUS block, so the "new block"
 * test below would fire, re-apply stale events, and then mark the new block
 * drained: the real note-ons of that block would be silently dropped.  The
 * window is a microsecond wide and the pump fires 1000 times a second, so it
 * is rare, nondeterministic, and exactly the class of defect this tree refuses
 * to ship.  Running BELOW the audio ISR removes it outright -- software_isr can
 * never be preempted by this handler, so this handler can never observe a
 * half-finished pass.
 */
IntervalTimer pump;
static void pump_isr(void)
{
    /* ★ ONCE PER AUDIO BLOCK, NOT ONCE PER TIMER TICK.  seq_step.h is explicit
     * that reading the queue does not consume it: update() clears it at the top
     * of each block and refills it, so it keeps reporting the same events until
     * the next block arrives.  At 1 kHz against a 344.5 Hz block rate every
     * event would otherwise be applied about three times, retriggering the
     * voice's envelope for the whole 2.9 ms of the block.  The transport's own
     * sample counter is the audio-clock way to say "a new block has arrived",
     * and it is the same guard drainSequencer() uses in step_seq_test. */
    static uint64_t lastDrained = 0;
    SeqEvent ev[AudioStepSequencer::MAX_EVENTS];
    int n = 0;

    /* ★ ONE CRITICAL SECTION COVERING BOTH READS.  software_isr (208) preempts
     * this handler (224), so reading samples() and the queue as two separate
     * steps could straddle a block boundary: the counter from the old block and
     * the events from the new one, which double-applies now and drops later.
     * Taking both under one __disable_irq() makes the pair atomic. */
    __disable_irq();
    const uint64_t now = transport.samples();
    if (now != lastDrained) {
        lastDrained = now;
        n = seq.eventCount();
        if (n > AudioStepSequencer::MAX_EVENTS) n = AudioStepSequencer::MAX_EVENTS;
        for (int i = 0; i < n; i++) ev[i] = seq.eventAt(i);
    }
    __enable_irq();

    /* Applied OUTSIDE the critical section on purpose: noteOn/noteOff take
     * their own __disable_irq() guards (synth_acidbass.h), and holding
     * interrupts off across up to eight voice updates would delay the audio ISR
     * this handler exists to serve. */
    for (int i = 0; i < n; i++) {
        if (ev[i].type == SEQ_NOTE_ON) acid.noteOn(ev[i].note, ev[i].velocity, ev[i].slide);
        else                           acid.noteOff(ev[i].note);
    }
}

/* --- the logical frame ------------------------------------------------------
 * The 1280x720 frame LVGL renders -- the port's landscape canvas.  Declared
 * here rather than in the UI section's geometry block (which is placed from
 * it) because the rotation witnesses below read the canvas at this size.  The
 * canvas is the panel turned on its side (lvgl_mipi_panel.h), and the guard's
 * index arithmetic is only right if the two agree, so that is asserted rather
 * than assumed. */
static constexpr int UI_W = 1280, UI_H = 720;
static_assert((uint32_t)UI_W == PANEL_HEIGHT && (uint32_t)UI_H == PANEL_WIDTH,
              "the logical frame must be the RK055 panel turned on its side");

/* --- rotation witnesses (spec section 7) ------------------------------------
 * ACIDBOX_ROT: the port's PXP present counters.  An image that fell back to
 * the portrait create_db path never prints this line, so the gate fails it BY
 * NAME rather than passing on everything else.
 *
 * ACIDBOX_ROT_EQ: the EQUALITY GUARD -- gate-compared, never re-goldened, in
 * the spirit of the knob's delta guard.  After flip_sync() the presented
 * buffer and the canvas hold the same frame (single thread: no render can
 * intervene between the present and this check), so every 16th PHYSICAL row
 * -- which is every 16th LOGICAL column -- is compared against a CPU rotation
 * of the canvas.  Sampled, not exhaustive: it catches any stale region >= 16
 * logical columns wide at ~230 KB of reads per buffer (~460 KB in all, the
 * presented buffer and the canvas); the port's host property test is the
 * exhaustive one.  The formula here is written out on purpose rather than
 * calling the port's helper -- a guard that shares the code it checks is not
 * a check.
 *
 * WHY NO RENDER CAN INTERVENE: every compare runs from setup() (after the
 * boot frame) or from audio_probe_poll(), which loop() and idleUi() run AFTER
 * lvgl_rt1176_loop() has returned -- never from inside an LVGL callback -- so
 * LVGL cannot start a refresh into the canvas between a flip_sync() below and
 * the last compare that follows it.
 *
 * TWO SHAPES, ONE COMPARE.
 *   * BOOT: rot_equality_check_boot(), synchronous -- flip_sync() (which may
 *     wait up to a frame) and all 80 sampled rows in one call.  The stall is
 *     irrelevant in setup(), and it is the first ACIDBOX_ROT_EQ line (pass=1).
 *   * PER BAR: INCREMENTAL.  rot_equality_begin() arms a check when the
 *     sequencer enters step 8 (mid-bar), and rot_equality_step() runs from
 *     every audio_probe_poll() pass while the transport plays, comparing
 *     ROT_EQ_CHUNK sampled rows per pass until all 80 are done; the 15->0 seam
 *     then prints the finished verdict.  MEASURED ON THE EVKB before this
 *     shape existed: the synchronous per-bar guard cost a median 31.4 ms
 *     (max 38.8 ms) inside the seam pass, the loopstat probe slot read 59 ms
 *     in seam seconds against ~18 ms otherwise, touch p95 read 52-96 ms while
 *     dragging, and a frame was skipped once per bar -- with the M2_BT_OUT PCM
 *     ring covering only ~93 ms.
 *
 * WHY EACH CHUNK IS VALID ON ITS OWN.  With no flip pending, the presented
 * buffer holds the rotated canvas: the rotated flush_cb renders, PXP-presents
 * into the back buffer and requests the flip in one call, and the vsync ISR
 * retires it into scanned_fb().  (Unless that present failed or its flip
 * timed out -- then scanned_fb() is stale and the compare fails, which is the
 * verdict those faults deserve; the gate names both separately too.)  So a
 * step pass with a flip PENDING is SKIPPED -- flip_sync() would block up to a
 * frame and serialise the loop to vsync, the stall this shape exists to
 * remove -- and a pass with none pending calls flip_sync() (non-blocking in
 * that state: it only consumes the retire count) and compares.  No render or
 * present can run between that test and the end of the chunk (single thread;
 * outside LVGL), and the ISR cannot CREATE a pending flip, only retire one.
 * Chunks of one check may compare different frames; each is a true statement
 * about the frame it read.
 *
 * THE NO-PENDING PREDICATE is isrs + timeouts >= flips, from the port's public
 * counters (lvgl_mipi_panel.cpp's ownership table): flips counts at the
 * request, and a pending flip ends EITHER by an ISR retire (isrs) OR by
 * flip_sync()'s 40 ms thread-side abandon (timeouts), which clears the
 * pending pointer with no retire.  So isrs + timeouts == flips - pending,
 * exactly.  ★ NOT flips == isrs: that is exact only while timeouts == 0 --
 * after ONE true abandon it reads "pending" FOREVER, every later check would
 * starve and be counted a FAIL, and a fence timeout would masquerade as an
 * image inequality for the rest of the run.  The port's one documented
 * double-count (an ISR retire landing inside the abandon, both counters
 * ticking for one flip) can only make this predicate say "go" while a flip
 * is pending -- flip_sync() then waits, a bounded stall and a still-valid
 * compare -- never "skip" while none is, so it cannot starve a check.
 *
 * ITS COST IS REPORTED: us= on the ROT_EQ line is the LARGEST SINGLE-PASS
 * STALL of the last check to reach a verdict -- the flip_sync() call plus one
 * pass's compares, the print excluded.  For the boot check that pass is the
 * whole check (its flip_sync() may wait); per bar it is one chunk, the number
 * that bounds loop latency.  us= is INFORMATIONAL and never gated: QEMU time
 * is fiction.  The guard is not compiled out anywhere -- the gate ELF is the
 * silicon ELF.
 *
 * ROTWIT_FN puts these functions in FLASH (.progmem, XIP): their cost is
 * SDRAM reads, not instruction fetches, and the default build's ITCM headroom
 * had fallen to 836 B.  The per-pass "is a check active" test is inlined at
 * the call site in audio_probe_poll(), so a pass with no check in progress
 * pays one load and a branch in ITCM and never makes the flash call. */
#define ROTWIT_FN __attribute__((section(".progmem.acid_rotwit"), noinline))
#define ROT_EQ_STRIDE 16u                              /* every 16th physical row */
#define ROT_EQ_ROWS   (PANEL_HEIGHT / ROT_EQ_STRIDE)   /* 80 sampled rows */
#define ROT_EQ_CHUNK  8u                               /* sampled rows per loop pass */
static_assert(PANEL_HEIGHT % ROT_EQ_STRIDE == 0,
              "the sampled rows must cover the panel to its last stride");
static uint32_t s_rotEqPass = 0, s_rotEqFail = 0, s_rotEqUs = 0;
/* The incremental check in progress (touched from the loop thread only). */
static bool     s_rotEqActive = false;   /* armed, no verdict yet */
static bool     s_rotEqOk     = true;    /* every chunk so far matched */
static uint32_t s_rotEqRow    = 0;       /* next SAMPLED row index, 0..ROT_EQ_ROWS */
static uint32_t s_rotEqMaxUs  = 0;       /* its largest single-pass cost so far */

/* Sampled rows [from, to): sampled row k is physical row k*ROT_EQ_STRIDE.
 * False at the first mismatch, or if either buffer does not exist.  ROTWIT_FN
 * rather than inline: an out-of-line copy the compiler chose to emit would
 * otherwise land in ITCM. */
ROTWIT_FN static bool rot_eq_rows(uint32_t from, uint32_t to)
{
    const uint32_t *fb = (const uint32_t *)lvgl_mipi_panel_scanned_fb();
    const uint32_t *cv = (const uint32_t *)lvgl_mipi_panel_canvas();
    if (fb == nullptr || cv == nullptr) return false;
    for (uint32_t k = from; k < to; k++) {
        const uint32_t py = k * ROT_EQ_STRIDE;
        const uint32_t *row = fb + (size_t)py * PANEL_WIDTH;
        for (uint32_t px = 0; px < PANEL_WIDTH; px++) {
            /* physical (px,py) reads logical (py, PANEL_WIDTH-1-px).  The
             * PXP writes the X byte of a 32-bit output as 0 (silicon,
             * lvgl_pxp_copy_bench transcript; modelled in QEMU), so the
             * reference is masked to RGB and the presented byte 3 is PINNED
             * to 0 -- pxp_rotate_probe's ref_px makes the same decision. */
            const uint32_t want = cv[(size_t)(PANEL_WIDTH - 1 - px) * (size_t)UI_W + py] & 0x00FFFFFFu;
            if (row[px] != want) return false;
        }
    }
    return true;
}

/* BOOT ONLY -- synchronous, and it may wait up to a frame in flip_sync().
 * Never call it from loop(): that is the per-bar stall the incremental pair
 * below replaced. */
ROTWIT_FN static void rot_equality_check_boot(void)
{
    const uint32_t t0 = micros();            /* the flip_sync() wait is part of the cost */
    lvgl_mipi_panel_flip_sync();
    if (rot_eq_rows(0, ROT_EQ_ROWS)) s_rotEqPass++; else s_rotEqFail++;
    s_rotEqUs = micros() - t0;               /* one pass: the whole check */
}

/* Arms a per-bar check.  A check STILL ACTIVE when the next is armed never
 * reached a verdict -- a flip was pending on every pass it was given, or the
 * transport stopped under it -- and it is counted as a FAIL here, never
 * silently dropped: a guard that quietly stops finishing would otherwise read
 * exactly like a guard that keeps passing.  Its largest pass so far becomes
 * us=, so the line's us always belongs to the last verdict counted. */
ROTWIT_FN static void rot_equality_begin(void)
{
    if (s_rotEqActive) {
        s_rotEqFail++;
        s_rotEqUs = s_rotEqMaxUs;
    }
    s_rotEqActive = true;
    s_rotEqOk     = true;
    s_rotEqRow    = 0;
    s_rotEqMaxUs  = 0;
}

/* One loop pass of the armed check: skip if a flip is pending (never wait),
 * else compare the next ROT_EQ_CHUNK sampled rows; at the last row -- or the
 * first mismatch -- count the verdict and disarm. */
ROTWIT_FN static void rot_equality_step(void)
{
    if (!s_rotEqActive) return;
    /* isrs is read last-or-first indifferently: it only ever INCREASES under
     * us, so a stale read can only skip a pass that could have run. */
    if (lvgl_mipi_panel_vsync_isrs() + lvgl_mipi_panel_vsync_timeouts()
            < lvgl_mipi_panel_flips())
        return;                              /* a flip is pending: try next pass */
    const uint32_t t0 = micros();
    lvgl_mipi_panel_flip_sync();             /* nothing pending: returns at once */
    uint32_t to = s_rotEqRow + ROT_EQ_CHUNK;
    if (to > ROT_EQ_ROWS) to = ROT_EQ_ROWS;
    if (!rot_eq_rows(s_rotEqRow, to)) s_rotEqOk = false;
    s_rotEqRow = to;
    const uint32_t us = micros() - t0;
    if (us > s_rotEqMaxUs) s_rotEqMaxUs = us;
    if (!s_rotEqOk || s_rotEqRow >= ROT_EQ_ROWS) {
        if (s_rotEqOk) s_rotEqPass++; else s_rotEqFail++;
        s_rotEqUs     = s_rotEqMaxUs;
        s_rotEqActive = false;
    }
}

ROTWIT_FN static void print_rot_lines(void)
{
    CONSOLE.printf("ACIDBOX_ROT ops=%lu full=%lu px=%lu us=%lu errors=%lu\n",
                   (unsigned long)lvgl_mipi_panel_rot_ops(),
                   (unsigned long)lvgl_mipi_panel_rot_full(),
                   (unsigned long)lvgl_mipi_panel_rot_px(),
                   (unsigned long)lvgl_mipi_panel_rot_us(),
                   (unsigned long)lvgl_mipi_panel_rot_errors());
    /* us= here is the largest SINGLE-PASS stall of the last check to reach a
     * verdict (the boot check: its whole cost; per bar: its costliest chunk),
     * never the check's total -- see the witnesses' header above. */
    CONSOLE.printf("ACIDBOX_ROT_EQ pass=%lu fail=%lu us=%lu\n",
                   (unsigned long)s_rotEqPass, (unsigned long)s_rotEqFail,
                   (unsigned long)s_rotEqUs);
}

/* --- per-step RMS windows, referenced to the SEQUENCER's own position ----- *
 *
 * Float DSP is asserted by measured windows with margin, never by bit-goldens
 * (the acid_bass_test idiom) -- and the window boundaries come from the
 * sequencer's step index rather than from millis(), so the table below is a
 * property of the audio clock and cannot move with host speed.  All state here
 * is touched from loop() only.
 *
 * ★ BAR 1 IS NOT A VALID WINDOW FOR STEP 0 -- assert from bar 2 onwards.
 * The transport never emits tick 0 at phase 0 (transport.cpp records boundaries
 * strictly inside (from, to]), so step 0 first fires at the loop seam and its
 * bar-1 window opens part-way through the note.  Measured under a throwaway
 * play(): step 0 reads 0.1843 in bar 1 and 0.42-0.43 in every bar after it,
 * while the accented steps 0/7/12 sit at ~0.42 against ~0.37-0.39 for the plain
 * gated steps and the four rests (2, 5, 9, 14) read ~0.0002.  Those are the
 * numbers the gate's windows should be built from, and the bar-1 outlier is a
 * property of the transport, not a startup transient that settles. */
static float    stepPeakRms[16];
static int      lastSeenStep = -1;
static uint32_t barsDone = 0;
static void audio_probe_poll(void)
{
    /* -1 until the first step fires.  While the transport is stopped this is
     * the only statement that runs, which is what makes the boot state silent
     * on the wire as well as in the headphones. */
    const int s = seq.currentStep();
    if (s < 0) return;
    if (rms.available()) {
        const float v = rms.read();
        if (v > stepPeakRms[s]) stepPeakRms[s] = v;
#ifdef ACIDBOX_DIAG
        /* Same peak, but NEVER cleared at the bar seam, so an SWD read that
         * lands mid-bar still sees a whole measured table. */
        if (v > g_diag_rms[s]) g_diag_rms[s] = v;
#endif
    }
    /* The per-bar equality check, one chunk per pass while the transport plays
     * (armed at step 8 below).  The active test is inlined HERE so a pass with
     * no check in progress never makes the flash call. */
    if (s_rotEqActive) rot_equality_step();
    if (s != lastSeenStep) {
        /* Mid-bar: arm this bar's equality check, so it has the second half
         * of the bar (~0.94 s at 128 BPM) to finish before the seam below
         * prints its verdict -- the gate's pass >= bars+1 needs every bar's
         * check COMPLETE by that bar's seam line.  An unfinished one is
         * counted a FAIL by the next arm. */
        if (s == 8) rot_equality_begin();
        /* 15 -> 0 is the loop seam.  Anchoring on the seam rather than on
         * "s == 0" means a bar is only reported once the whole 16-step table
         * has been filled, so no line can carry a half-measured window. */
        if (s == 0 && lastSeenStep == 15) {
            barsDone++;
#ifdef ACIDBOX_DIAG
            g_diag_bars = barsDone;
#endif
            CONSOLE.printf("ACIDBOX_BAR=%lu RMS=[", (unsigned long)barsDone);
            /* ★ print(float, digits), NOT printf("%.4f"), AND THAT IS NOT A
             * STYLE CHOICE. Print::printf goes through newlib's vsnprintf, and
             * this tree links the INTEGER-ONLY formatter: in the ELF
             * _svfprintf_r and _svfiprintf_r resolve to the same address and
             * _dtoa_r is absent entirely. A "%.4f" therefore emits a NUL byte
             * rather than digits -- measured here, not assumed, and it is why
             * no other example in this tree formats a float with printf.
             * printFloat() is the core's own converter and needs no libc.
             *
             * ★ Note how nearly this hid: the boot state is STOPPED, so this
             * whole branch is unreachable in the shipped configuration. It only
             * surfaced under a throwaway transport.play(). Checking that the
             * ELF merely CONTAINS a symbol named _svfprintf_r would have
             * "confirmed" the opposite -- the name is an alias. */
            for (int i = 0; i < 16; i++) {
                if (i) CONSOLE.print(',');
                CONSOLE.print(stepPeakRms[i], 4);
            }
            CONSOLE.println("]");
            memset(stepPeakRms, 0, sizeof(stepPeakRms));
            /* Fence health beside every bar line, so the counter is witnessed
             * for the whole gated run (play + taps + drag), not only at boot.
             * The gate rejects ANY of these lines carrying timeouts!=0. */
            CONSOLE.printf("ACIDBOX_VSYNC flips=%lu isrs=%lu timeouts=%lu\n",
                           (unsigned long)lvgl_mipi_panel_flips(),
                           (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                           (unsigned long)lvgl_mipi_panel_vsync_timeouts());
            /* The verdict of the check armed at step 8 -- no compare runs
             * here any more (it was the ~31 ms seam stall). */
            print_rot_lines();
        }
        lastSeenStep = s;
    }
}

#if defined(M2_BT_OUT)
/* One pass of the UI half of loop() -- see the forward declaration in the BT
 * preamble.  Not re-entrant with LVGL by construction: every caller is a
 * top-level blocking wait reached from setup() or loop(), never from inside
 * lv_timer_handler(). */
static void idleUi()
{
    yield();                       /* the yield-attached HciPump: parses HCI events for the waiting caller */
    lvgl_rt1176_loop();            /* render + touch + the ui_poll timer: the panel stays live */
    audio_probe_poll();            /* bar/RMS bookkeeping keeps its cadence */
#if defined(ACIDBOX_LOOPSTAT)
    ls_summary();                  /* loopstat/framestat/touchstat keep printing through the wait */
#endif
}
#endif

/* --- UI ------------------------------------------------------------------- *
 *
 * Layout C (spec 2026-09-14-acid-box-landscape-design.md section 6) on a
 * LOGICAL 1280x720 frame: the transport bar, the 2x8 step lane with the
 * selected-step editor beside it, an EMPTY band reserved for later work, and
 * the eight sound knobs along the bottom edge -- nearest the player once the
 * board lies flat, turned counter-clockwise.  The panel is still the RK055's
 * 720x1280 glass: the port presents this frame rotated 90 degrees clockwise
 * (lvgl_mipi_panel_create_rotated) and the touch binding inverts the same map,
 * so nothing here knows about rotation.
 *
 * Everything is placed from the constants below, and those constants are the
 * GATE's geometry as well as the picture's -- the touch script's raw GT911
 * percentages are derived from them through the CW90 map (run_qemu.sh's
 * GEOMETRY block), so moving a widget moves a tap point.  Regenerate the
 * script if any of these change.
 *
 * The screen still paints an OPAQUE ground, for the reason the stub did: it is
 * what makes every pixel of the frame defined, and therefore makes
 * ACIDBOX_UI_SUM a checksum of the scene rather than of whatever the allocator
 * left behind. */
/* UI_W, UI_H (the logical frame) are declared above the rotation witnesses,
 * which read the canvas at that size before this section begins. */
/* top bar */
static constexpr int TITLE_X = 24,  TITLE_Y = 36;
static constexpr int BAR_Y = 20,    BAR_BTN_H = 48;
static constexpr int TEMPO_DN_X = 560, TEMPO_UP_X = 690, TEMPO_BTN_W = 50;
static constexpr int BPM_X = 624,   BPM_Y = 35;
static constexpr int PLAY_X = 1040, STOP_X = 1156, TRANSPORT_BTN_W = 100;   /* PLAY 1040..1139 x 20..67; the gate's tap lands at (1088,43) */
/* pattern band */
static constexpr int LANE_X0 = 16,  LANE_Y0 = 96, LANE_CELL = 100, LANE_PITCH_X = 108, LANE_PITCH_Y = 112;
/* cell 2 is 232..331 x 96..195 -- the gate's edit target; its tap lands at (281,143) */
static constexpr int PITCH_X = 912, PITCH_Y = 96, PITCH_SIZE = 150;
static constexpr int NOTE_X = 1080, NOTE_Y = 110;
static constexpr int ACC_X = 1080,  SLD_X = 1176, TOG_Y = 148, TOG_W = 88, TOG_H = 56;
static constexpr int WAVE_X = 1080, WAVE_Y = 222, WAVE_W = 184, WAVE_H = 56;
/* y 308..520 is RESERVED: empty on purpose (spec section 6), not centred away */
/* sound knobs */
static constexpr int KNOB_X0 = 16,  KNOB_Y0 = 520, KNOB_SIZE = 150, KNOB_PITCH = 158;   /* CUTOFF: 16..165 x 520..669; the drag lands at x 89, y 583..647 */
static constexpr int KNOB_LABEL_DX = 50, KNOB_LABEL_DY = 152;
/* The present strategy, measured on silicon by display/pxp_rotate_probe
 * (spec section 4.1): a full-frame rotate costs 13.06 ms and a 160x160 rect
 * 366 us with ~5 us per op, so per-rect ops win until the damage union is
 * 915456 logical px -- 99.33 % of the frame.  At or above it, ONE full-frame op. */
static constexpr uint32_t ACIDBOX_ROT_FULL_THRESHOLD_PX = 915456u;
static lv_obj_t *stepCell[16];
static lv_obj_t *playBtnLabel, *bpmLabel, *noteLabel, *accBtn, *sldBtn, *waveBtnLabel;
static lv_obj_t *pitchKnob;
static int selectedStep = 0;

/* knob -> parameter maps (spec §4).  Every knob is created with an EXPLICIT
 * -140..+140 range (the rotary widget's DC default is ±150), so the whole
 * sweep is 280 degrees and t lands in [0,1]; the input layer clamps the drag
 * to that range, so knob01() needs no clamp of its own. */
static inline float knob01(lv_obj_t *k)
{ return (synthui_rotary_knob_get_angle(k) + 140.0f) / 280.0f; }
static inline float expmap(float t, float lo, float hi)
{ return lo * powf(hi / lo, t); }

/* Pitch map: 25 semitones C1(24)..C3(48), one detent each, so detent_step is
 * 280/24 -- 24 intervals between 25 stops, not 25.  The knob snaps onto the
 * lattice anchored at min_deg (synthui_knob_math.h), which is exactly the
 * lattice these two functions assume. */
static inline uint8_t angleToNote(float deg)
{ return (uint8_t)(24 + (int)roundf((deg + 140.0f) / (280.0f / 24.0f))); }
static inline float noteToAngle(uint8_t note)
{
    /* Clamped because seq.step() accepts the full 0..127 MIDI range while this
     * knob only spans C1..C3: an out-of-range note would otherwise park the
     * pointer outside the drawn end stops, drawing a position the user cannot
     * reach and this map cannot round-trip. */
    if (note < 24) note = 24;
    if (note > 48) note = 48;
    return -140.0f + (float)(note - 24) * (280.0f / 24.0f);
}

/* One callback per sound knob.  CUTOFF, DECAY and SLIDE T are exponential
 * because they are frequency and time; the five 0..1 amounts are linear. */
static void cbRes(lv_event_t *e){ acid.resonance (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbEnv(lv_event_t *e){ acid.envMod    (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbAcc(lv_event_t *e){ acid.accent    (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbDst(lv_event_t *e){ acid.distortion(knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbSub(lv_event_t *e){ acid.subLevel  (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbDec(lv_event_t *e){ acid.decay    (expmap(knob01((lv_obj_t *)lv_event_get_target(e)), 0.03f, 2.0f)); }
static void cbSld(lv_event_t *e){ acid.slideTime(expmap(knob01((lv_obj_t *)lv_event_get_target(e)), 0.01f, 0.3f)); }

/* Cutoff also prints, because the gate's drag assertion needs a value it can
 * order.  ★ print(float, digits), NOT printf("%.1f") -- see the identical note
 * over the RMS table above: this tree links newlib's INTEGER-ONLY formatter and
 * a %f conversion emits a NUL byte instead of digits, which would truncate the
 * line and leave the gate parsing "CUTOFF=" with nothing after it. */
static void cbCut(lv_event_t *e)
{
    const float hz = expmap(knob01((lv_obj_t *)lv_event_get_target(e)), 20.0f, 12000.0f);
    acid.cutoff(hz);
    CONSOLE.print("CUTOFF=");
    CONSOLE.println(hz, 1);
}

static const char *noteName(uint8_t n)
{
    static const char *N[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    /* %s and %d only: integer conversions are all the linked formatter has. */
    snprintf(buf, sizeof buf, "%s%d", N[n % 12], (int)(n / 12) - 1);
    return buf;
}

/* Write the selected step's FULL state back as ONE seq.step() call and refresh
 * its cell and the editor readouts.  The single write is the atomic transaction
 * of spec §3.3: seq.step() takes its own __disable_irq() guard internally, so a
 * four-field store is indivisible against the audio ISR, while four separate
 * read-modify-writes would let the sequencer observe a half-edited step. */
static void commit_selected(uint8_t note, bool gate, bool accent, bool slide)
{
    seq.step(selectedStep, note, gate, accent, slide);
    synthui_step_set(stepCell[selectedStep], gate, accent, slide);
    lv_label_set_text(noteLabel, gate ? noteName(note) : "--");
    lv_obj_set_style_bg_color(accBtn, accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    CONSOLE.printf("STEP[%d]=note%u gate%d acc%d sld%d\n",
                   selectedStep, (unsigned)note,
                   gate ? 1 : 0, accent ? 1 : 0, slide ? 1 : 0);
}

/* Pure VIEW change: moves the editor onto step i without touching the
 * pattern, so it emits no STEP token.  A rest has note 0 in the preset, which
 * is not a pitch the knob can show -- park it on A1 so the first pitch edit
 * after un-resting starts somewhere musical. */
static void select_step(int i)
{
    synthui_step_set_selected(stepCell[selectedStep], false);
    selectedStep = i;
    synthui_step_set_selected(stepCell[i], true);
    const AcidStep st = seq.step(i);
    synthui_rotary_knob_set_angle(pitchKnob, noteToAngle(st.note ? st.note : 33));
    lv_label_set_text(noteLabel, st.gate ? noteName(st.note) : "--");
    lv_obj_set_style_bg_color(accBtn, st.accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, st.slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
}

/* A tap does BOTH: it selects the cell for editing and toggles its gate.  One
 * gesture, because on a 16-cell lane a select-then-toggle pair would double
 * every pattern edit. */
static void cbStepTap(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    select_step(i);
    const AcidStep st = seq.step(i);
    commit_selected(st.note ? st.note : 33, !st.gate, st.accent, st.slide);
}
static void cbPitch(lv_event_t *e)
{
    const uint8_t note = angleToNote(synthui_rotary_knob_get_angle((lv_obj_t *)lv_event_get_target(e)));
    const AcidStep st = seq.step(selectedStep);
    commit_selected(note, st.gate, st.accent, st.slide);
}
static void cbAccBtn(lv_event_t *e)
{ (void)e; const AcidStep st = seq.step(selectedStep); commit_selected(st.note, st.gate, !st.accent, st.slide); }
static void cbSldBtn(lv_event_t *e)
{ (void)e; const AcidStep st = seq.step(selectedStep); commit_selected(st.note, st.gate, st.accent, !st.slide); }

static void cbPlay(lv_event_t *e)
{
    (void)e;
    if (transport.playing()) transport.pause(); else transport.play();
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
}
static void cbStop(lv_event_t *e)
{ (void)e; transport.stop(); CONSOLE.println("PLAYING=0"); }
static void cbTempoUp(lv_event_t *e)
{ (void)e; transport.tempo(transport.tempo() + 1.0f); }
static void cbTempoDn(lv_event_t *e)
{ (void)e; transport.tempo(transport.tempo() - 1.0f); }
static void cbWave(lv_event_t *e)
{
    (void)e;
    /* Mirrors default_patch()'s WAVEFORM_SAWTOOTH; the voice keeps the truth,
     * this only remembers which way to flip next. */
    static bool square = false;
    square = !square;
    acid.waveform(square ? WAVEFORM_SQUARE : WAVEFORM_SAWTOOTH);
    lv_label_set_text(waveBtnLabel, square ? "SQR" : "SAW");
}

/* 33 ms poller: cursor ring, play label, bpm readout (spec §3.3).
 *
 * ★ EVERY WRITE IS GUARDED BY A CHANGE TEST, AND NOT AS AN OPTIMISATION.
 * lv_label_set_text() reallocates and invalidates unconditionally, so an
 * unguarded poller would dirty two labels 30 times a second forever -- a
 * permanent full-rate repaint on a panel whose software render costs tens of
 * milliseconds.  The `shown*` caches start at values no reading can equal, so
 * the first call always paints. */
static int shownCursor  = -1;
static int shownPlaying = -1;
static int shownBpm10   = -1;
static void ui_poll(lv_timer_t *t)
{
    (void)t;
    /* Stopped means NO cursor, not "the cursor where it stopped": currentStep()
     * keeps its last index after pause(), and a ring left burning on a paused
     * box reads as a playhead that has stalled. */
    const bool play = transport.playing();
    const int  s    = play ? seq.currentStep() : -1;
    if (s != shownCursor) {
        if (shownCursor >= 0) synthui_step_set_cursor(stepCell[shownCursor], false);
        if (s >= 0)           synthui_step_set_cursor(stepCell[s], true);
        shownCursor = s;
    }
    if ((int)play != shownPlaying) {
        shownPlaying = (int)play;
        lv_label_set_text(playBtnLabel, play ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    /* One decimal, assembled from INTEGERS: see cbCut's note on %f.  tempo() is
     * clamped to 20..999 by the transport, so both halves stay non-negative. */
    const int bpm10 = (int)lroundf(transport.tempo() * 10.0f);
    if (bpm10 != shownBpm10) {
        shownBpm10 = bpm10;
        char b[16];
        snprintf(b, sizeof b, "%d.%d", bpm10 / 10, bpm10 % 10);
        lv_label_set_text(bpmLabel, b);
    }
}

/* UIBUILD_FN puts the ONE-SHOT scene construction in FLASH (.progmem, XIP):
 * build_ui() runs once from setup(), and mkbtn()/mkknob() only from build_ui().
 * noinline is load-bearing -- without it build_ui() inlines into setup(),
 * which the default build leaves in ITCM.  Nothing touch, the poller or the
 * audio path drives at run time carries it (the callbacks, ui_poll(),
 * select_step(), commit_selected() stay in ITCM); the default build's ITCM
 * headroom had fallen to 836 B with the landscape rework. */
#define UIBUILD_FN __attribute__((section(".progmem.acid_uibuild"), noinline))
UIBUILD_FN static lv_obj_t *mkbtn(lv_obj_t *par, const char *txt, lv_event_cb_t cb,
                                  lv_obj_t **labelOut)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (labelOut) *labelOut = l;
    return b;
}
UIBUILD_FN static lv_obj_t *mkknob(lv_obj_t *scr, int i, const char *name,
                                   float boot01, lv_event_cb_t cb)
{
    lv_obj_t *k = synthui_rotary_knob_create(scr);
    lv_obj_set_size(k, KNOB_SIZE, KNOB_SIZE);
    lv_obj_set_pos(k, KNOB_X0 + i * KNOB_PITCH, KNOB_Y0);
    synthui_rotary_knob_set_mode(k, SYNTHUI_ROTARY_MODE_BOUNDED);
    /* The DC default range is ±150; every angle<->param map in this file
     * hardcodes ±140, so the range is stated here instead of inherited. */
    synthui_rotary_knob_set_range(k, -140.0f, 140.0f);
    synthui_rotary_knob_set_angle(k, boot01 * 280.0f - 140.0f);
    lv_obj_add_event_cb(k, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9aa0b8), LV_PART_MAIN);
    lv_obj_set_pos(l, KNOB_X0 + i * KNOB_PITCH + KNOB_LABEL_DX, KNOB_Y0 + KNOB_LABEL_DY);
    return k;
}

UIBUILD_FN static lv_obj_t *build_ui(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    /* ★ CROSS-AXIS SCROLL CHAINING, decided here (plan's Task-5 note).  The
     * knob clears SCROLL_CHAIN_VER permanently but leaves SCROLL_CHAIN_HOR on,
     * so a vertical drag that wanders far enough sideways can flip
     * lv_indev_find_scroll_obj() to the horizontal axis and let a scrollable
     * ancestor steal the press mid-turn (PRESS_LOST).  Nothing here is
     * horizontally scrollable, but "nothing scrolls today" is a property of the
     * layout and would evaporate the first time a widget is placed past the
     * right edge.  Clearing the flag on the screen makes it structural.  If a
     * knob is ever put inside a genuinely horizontally scrolling panel, port
     * lv_slider's pattern -- remove the CROSS-axis flag only once the drag is
     * established, restore it on RELEASED/PRESS_LOST -- rather than killing a
     * legitimate swipe gesture outright. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* transport bar */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ACID BOX");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(title, TITLE_X, TITLE_Y);
#if defined(ACIDBOX_LOOPSTAT)
    ls_attach_title(title);        /* tap = synthetic knob wiggle on/off (bench) */
#endif
    lv_obj_t *dn = mkbtn(scr, "-", cbTempoDn, NULL);
    lv_obj_set_pos(dn, TEMPO_DN_X, BAR_Y);
    lv_obj_set_size(dn, TEMPO_BTN_W, BAR_BTN_H);
    bpmLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(bpmLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(bpmLabel, BPM_X, BPM_Y);
    lv_obj_t *up = mkbtn(scr, "+", cbTempoUp, NULL);
    lv_obj_set_pos(up, TEMPO_UP_X, BAR_Y);
    lv_obj_set_size(up, TEMPO_BTN_W, BAR_BTN_H);
    lv_obj_t *play = mkbtn(scr, LV_SYMBOL_PLAY, cbPlay, &playBtnLabel);
    lv_obj_set_pos(play, PLAY_X, BAR_Y);
    lv_obj_set_size(play, TRANSPORT_BTN_W, BAR_BTN_H);
    lv_obj_t *stop = mkbtn(scr, LV_SYMBOL_STOP, cbStop, NULL);
    lv_obj_set_pos(stop, STOP_X, BAR_Y);
    lv_obj_set_size(stop, TRANSPORT_BTN_W, BAR_BTN_H);

    /* step lane: 2x8 of LANE_CELL px cells at LANE_PITCH_X/Y */
    for (int i = 0; i < 16; i++) {
        lv_obj_t *c = synthui_step_create(scr);
        lv_obj_set_size(c, LANE_CELL, LANE_CELL);
        lv_obj_set_pos(c, LANE_X0 + (i % 8) * LANE_PITCH_X, LANE_Y0 + (i / 8) * LANE_PITCH_Y);
        synthui_step_set(c, kPreset[i].gate, kPreset[i].accent, kPreset[i].slide);
        lv_obj_add_event_cb(c, cbStepTap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        stepCell[i] = c;
    }

    /* editor: pitch detent knob + note name + ACC/SLD toggles + SAW/SQR */
    pitchKnob = synthui_rotary_knob_create(scr);
    lv_obj_set_size(pitchKnob, PITCH_SIZE, PITCH_SIZE);
    lv_obj_set_pos(pitchKnob, PITCH_X, PITCH_Y);
    /* detents are input behavior on the rotary widget (no visual mode):
     * bounded well + 24 semitone stops on the ±140 lattice the pitch maps
     * above assume. */
    synthui_rotary_knob_set_mode(pitchKnob, SYNTHUI_ROTARY_MODE_BOUNDED);
    synthui_rotary_knob_set_range(pitchKnob, -140.0f, 140.0f);
    synthui_rotary_knob_set_detent_step(pitchKnob, 280.0f / 24.0f);
    lv_obj_add_event_cb(pitchKnob, cbPitch, LV_EVENT_VALUE_CHANGED, NULL);
    noteLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(noteLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(noteLabel, NOTE_X, NOTE_Y);
    accBtn = mkbtn(scr, "ACC", cbAccBtn, NULL);
    lv_obj_set_pos(accBtn, ACC_X, TOG_Y);
    lv_obj_set_size(accBtn, TOG_W, TOG_H);
    sldBtn = mkbtn(scr, "SLD", cbSldBtn, NULL);
    lv_obj_set_pos(sldBtn, SLD_X, TOG_Y);
    lv_obj_set_size(sldBtn, TOG_W, TOG_H);
    lv_obj_t *wave = mkbtn(scr, "SAW", cbWave, &waveBtnLabel);
    lv_obj_set_pos(wave, WAVE_X, WAVE_Y);
    lv_obj_set_size(wave, WAVE_W, WAVE_H);

    /* Sound knobs, one row along the bottom edge.  Boot angles are the INVERSE
     * of each map applied to default_patch()'s values, so the first frame
     * shows the patch the engine is actually holding -- a knob drawn at a
     * position its parameter is not at would make the very first drag jump. */
    LS_KNOB(mkknob(scr, 0, "CUTOFF",  logf(800.0f / 20.0f) / logf(12000.0f / 20.0f), cbCut));
    LS_KNOB(mkknob(scr, 1, "RESO",    0.55f, cbRes));
    LS_KNOB(mkknob(scr, 2, "ENV MOD", 0.60f, cbEnv));
    LS_KNOB(mkknob(scr, 3, "DECAY",   logf(0.28f / 0.03f) / logf(2.0f / 0.03f), cbDec));
    LS_KNOB(mkknob(scr, 4, "ACCENT",  0.70f, cbAcc));
    LS_KNOB(mkknob(scr, 5, "DIST",    0.15f, cbDst));
    LS_KNOB(mkknob(scr, 6, "SUB",     0.20f, cbSub));
    LS_KNOB(mkknob(scr, 7, "SLIDE T", logf(0.06f / 0.01f) / logf(0.3f / 0.01f), cbSld));
    select_step(0);

    /* ★ RUN THE POLLER ONCE BEFORE ARMING THE TIMER, or the boot golden is a
     * race.  lv_timer_create() schedules the first callback one FULL period
     * away, while the caller renders as soon as the first lv_timer_handler()
     * returns; whether the 33 ms tick beat the first flush would then decide
     * whether ACIDBOX_UI_SUM covers a bpm readout saying "128.0" or the
     * lv_label default "Text".  Priming it here makes the first frame a
     * function of engine state and nothing else. */
    ui_poll(NULL);
    lv_timer_create(ui_poll, 33, NULL);
    return scr;
}

/* --- touch ---------------------------------------------------------------- *
 * D9 = GPIO_AD_01 = touch reset, D6 = GPIO_AD_00 = touch interrupt, both owned
 * by GT911::begin(); the INT line is never attached to, exactly as
 * lvgl_rk055_touch_test leaves it. */
static constexpr uint8_t TOUCH_RST_PIN = 9;
static constexpr uint8_t TOUCH_INT_PIN = 6;
static GT911 touch(Wire2, TOUCH_RST_PIN, TOUCH_INT_PIN);

void setup()
{
    CONSOLE.begin(115200);
    while (!CONSOLE && millis() < 2000) {}

    // [APP:<name>] [VER:<gate_version>] [BUILD:<timestamp>]
    CONSOLE.println("=== BOOT ===");
    CONSOLE.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n",
                   "acid_box", ACIDBOX_VERSION, __DATE__, __TIME__);
    CONSOLE.println("ACIDBOX_BEGIN");
    diag_mark();               /* after CONSOLE.begin + boot banner */

    AudioMemory(24);
    diag_mark();               /* after AudioMemory(24) */
    const bool codec = wm.enable();
    wm.volume(0.6f);
    CONSOLE.println(codec ? "CODEC_OK" : "CODEC_FAIL");
    diag_mark();               /* after wm.enable() + volume */

    const bool panel = Display.begin();
    CONSOLE.println(panel ? "PANEL_OK" : "PANEL_FAIL");
    diag_mark();               /* after Display.begin() */
    if (!panel) {
        /* No lv_init() happened, so loop()'s lv_timer_handler() returns
         * immediately -- the same contract the sibling display examples use.
         * The transport is also never configured, so audio_probe_poll() stays
         * on its -1 early return and the run is silent in both senses. */
        CONSOLE.println("ACIDBOX_DONE");
        return;
    }

    lvgl_rt1176_begin();
    diag_mark();               /* after lvgl_rt1176_begin() */

    /* GC355 probe BEFORE any compositor commitment (synthui_knob_test's
     * wiring): vg_lite_init() SPINS on absent hardware, so the chip-ID read
     * is what makes QEMU a clean negative. */
    memset(vglite_pool, 0, VGLITE_POOL_BYTES);
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool,
                     VGLITE_POOL_BYTES);
    const bool vg_up = (vg_lite_hal_probe_chip_id() != 0u) &&
                       (vg_lite_init(TESS_W, TESS_H) == VG_LITE_SUCCESS);

    /* Rotated create + the pre-flip compose hook: when the GC355 is up the
     * knob compositor draws into the off-screen CANVAS (never scanned out)
     * right before the port rotates it into the off-screen scanout buffer and
     * requests the flip -- still tear-free BY CONSTRUCTION, as the v4/v5
     * double-buffered create this replaces was (the scanout-flash finding,
     * gpu-well spec section 5c; the single-buffer begin() can flash a
     * damage-box-sized square when the scanline crosses mid-composite).  In
     * QEMU vg_up is false and every knob stays fully software.
     *
     * Landscape: LVGL renders a 1280x720 canvas and the port presents it
     * rotated 90 degrees clockwise through the PXP into the off-screen scanout
     * buffer, then flips -- the same fenced, tear-free pipeline as create_db,
     * with the rotation confined to the present (spec section 5.B). */
    lvgl_mipi_panel_set_rot_threshold(ACIDBOX_ROT_FULL_THRESHOLD_PX);
    lv_display_t *disp = lvgl_mipi_panel_create_rotated(Display, LVGL_PANEL_PRESENT_CW90);
    diag_mark();               /* after lvgl_mipi_panel_create_rotated() */
#if defined(ACIDBOX_LOOPSTAT)
    ls_attach_display(disp);
#endif
    /* The compositor draws into the CANVAS (the pre-flip hook hands it the
     * canvas), so it is told the canvas's geometry, not the panel's. */
    if (vg_up && synthui_rotary_gpu_begin_deferred(
                     UI_W, UI_H, UI_W * PANEL_BYTES_PER_PIXEL)) {
        s_gpu = true;
        /* the app owns the wiring: compositor <- pre-flip hook -> panel */
        lvgl_mipi_panel_set_preflip_cb(synthui_rotary_gpu_compose_into);
    }
    CONSOLE.printf("ACIDBOX_ENGINE=%s\n", s_gpu ? "gpu" : "sw");

    load_preset();
    default_patch();
    transport.tempo(128.0f);
    transport.loop(0.0f, 1.0f);        /* one bar == the 384-tick pattern */
    transport.looping(true);
    diag_mark();               /* after preset + patch + transport cfg */
    pump.priority(224);                /* BEFORE begin(): see pump_isr's note.
                                        * priority() applied afterwards would
                                        * leave a window running at 128. */
    pump.begin(pump_isr, 1000);        /* MICROseconds, Teensy convention: 1 kHz */
    diag_mark();               /* after pump.begin() */

    /* The scene is the FIRST refresh, so the sum below covers a whole-screen
     * paint rather than whatever a partial repaint touched. */
    lv_screen_load(build_ui());
    diag_mark();               /* after build_ui() + screen load */
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    /* db/rotated mode: checksum the PRESENTED buffer, never
     * Display.framebuffer() -- flip_sync() first so the pending flip has
     * retired and scanned_fb() names the front buffer (synthui_knob_test's sum
     * contract).  The checksum reads lvgl_mipi_panel_scanned_fb() over
     * PANEL_FB_BYTES, which now covers the PRESENTED, rotated portrait frame. */
    lvgl_mipi_panel_flip_sync();
    lvgl_sum_reset();
    diag_mark();               /* after the frame_done wait loop */
    lvgl_sum_feed(lvgl_mipi_panel_scanned_fb(), PANEL_FB_BYTES);
    diag_mark();               /* after the 3.6 MB checksum   == :591 pre-diag */
    CONSOLE.printf("ACIDBOX_UI_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
    rot_equality_check_boot(); /* the boot present: the forced full-frame path, synchronous */
    print_rot_lines();
    diag_mark();               /* after the two console printfs + the boot rotation witnesses */

    /* Touch bring-up AFTER the golden, which is what keeps the golden a
     * statement about the scene alone: no indev exists yet, so no contact can
     * have moved a widget before the checksum was taken.  Wire2 is already open
     * (wm.enable() begins it -- it is the codec's bus too); re-begin()ing it is
     * lvgl_rk055_touch_test's proven sequence and states the ownership rather
     * than inheriting it by luck. */
    Wire2.begin();
    Wire2.setClock(400000);
    diag_mark();               /* after Wire2.begin()/setClock() == :602 pre-diag */
    const bool touchOk = touch.begin();
    diag_mark();               /* after touch.begin()          == :603 pre-diag */
    if (touchOk) {
        CONSOLE.println("I2C_OK");
        /* From here LVGL polls the part every 10 ms; in QEMU the model replays
         * a script, on the bench a finger drives it. */
        lv_indev_t *indev = lvgl_gt911_indev_create_rotated(disp, touch, LVGL_PANEL_PRESENT_CW90);
        (void)indev;
#if defined(ACIDBOX_LOOPSTAT)
        ls_attach_touch(indev);
#endif
    } else {
        /* Not fatal: the box keeps playing and drawing, it just cannot be
         * touched.  The gate's verdict is the ABSENCE of I2C_OK, so a silent
         * fallthrough is still caught -- but the diagnostics say which of the
         * three failure modes it was without a second run. */
        CONSOLE.printf("TOUCH_FAIL err=%s i2c=%u id=0x%08lX\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned)touch.lastI2cStatus(),
                       (unsigned long)touch.lastDeviceId());
    }
    /* gpu lines NEVER appear in a sw run -- the gate tripwires on them. */
    if (s_gpu)
        CONSOLE.printf("ACIDBOX_GPU_ERR=%lu\n",
                       (unsigned long)synthui_rotary_gpu_errors());
    /* vsync-fence health (the rotated present keeps the db fence, both
     * engines): a timeout means the pipeline silently degraded to unfenced
     * presents (tearing possible) and must fail by name, not by eye -- gated
     * in QEMU. */
    CONSOLE.printf("ACIDBOX_VSYNC flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    diag_mark();               /* setup() COMPLETE */
    CONSOLE.println("ACIDBOX_DONE");

#if defined(M2_BT_OUT)
    // Bring up Bluetooth AFTER the panel/codec/touch are live: the local WM8962
    // audio already plays and the SynthUI is on screen during the ~30 s connect.
    hciIo.begin(115200);
    m2ReleaseWifiReset();
#if defined(M2_BT_WAKE_PULSE)
    m2WakeFromBootSleep();
#endif
#if defined(M2_BT_RTS_FLOW)
    Serial2.attachRts((uint8_t)M2_BT_RTS_WATER);
#endif
    btFirmwareDownload();
    hci.begin();
    btPump.attach(hci);
    Hci::Reply r;
    for (uint8_t a = 1; a <= 10; a++) { s_hciSt = hci.run(OP_RESET, nullptr, 0, &r, 500, idleUi); if (s_hciSt == Hci::OK) break; }
    if (s_hciSt == Hci::OK) {
        CONSOLE.println("bt_hci_reset=ok");
        probeIdentity();
#if defined(M2_BT_FAST_BAUD)
        probeFastBaud();
#endif
    } else {
        CONSOLE.println("bt_hci_reset=fail");
    }
    hci.onEvent(onEvt, nullptr);
    hci.onAcl(onAclThunk, nullptr);
    src.setLog(btLog, nullptr); src.setPin("1234");
    // NEW-34: the bond store.  load() once, after Hci::begin(); save() after EVERY connect() return
    // (in loop(), before btout.begin(): no BT media yet; the local audio path IS live).  M2_BT_FORGET_BONDS wipes it instead --
    // the control arm that proves a headset out of pairing mode refuses us WITHOUT a bond.
#if defined(M2_BT_FORGET_BONDS)
    CONSOLE.print("bonds_forgotten="); CONSOLE.println(BondStoreEeprom::wipe(bonds) ? 1 : 0);
#else
    BondStoreEeprom::load(bonds);
#endif
    src.setBonds(&bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
#if defined(M2_BT_LEGACY_PIN)
    src.setLegacyPin(true);
#endif
#if defined(M2_BT_SUPERVISION_MS)
    src.link().setSupervisionSlots((uint16_t)((uint32_t)M2_BT_SUPERVISION_MS * 1000u / 625u));  // ms -> 0.625 ms slots
#endif
    // NEW-34 piece 2: register the session callbacks and start the boot walk (bonded candidates,
    // most-recent-first, then inquiry).  Guarded on s_hciSt like bt_tone_test's setup() (Task 8): with
    // no card, session.begin() is never called, so session.tick()/src.service() in loop() stay vacuous
    // (IDLE state, zeroed Stats) rather than paging into a dead transport forever.
    session.onStream(onStreamCb, nullptr);
    session.onAttempt(onAttemptCb, nullptr);
#if defined(M2_BT_RETRY_MS)
    session.setRetryMs(M2_BT_RETRY_MS);
#endif
    if (s_hciSt == Hci::OK) {
#if defined(M2_BT_TARGET_NAME)
        session.begin(&bonds, M2_BT_TARGET_NAME, s_aclNum, millis());
#else
        session.begin(&bonds, nullptr, s_aclNum, millis());
#endif
    }
#endif
}

void loop()
{
#if defined(ACIDBOX_LOOPSTAT)
    const uint32_t ls_iter0 = micros();
    uint32_t ls_t = ls_iter0;
#endif
#if defined(M2_BT_OUT)
    yield();                                   // drives the yield-attached HciPump (parses NCP/credits)
    LS_LAP(LS_YIELD);
    src.service();                             // SdpServer + L2cap::service() (the ACL UART write) + Avdtp
    src.l2().tickClock(millis());               // NEW-34 piece 4: ms reference for the credit-starve fingerprint
    LS_LAP(LS_SVC);
    if (s_btBegun) btout.poll();               // SBC encode of the buffered PCM + drain into L2cap's queue
    LS_LAP(LS_POLL);
    // NEW-34 piece 2: session.tick() replaces the old 5 s retry block -- it drives A2dpSource's attempt
    // state machine (boot walk / lost-peer retry-forever / page-scan-when-idle / retry-cancel-on-
    // incoming) and fires onStreamCb/onAttemptCb (above) at a link's start/end, including
    // BondStoreEeprom::save() on every attempt end, same ordering guarantee as before (save happens
    // before btout.begin(), which onStreamCb also does).
    session.tick(millis());
    {
        static uint32_t last = 0;
        if (millis() - last >= 1000) { last = millis(); acidBtReport(); }   // report block is flash-resident (see acidBtReport)
    }
    LS_LAP(LS_PRINT);                          // session.tick() (the attempt walk) and bt_hb land here
#endif
#if defined(ACIDBOX_LOOPSTAT)
    ls_summary();
    LS_LAP(LS_PRINT);
#endif
#ifdef ACIDBOX_DIAG
    /* Synthetic play, diagnostic build only.  The shipped contract is BOOT
     * SILENT and this must not be allowed to soften that claim -- so it lives
     * behind the definition build/ never sets, and it starts the transport
     * FOUR SECONDS IN, purely so the audio path can be MEASURED over SWD
     * (g_diag_rms) while the VCOM carries nothing. */
    if (!g_diag_played && millis() >= 4000) {
        g_diag_played = millis();
        transport.play();
    }
#endif
    lvgl_rt1176_loop();
    LS_LAP(LS_LVGL);
    audio_probe_poll();
    LS_LAP(LS_PROBE);
#if defined(ACIDBOX_LOOPSTAT)
    {
        const uint32_t it = micros() - ls_iter0;
        if (it > ls_maxUs) ls_maxUs = it;
        ls_loops++;
    }
#endif
}

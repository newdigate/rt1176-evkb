// m2_hci_probe -- BT-1 of the M.2 Bluetooth programme: does the IW416 answer
// HCI, and what does it say it is?
//
// Sequence: board preamble -> SDIO enumerate -> combo blob download (if
// supplied) -> BAUD SWEEP (3M/921600/460800/115200 -- u-blox attach the
// controller at 3 Mbaud, not at the ROM's 115200) -> HCI over Serial2 at
// whichever rate answered:
//   B1  Reset, Read_Local_Version_Information, Read_BD_ADDR, Read_Buffer_Size
//   B2  Inquiry (GIAC, 10.24 s) then Remote_Name_Request per result
// then a 1 Hz heartbeat carrying the transport's counters.
//
// Every value printed is a value RECEIVED; this firmware contains none of the
// numbers its gates assert (the fake controller's manufacturer 0x1234, the
// card's real one).  Every failure is printed BY NAME with the counters, so a
// transcript reads as an accounting rather than a hope.
//
// Spec: docs/superpowers/specs/2026-08-23-m2-bluetooth-a2dp-programme-design.md §4
#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

// --- BOARD AXIS ------------------------------------------------------------
// ★ The M.2 Bluetooth UART is `Serial2` on BOTH boards, and that is a genuine
// alignment rather than a coincidence worth hiding:
//   rt1176  Serial2 = LPUART2 = GPIO_AD_26/27  -> J54 pins 22/32 (R1901 HAND
//           BRIDGED 2026-08-18; DNP from the factory)
//   rt1062  Serial2 = LPUART3 = GPIO_AD_B1_06/07 (core pins 17/16) -> the M.2
//           BT UART through the FITTED 47R R189/R200.  **No rework needed** --
//           the MIMXRT1060-EVKB ships this path populated, which the 1170 does
//           not.  Verified from the RevB1 netlist, not assumed.
// So every line of transport, loader and HCI code below is board-independent.
// What differs is the console, the module power pins, and whether SDIO exists.
#if defined(ARDUINO_MIMXRT1060_EVKB)
  #define CONSOLE Serial6            // LPUART1 -> the DAPLink VCOM
  #define M2_HAS_SDIO 0
#else
  #define CONSOLE Serial1            // LPUART1 -> the MCU-Link VCOM
  #define M2_HAS_SDIO 1
#endif

#if M2_HAS_SDIO
#include <SdioHost.h>
#include <SdioFunc.h>
#include <Iw416.h>
#endif
#include <Hci.h>
#include <HciEvents.h>
#include <HciTransport.h>
#include <HciPump.h>
#include <BtFwLoader.h>
#if defined(M2_BT_CONNECT)
#include <L2cap.h>
#include <BtLink.h>
#include <SdpServer.h>
#include <Sdp.h>
#include <Avdtp.h>
#endif

// --- the Bluetooth side: identical on both boards ---------------------------
static HciTransport hciIo(Serial2);
static Hci hci(hciIo);
static HciPump pump;
static BtFwLoader btLoader(hciIo);
static BtFwLoader::Error s_btFwSt = BtFwLoader::NO_IMAGE;
static Hci::Error s_hciSt = Hci::TIMEOUT;     // outcome of the Reset step
static bool       s_card  = false;            // Wi-Fi firmware confirmed running

#if defined(HAVE_IW416_BT_FW)
extern const uint8_t  iw416_bt_fw[];
extern const uint32_t iw416_bt_fw_len;
#endif

// --- the Wi-Fi side: rt1176 only --------------------------------------------
// ★ M2Radio's SdioHost targets the RT1176's USDHC and is NOT ported to the
// RT1062, so the Wi-Fi half is compiled out on that board.  The probe SAYS SO
// at runtime rather than simply omitting the lines: a capability that was never
// built must never read as one that failed.
#if M2_HAS_SDIO
static SdioHost sdio;
static SdioFunc func(sdio);
static Iw416 iw416(sdio, func);

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static SdioHost::Status s_sdioSt = SdioHost::CMD5_NO_RESPONSE;
static SdioHost::Status s_iwSt   = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_fwSt   = SdioHost::CMD_TIMEOUT;

// Same spelling as every other m2_* example.
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
#endif


// --- board preamble -- copied from m2_uap_probe (and WiFi.cpp); keep in step --
// Release SDIO_RST (GPIO_AD_16 = GPIO9.15) then WL_RST/PDn (GPIO_AD_31 =
// GPIO9.30, reaching PDn via the hand-bridged R404), with the 1 s ROM-boot
// wait PDn requires.  Without it the card stays in power-down.
#if defined(ARDUINO_MIMXRT1060_EVKB)
// MIMXRT1060-EVKB.  Same two-signal sequence, different pads -- taken from
// NXP's own BOARD_WIFI_BT_Enable() for this board (SDIO_RST high, 100 ms,
// WL_RST high, 100 ms) and cross-checked against the RevB1 netlist:
//   SDIO_RST = GPIO_AD_B1_08 = GPIO1_IO24
//   WL_RST   = GPIO_AD_B1_03 = GPIO1_IO19   (this is PDn to the module)
// ALT5 is the GPIO1 alternate on the AD_B1 pads.
#define M2_SDIO_RST_MUX IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_08
#define M2_WL_RST_MUX   IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_03
#define M2_SDIO_RST_BIT 24
#define M2_WL_RST_BIT   19
// ★ GPIO6, NOT GPIO1.  The teensy4 core sets IOMUXC_GPR_GPR26..29 = 0xFFFFFFFF
// in startup.c, which hands every pad to the FAST GPIO aliases (GPIO6-GPIO9).
// The IOMUX ALT still selects the "GPIO1" function; GPR26 then decides that
// GPIO6 owns the pad.  Writing GPIO1 registers does nothing at all -- and
// reading GPIO1_PSR returns a value unrelated to the pin, which is exactly how
// this was got wrong first time (a bogus "PIN STUCK" verdict that cost two
// resistors off the board).
#define M2_RST_GDIR     GPIO6_GDIR
#define M2_RST_SET      GPIO6_DR_SET
#define M2_RST_CLEAR    GPIO6_DR_CLEAR
#define M2_RST_PSR      GPIO6_PSR
#define M2_RST_ALT      0x5u
#else
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30
#define M2_RST_GDIR     GPIO9_GDIR
#define M2_RST_SET      GPIO9_DR_SET
#define M2_RST_CLEAR    GPIO9_DR_CLEAR
#define M2_RST_PSR      GPIO9_PSR
#define M2_RST_ALT      0xAu
#endif

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

// ---------------------------------------------------------------------------
// CONTINUITY PROBE -- is the card's UART TX line actually driven, and does PDn
// actually swing?
//
// ★ WHY.  With R345 and R96 both hand-bridged, the MIMXRT1060-EVKB traces
// COMPLETE on paper -- rail always on, both level shifters fitted and enabled,
// reset chain populated, both UART directions populated -- and the card still
// never greets (start_inds=0).  Every remaining hypothesis is about PHYSICAL
// state, which a netlist cannot settle.  This is the instrument that settled
// exactly the same question on the 1170: it drove the MCU's RX pad through its
// own internal pull-up and then pull-down and asked whether anything external
// held it.  That is what proved the R1901 bridge conducted
// ("pullup_reads=0 pulldown_reads=0 -> DRIVEN LOW").
//
// READING IT:
//   pullup=1 pulldown=0  the pad FOLLOWS our pull -> nothing external is
//                        driving it: the bridge is open, or the shifter output
//                        is high-Z, or the card is not powered
//   pullup=0 pulldown=0  DRIVEN LOW  by something external
//   pullup=1 pulldown=1  DRIVEN HIGH by something external -- which is what a
//                        healthy idle UART line looks like
//
// ★ It re-muxes the RX pad to GPIO, so it MUST run before the transport takes
// the pad, and it hands it back afterwards.  That re-mux is why these helpers
// were deleted from m2_sdio_probe during BT-1: they stole the pad from the
// UART and turned bytes into "edges".
#if defined(M2_CONTINUITY_PROBE)
#if defined(ARDUINO_MIMXRT1060_EVKB)
  #define M2_RX_MUX  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_07   // LPUART3_RX pad
  #define M2_RX_PAD  IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_B1_07
  #define M2_RX_BIT  23                                    // GPIO1_IO23
  #define M2_RX_PSR  GPIO6_PSR     // fast-GPIO alias -- see the GPR26 note above
  #define M2_RX_GDIR GPIO6_GDIR
  #define M2_RX_ALT  0x5u                                  // ALT5 = GPIO1
  // RT1060 pad control: PKE bit12, PUE bit13, PUS bits14-15
  // (00 = 100K pull-DOWN, 10 = 100K pull-UP).  These encodings are NOT the
  // same as the RT1176's, which is why they are spelled out here.
  #define M2_RX_PULLUP   0xB000u
  #define M2_RX_PULLDOWN 0x3000u
#else
  #define M2_RX_MUX  (*(volatile uint32_t *)0x400E8180u)   // GPIO_AD_27
  #define M2_RX_PAD  (*(volatile uint32_t *)0x400E83C4u)
  #define M2_RX_BIT  26
  #define M2_RX_PSR  GPIO9_PSR
  #define M2_RX_GDIR GPIO9_GDIR
  #define M2_RX_ALT  0xAu
  #define M2_RX_PULLUP   0x04u
  #define M2_RX_PULLDOWN 0x08u
#endif

static void m2RxContinuity(bool *up, bool *down) {
    M2_RX_MUX = 0x10u | M2_RX_ALT;              // SION so the pad is readable
    M2_RX_GDIR &= ~(1u << M2_RX_BIT);           // input
    M2_RX_PAD = M2_RX_PULLUP;   delayMicroseconds(500);
    *up   = (M2_RX_PSR >> M2_RX_BIT) & 1u;
    M2_RX_PAD = M2_RX_PULLDOWN; delayMicroseconds(500);
    *down = (M2_RX_PSR >> M2_RX_BIT) & 1u;
    M2_RX_PAD = M2_RX_PULLUP;   delayMicroseconds(500);
}

// Watch the card's TX pad as a GPIO.  Counts EDGES, not bytes -- the point is
// only "did anything move", which is what distinguishes a dead line from a
// talking one.
static void m2WatchRxLine(uint32_t ms, bool *anyHigh, uint32_t *edges) {
    M2_RX_MUX = 0x10u | M2_RX_ALT;
    M2_RX_GDIR &= ~(1u << M2_RX_BIT);
    M2_RX_PAD = M2_RX_PULLUP;
    bool prev = (M2_RX_PSR >> M2_RX_BIT) & 1u, hi = prev; uint32_t e = 0;
    for (uint32_t i = 0; i < ms * 100u; i++) {  // ~10 us per sample
        bool now = (M2_RX_PSR >> M2_RX_BIT) & 1u;
        if (now != prev) { e++; prev = now; }
        if (now) hi = true;
        delayMicroseconds(10);
    }
    *anyHigh = hi; *edges = e;
}

// Drive PDn low and high and READ THE PAD BACK each time.
// ★ On the MIMXRT1060-EVKB that pin now drives THREE fitted loads -- R343 to
// SD_PWREN, R344 to SPDIF_IN, and the hand-bridged R345 to WL_RST# -- so
// "can it still swing" is a real question, not a formality.
static void m2PdnSwing(bool *lowOk, bool *highOk) {
    M2_WL_RST_MUX = 0x10u | M2_RST_ALT;         // SION: readable while driven
    M2_RST_GDIR |= (1u << M2_WL_RST_BIT);
    M2_RST_CLEAR = (1u << M2_WL_RST_BIT);  delay(5);
    *lowOk  = !((M2_RST_PSR >> M2_WL_RST_BIT) & 1u);
    M2_RST_SET = (1u << M2_WL_RST_BIT);    delay(5);
    *highOk =  ((M2_RST_PSR >> M2_WL_RST_BIT) & 1u);
}

static void m2ContinuityProbe() {
    bool up=false, down=false;
    m2RxContinuity(&up, &down);
    CONSOLE.print("rx_continuity: pullup_reads="); CONSOLE.print(up ? 1 : 0);
    CONSOLE.print(" pulldown_reads="); CONSOLE.print(down ? 1 : 0);
    CONSOLE.print(" -> ");
    if (up && !down)      CONSOLE.println("FLOATING (nothing external drives it)");
    else if (!up && !down) CONSOLE.println("DRIVEN LOW externally");
    else if (up && down)   CONSOLE.println("DRIVEN HIGH externally (a healthy idle UART)");
    else                   CONSOLE.println("INVERTED -- read is not trustworthy");

    bool lowOk=false, highOk=false;
    m2PdnSwing(&lowOk, &highOk);
    CONSOLE.print("pdn_swing: drives_low="); CONSOLE.print(lowOk ? 1 : 0);
    CONSOLE.print(" drives_high="); CONSOLE.print(highOk ? 1 : 0);
    CONSOLE.println(lowOk && highOk ? " -> PIN SWINGS" : " -> PIN STUCK (loaded or shorted)");

    // Now cycle the module with the pad still on GPIO and watch for ANY edge.
    // The ROM greets once per power-up; if it greets, this must see movement.
    //
    // ★ WINDOW LENGTH IS LOAD-BEARING and the first version got it wrong.  It
    // watched 400 ms.  The 1170's own sequence waits 1000 ms after PDn release
    // before its loader looks, and on that board the greeting lands within
    // roughly the first 200 ms of THAT wait -- so a slower-booting part can
    // greet outside a 400 ms window entirely and read as silent.  Watch in
    // SEGMENTS instead, printing each, so the ANSWER CARRIES ITS OWN TIMING:
    // a greeting at 1.4 s and no greeting at all are then different readings
    // rather than the same "edges=0".
    // PDn is held low for 150 ms -- above the 100 ms minimum the MAYA-W1 SIM
    // (UBX-21010495 R09 s2.4.1) requires for a correct reset.
    M2_RST_CLEAR = (1u << M2_WL_RST_BIT); delay(150);
    M2_RST_SET   = (1u << M2_WL_RST_BIT);
    uint32_t total = 0;
    for (int seg = 0; seg < 5; seg++) {        // 5 x 600 ms = 3 s of watching
        bool anyHigh=false; uint32_t edges=0;
        m2WatchRxLine(600, &anyHigh, &edges);
        total += edges;
        CONSOLE.print("rx_after_pdn[");    CONSOLE.print(seg);
        CONSOLE.print("]: t=");            CONSOLE.print((seg + 1) * 600);
        CONSOLE.print("ms any_high=");     CONSOLE.print(anyHigh ? 1 : 0);
        CONSOLE.print(" edges=");          CONSOLE.println(edges);
    }
    CONSOLE.print("rx_after_pdn_total: edges="); CONSOLE.print(total);
    CONSOLE.println(total ? "  -> THE CARD TRANSMITTED" : "  -> silent for 3 s");
    CONSOLE.println("continuity_probe_done (pad handed back to the UART)");
}
#endif  // M2_CONTINUITY_PROBE

// --- HCI opcodes and event codes (Core 5.2 Vol 4 Part E 7.x) ------------------
static const uint16_t OP_RESET            = 0x0C03;
static const uint16_t OP_READ_LOCAL_VER   = 0x1001;
static const uint16_t OP_READ_BUFFER_SIZE = 0x1005;
static const uint16_t OP_READ_BD_ADDR     = 0x1009;
static const uint16_t OP_INQUIRY          = 0x0401;
static const uint16_t OP_REMOTE_NAME_REQ  = 0x0419;
static const uint8_t  EV_INQUIRY_COMPLETE = 0x01;
static const uint8_t  EV_INQUIRY_RESULT   = 0x02;
static const uint8_t  EV_REMOTE_NAME_DONE = 0x07;

static void printHex8(uint8_t v)   { if (v < 0x10) CONSOLE.print('0'); CONSOLE.print(v, HEX); }
static void printHex16(uint16_t v) { printHex8((uint8_t)(v >> 8)); printHex8((uint8_t)v); }
static void printHex24(uint32_t v) { printHex8((uint8_t)(v >> 16)); printHex16((uint16_t)v); }
static void printBd(const uint8_t *bd) { char s[18]; hciFormatBd(bd, s); CONSOLE.print(s); }

// ★ COUNTERS ARE CUMULATIVE ACROSS begin(), and they have to be.  The baud
// sweep calls Hci::begin() once per rate, which zeroes every counter -- so
// without these bases the heartbeat after a four-rate sweep printed
// `timeouts=0` on a run that had just suffered FOURTEEN of them.  A counter
// that reads zero when the failures were real is worse than no counter: it
// reads as a healthy link that merely has nothing to say.  Measured on the
// card-absent gate before this was added.
// Zero unless a sweep has happened, so every pre-existing transcript and every
// gate assertion on these fields is byte-identical.
static uint32_t s_toBase = 0, s_frBase = 0, s_stBase = 0, s_qfBase = 0, s_lateBase = 0;

static void hciCountersFold() {          // call BEFORE an Hci::begin() that would reset them
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
static void idleMs() { delay(1); }

// --- B2 bookkeeping: filled by the event callback, which the pump runs from
// inside delay() while the probe waits -------------------------------------------
struct Found { HciInquiryResult r; bool named; HciRemoteName name; };
static Found         s_found[8];
static uint8_t       s_foundN = 0;
static volatile bool s_inqDone = false;
static uint8_t       s_inqStatus = 0xFF;
static volatile bool s_nameDone = false;
// B4/B6/B7 (BT-2/BT-3): the productized path, from M2Radio/bt -- L2cap (basic
// mode signalling + CO channels + ACL demux + credits), BtLink (inquiry,
// Create_Connection, SSP pairing with legacy-PIN fallback, encryption), Sdp
// (AudioSink ProtocolDescriptorList -> AVDTP version) and Avdtp (DISCOVER..
// START initiator).  These replace the BT-2 inline prototype this file used
// to carry -- BtLink.cpp's header records it was ported line-for-line from
// this file's former probeInquiry()/probeConnect()/onEvent().
#if defined(M2_BT_CONNECT)
static L2cap  l2(hciIo);
static BtLink link(hci);
static Avdtp  avdtp;
static SdpServer sdpServer;   // answers the peer's SDP queries of US (both headsets make one on AVDTP contact)

static uint32_t nowMs() { return millis(); }
static void btLog(void *, const char *s) { CONSOLE.println(s); }

// B6/B7 bookkeeping: filled by the L2cap data callback (record only; all TX
// happens from probeConnect()'s main-context loop, never from here).
static volatile bool     s_sdpDone  = false;
static volatile uint16_t s_avdtpVer = 0;
// Phase-2 outcome, latched by probeConnect() and echoed in every loop() heartbeat
// so the result is readable from ANY capture -- the one-shot setup() output is
// easily missed across a reset (the VCOM reconnect gap), and this makes the bench
// run deterministic regardless of when the reader attaches.
static const char *s_p2link = "n/a", *s_p2sec = "n/a", *s_p2pair = "-";
static int         s_p2avdtp = -1;
static uint16_t    s_p2mtu   = 0;

static void onL2capData(void *, L2cap::Channel &ch, const uint8_t *payload, uint16_t len) {
    if (sdpServer.onData(ch, payload, len)) return;        // the PEER's SDP query on its own channel: answered from the main loop
    if (ch.psm == Avdtp::PSM && ch.localCid == 0x0041) {   // signalling channel only -- the media
        avdtp.onSignalling(payload, len);                  // channel (0x0042) shares this PSM
    } else if (ch.psm == Sdp::PSM) {
        s_avdtpVer = Sdp::parseAvdtpVersion(payload, len);
        s_sdpDone  = true;
    }
}

// Hci::AclFn -> L2cap::onAcl thunk (L2cap's RX entry point takes no ctx).
static void onAclThunk(void *, uint16_t handle, const uint8_t *d, uint16_t len) {
    l2.onAcl(handle, d, len);
}
#endif

#if defined(M2_BT_LOOPBACK)
// Phase 0 loopback (BT-3): its own connection bookkeeping, independent of
// BtLink's m_handle above (compiled only under M2_BT_CONNECT, which this
// build leaves OFF -- loopback runs standalone, right after the baud switch).
static volatile bool s_lbConnDone = false; static volatile uint16_t s_lbConnHandle = 0;
#endif

static volatile bool s_remExtDone = false;
static void onEvent(void *, uint8_t code, const uint8_t *p, uint8_t len) {
    if (code != 0x13) {                              // timeline: every event but Number_Of_Completed_Packets
        CONSOLE.print("[t="); CONSOLE.print(millis()); CONSOLE.print("] ev=0x"); printHex8(code); CONSOLE.println();
    }
    if (code == 0x12 && len >= 8) {                  // Role Change: status, bd, new_role (0=master 1=slave)
        CONSOLE.print("role_change: status=0x"); printHex8(p[0]);
        CONSOLE.print(" bd="); printBd(p + 1);
        CONSOLE.print(" our_new_role="); CONSOLE.println(p[7] == 0 ? "master" : "slave");
        return;
    }
    if (code == 0x23 && len >= 13) {                 // Read Remote Extended Features Complete
        CONSOLE.print("remote_ext_features page="); CONSOLE.print(p[3]);
        CONSOLE.print(" max="); CONSOLE.print(p[4]); CONSOLE.print(" status=0x"); printHex8(p[0]); CONSOLE.print(" f=");
        for (int i = 0; i < 8; i++) { printHex8(p[5 + i]); CONSOLE.print(' '); }
        if (p[3] == 1) { CONSOLE.print(" ssp_host="); CONSOLE.print(p[5] & 0x01); CONSOLE.print(" sc_host="); CONSOLE.print((p[5] >> 3) & 1); }
        if (p[3] == 2) { CONSOLE.print(" sc_ctrl=");  CONSOLE.print(p[6] & 0x01); }
        CONSOLE.println();
        s_remExtDone = true;
        return;
    }
    if (code == EV_INQUIRY_RESULT) {
        uint8_t n = hciInquiryResultCount(p, len);
        for (uint8_t i = 0; i < n && s_foundN < 8; i++) {
            Found &f = s_found[s_foundN];
            if (!hciParseInquiryResult(p, len, i, &f.r)) break;
            f.named = false; s_foundN++;
            CONSOLE.print("inq: bd="); printBd(f.r.bd);
            CONSOLE.print(" cod=0x"); printHex24(f.r.cod);
            CONSOLE.print(" psrm="); CONSOLE.print(f.r.psrm);
            CONSOLE.print(" clk=0x"); printHex16(f.r.clockOffset);
            CONSOLE.println();
        }
        if (n == 0) { CONSOLE.print("inq: malformed len="); CONSOLE.println(len); }
    } else if (code == EV_INQUIRY_COMPLETE && len >= 1) {
        s_inqStatus = p[0]; s_inqDone = true;
    } else if (code == EV_REMOTE_NAME_DONE) {
        HciRemoteName nm;
        if (hciParseRemoteNameComplete(p, len, &nm)) {
            for (uint8_t i = 0; i < s_foundN; i++)
                if (memcmp(s_found[i].r.bd, nm.bd, 6) == 0) { s_found[i].name = nm; s_found[i].named = true; }
        }
        s_nameDone = true;
    }
#if defined(M2_BT_LOOPBACK)
    else if (code == 0x03 && len >= 11) {        // Connection_Complete (Core 5.2 Vol 4 Part E 7.7.3):
        // status(1) handle(2) bd(6) link_type(1) encryption_mode(1) -- the
        // controller reports its LOCAL LOOPBACK ACL handle this way (link_type
        // ACL), the only way the host learns which handle to write on.
        s_lbConnHandle = (uint16_t)(p[1] | (p[2] << 8));
        CONSOLE.print("lb_conn_complete: status=0x"); printHex8(p[0]);
        CONSOLE.print(" handle=0x"); printHex16(s_lbConnHandle);
        CONSOLE.print(" link_type="); CONSOLE.println(p[9]);
        s_lbConnDone = true;
    }
#endif
    else {
        CONSOLE.print("hci_event: code=0x"); printHex8(code); CONSOLE.print(" len="); CONSOLE.println(len);
    }
#if defined(M2_BT_CONNECT)
    // BtLink owns inquiry/connect/SSP-pairing/encryption; L2cap owns ACL
    // credit accounting (Number_Of_Completed_Packets, code 0x13).  Forward
    // every event to both, IN ADDITION to the base handling above -- this is
    // not exclusive with it (e.g. EV_INQUIRY_RESULT is still bookkept into
    // s_found by the base path too).
    link.onEvent(code, p, len);
    l2.onEvent(code, p, len);
#endif
}

#if defined(M2_BT_CONNECT)
// L2cap::begin()'s aclCredits argument -- the Total_Num_ACL_Data_Packets field
// from Read_Buffer_Size, captured below in probeIdentity().
static uint8_t s_aclNum = 0;
#endif

// --- B1: identity ---------------------------------------------------------------
static void probeIdentity() {
    Hci::Reply r;
    Hci::Error e = hci.run(OP_READ_LOCAL_VER, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 8) {
        // Return params after status: HCI_Version(1) HCI_Revision(2)
        // LMP_Version(1) Manufacturer_Name(2) LMP_Subversion(2)
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
        // ACL_Data_Packet_Length(2) Synchronous_Data_Packet_Length(1)
        // Total_Num_ACL_Data_Packets(2) Total_Num_Synchronous_Data_Packets(2)
        uint16_t aclLen = (uint16_t)(r.params[0] | (r.params[1] << 8));
        uint16_t aclNum = (uint16_t)(r.params[3] | (r.params[4] << 8));
        CONSOLE.print("hci_buffer: acl_len="); CONSOLE.print(aclLen);
        CONSOLE.print(" acl_num="); CONSOLE.print(aclNum);
        CONSOLE.print(" sco_len="); CONSOLE.print(r.params[2]);
        CONSOLE.print(" sco_num="); CONSOLE.println(r.params[5] | (r.params[6] << 8));
        hci.setAclMax(aclLen);           // the parser's plausibility bound becomes the card's word
#if defined(M2_BT_CONNECT)
        s_aclNum = (uint8_t)(aclNum > 255 ? 255 : aclNum);   // L2cap::begin()'s aclCredits argument
#endif
    } else printFail("hci_buffer", e, r, "short_reply");
}

#if defined(M2_BT_FAST_BAUD)
static const uint16_t OP_VS_SET_BAUD = 0xFC09;
// Phase 0: vendor set-baud (uint32 LE), then re-baud the port and re-validate
// with a fresh Reset + identity.  Every exit is named.
static void probeFastBaud() {
    uint32_t rate = M2_BT_FAST_BAUD;
    uint8_t p[4] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24) };
    (void)p;
    // Task 4 (silicon) refuted the "CC leaves at the OLD rate, then the card
    // switches" model: at 921600 and 3000000 the vendor set-baud got no_response
    // and poisoned the boot.  The IW416 acts on 0xFC09 by switching its OWN UART
    // and returning the Command Complete AT THE NEW RATE -- so a run() that waits
    // for that CC at 115200 can never see it, times out, and we desync.
    // Experiment A: send the command RAW (H4 01 09 FC 04 <rate LE>) without
    // waiting; let rebaud()'s end() drain those 8 bytes at the old rate, switch
    // the host, and read the CC + validate at the NEW rate.
    uint8_t cmd[8] = { 0x01, (uint8_t)(OP_VS_SET_BAUD & 0xFF), (uint8_t)(OP_VS_SET_BAUD >> 8), 4,
                       (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24) };
    hciIo.write(cmd, sizeof cmd);
    hciIo.rebaud(rate);                                   // end() drains the 8 bytes at 115200, then rewrites BAUD
    hciCountersFold(); hci.begin();                       // fresh HCI state at the new rate
    delay(20);                                            // let the controller finish switching its own UART
    Hci::Reply r;
    Hci::Error e = hci.run(OP_RESET, nullptr, 0, &r, 1000, idleMs);
    if (e != Hci::OK) {
        CONSOLE.print("bt_baud_switch=fail rate="); CONSOLE.print(rate);
        CONSOLE.print(" reason="); CONSOLE.println(Hci::errorName(e));
        // Leave the port as we found it (mirrors btBaudSweep()'s fallback):
        // the unconditional probeInquiry()/probeConnect() that follow must
        // not run at a rate the controller never actually switched to.
        hciIo.rebaud(115200);
        hciCountersFold(); hci.begin();
        CONSOLE.println("bt_baud_switch=reverted rate=115200");
        return;
    }
    CONSOLE.print("bt_baud_switch=ok rate="); CONSOLE.println(rate);
    probeIdentity();                                      // identity again, at the new rate
}
#endif

#if defined(M2_BT_LOOPBACK)
static uint16_t s_lbHandle = 0; static uint32_t s_lbEchoed = 0, s_lbBytes = 0;
// Loopback ACL packets come back on the loopback handle (Vol 4 Part E 7.6.2):
// count them and their bytes from onAcl -- no TX here, this is record-only.
static void lbOnAcl(void *, uint16_t handle, const uint8_t *, uint16_t len) {
    if (handle == s_lbHandle) { s_lbEchoed++; s_lbBytes += len; }
}
// Phase 0 (BT-3): put the controller in LOCAL LOOPBACK and count N ACL
// packets echoed back -- the loss/throughput measurement this board's
// one-sided flow control (M2_BT_ASSERT_CTS) demands.  Runs right after the
// baud switch, before probeInquiry() -- so it registers onEvent() itself
// rather than relying on probeInquiry() to have done it.
static void probeLoopback() {
    hci.onEvent(onEvent, nullptr);
    Hci::Reply r; uint8_t mode = 0x01;                      // 0x01 = local loopback
    s_lbConnDone = false;
    Hci::Error e = hci.run(0x1802, &mode, 1, &r, 1000, idleMs);
    if (e != Hci::OK) { CONSOLE.print("loopback=fail (Write_Loopback_Mode) reason="); CONSOLE.print(Hci::errorName(e)); CONSOLE.print(" status=0x"); printHex8(r.status); CONSOLE.println(); return; }
    uint32_t t0 = millis();                                 // the controller reports its loopback ACL handle
    while (!s_lbConnDone && millis() - t0 < 3000) delay(10);  // via Connection_Complete (link_type ACL)
    if (!s_lbConnDone) { CONSOLE.println("loopback=fail (no loopback Connection_Complete)"); return; }
    s_lbHandle = s_lbConnHandle; s_lbEchoed = 0; s_lbBytes = 0;
    hci.onAcl(lbOnAcl, nullptr);
    const uint32_t N = 200; const uint16_t LEN = 600;       // ~ the media packet size
    static uint8_t pkt[9 + 600];
    uint16_t hf = (uint16_t)((s_lbHandle & 0x0FFF) | (0x02u << 12));
    pkt[0] = 0x02; pkt[1] = (uint8_t)hf; pkt[2] = (uint8_t)(hf >> 8);
    pkt[3] = (uint8_t)(LEN + 4); pkt[4] = (uint8_t)((LEN + 4) >> 8);
    pkt[5] = (uint8_t)LEN; pkt[6] = (uint8_t)(LEN >> 8); pkt[7] = 0x40; pkt[8] = 0x00;
    for (uint16_t i = 0; i < LEN; i++) pkt[9 + i] = (uint8_t)i;
    uint32_t sent = 0, tStart = millis();                   // pace on the echo count: never more than ACL_NUM outstanding
    const uint32_t ACL_NUM = 7;                             // acl_num from Read_Buffer_Size
    while (sent < N && millis() - tStart < 20000) {
        if (sent - s_lbEchoed < ACL_NUM) { hciIo.write(pkt, sizeof pkt); sent++; }
        delay(1);
    }
    t0 = millis(); while (s_lbEchoed < sent && millis() - t0 < 3000) delay(10);
    uint32_t ms = millis() - tStart;
    CONSOLE.print("loopback_sent="); CONSOLE.print(sent); CONSOLE.print(" echoed="); CONSOLE.print(s_lbEchoed);
    CONSOLE.print(" bytes="); CONSOLE.print(s_lbBytes); CONSOLE.print(" ms="); CONSOLE.print(ms);
    CONSOLE.print(" kbps="); CONSOLE.println(ms ? (s_lbBytes * 8) / ms : 0);
    mode = 0x00; hci.run(0x1802, &mode, 1, &r, 1000, idleMs);
    hci.onAcl(nullptr, nullptr);
}
#endif

// --- B2: who is in the room -------------------------------------------------------
static void probeInquiry() {
    hci.onEvent(onEvent, nullptr);
    s_foundN = 0; s_inqDone = false; s_inqStatus = 0xFF;
    // LAP = GIAC 0x9E8B33 little-endian, Inquiry_Length 0x08 = 10.24 s, Num_Responses 0 = unlimited
    const uint8_t params[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };
    Hci::Reply r;
    Hci::Error e = hci.run(OP_INQUIRY, params, sizeof params, &r, 1000, idleMs);
    if (e != Hci::OK || !r.statusEvent) { printFail("inquiry", e, r, "not_command_status"); return; }
    CONSOLE.println("inquiry=started");
    uint32_t t0 = millis();
    while (!s_inqDone && millis() - t0 < 12000) delay(10);     // events arrive via the pump
    CONSOLE.print("inquiry_complete: status=0x"); printHex8(s_inqDone ? s_inqStatus : 0xFF);
    CONSOLE.print(" n="); CONSOLE.print(s_foundN);
    if (!s_inqDone) CONSOLE.print(" timeout=1");
    CONSOLE.println();

    for (uint8_t i = 0; i < s_foundN; i++) {
        // Remote_Name_Request: BD_ADDR(6) Page_Scan_Repetition_Mode(1) Reserved(1) Clock_Offset(2, bit 15 = valid)
        uint8_t p[10];
        memcpy(p, s_found[i].r.bd, 6);
        p[6] = s_found[i].r.psrm; p[7] = 0;
        p[8] = (uint8_t)(s_found[i].r.clockOffset & 0xFF);
        p[9] = (uint8_t)((s_found[i].r.clockOffset >> 8) | 0x80);
        s_nameDone = false;
        e = hci.run(OP_REMOTE_NAME_REQ, p, sizeof p, &r, 1000, idleMs);
        t0 = millis();
        while (e == Hci::OK && !s_nameDone && millis() - t0 < 5000) delay(10);
        CONSOLE.print("inq_name: bd="); printBd(s_found[i].r.bd);
        if (e != Hci::OK)         { CONSOLE.print(" fail reason="); CONSOLE.println(Hci::errorName(e)); continue; }
        if (!s_found[i].named)    { CONSOLE.println(" fail reason=no_name_event"); continue; }
        CONSOLE.print(" status=0x"); printHex8(s_found[i].name.status);
        CONSOLE.print(" name=\""); CONSOLE.print(s_found[i].name.name); CONSOLE.println("\"");
    }
}

#if defined(M2_BT_CONNECT)
// --- B4+B6+B7: connect + pair/encrypt (BtLink) -> SDP (L2cap+Sdp) -> AVDTP
// DISCOVER..START (L2cap+Avdtp).  This is the productized replacement for the
// BT-2 inline prototype this function used to be: BtLink now drives its own
// inquiry+Create_Connection+SSP (with legacy-PIN fallback) rather than reusing
// probeInquiry()'s s_found, so a duplicate "inq:"/"connect:" trace from
// BtLink's own log alongside probeInquiry()'s is expected on a CONNECT build.
static void probeConnect() {
#if defined(M2_BT_TARGET_NAME)
    const char *target = M2_BT_TARGET_NAME;
#else
    const char *target = nullptr;
#endif
    link.setLog(btLog, nullptr);
    link.setPin("1234");
#if defined(M2_BT_LEGACY_PIN)
    // Force legacy PIN (SSP disabled from the start): the IW416<->ESP32 sink SSP
    // stalls ~25 s at LMP IO-cap and poisons the SSP->PIN fallback -- silicon
    // 2026-09-03.  Headsets pair by SSP, so this stays a bench knob (default OFF).
    link.setLegacyPin(true);
#endif
    BtLink::Result r = link.connect(target, nowMs, idleMs);
    s_p2link = BtLink::resultName(r);
    CONSOLE.print("link="); CONSOLE.println(BtLink::resultName(r));
    if (r != BtLink::OK) return;

    r = link.pairAndEncrypt(nowMs, idleMs);
    s_p2sec = BtLink::resultName(r); s_p2pair = link.pairedBy();
    CONSOLE.print("secure="); CONSOLE.print(BtLink::resultName(r));
    CONSOLE.print(" paired_by="); CONSOLE.println(link.pairedBy());
    if (r != BtLink::OK) return;

    l2.begin(link.handle(), s_aclNum);
    l2.acceptIncoming(true);
    l2.onData(onL2capData, nullptr);
    hci.onAcl(onAclThunk, nullptr);

    // B6: SDP -> AVDTP version (informational -- a failure here must not abort B7)
    L2cap::Channel *sdp = l2.connect(Sdp::PSM, 0x0040);
    uint32_t t0 = millis();
    if (sdp) while (sdp->state != L2cap::OPEN && millis() - t0 < 5000) { sdpServer.service(l2); l2.service(); delay(10); }
    if (sdp && sdp->state == L2cap::OPEN) {
        uint8_t q[18];
        l2.send(sdp->remoteCid, q, Sdp::buildAudioSinkPdlRequest(q, 1));
        s_sdpDone = false;
        t0 = millis();
        while (!s_sdpDone && millis() - t0 < 5000) { sdpServer.service(l2); l2.service(); delay(10); }
    }
    CONSOLE.print("sdp_avdtp_version=0x"); printHex16(s_avdtpVer);
    CONSOLE.println(s_avdtpVer ? " (B6 DONE)" : " (no response)");

    // B7: AVDTP DISCOVER..START
    L2cap::Channel *sig = l2.connect(Avdtp::PSM, 0x0041);
    if (!sig) { CONSOLE.println("avdtp=fail (no L2CAP channel)"); return; }
    t0 = millis();
    while (sig->state != L2cap::OPEN && millis() - t0 < 5000) { sdpServer.service(l2); l2.service(); delay(10); }
    if (sig->state != L2cap::OPEN) { CONSOLE.println("avdtp=fail (signalling channel not open)"); return; }
    avdtp.begin(l2, 0x0041, 0x0042);
    Avdtp::SbcConfig want = { 44100, Avdtp::JOINT_STEREO, 16, 8, Avdtp::LOUDNESS, 2, 53 };
    avdtp.start(want);
    t0 = millis();
    Avdtp::State last = Avdtp::IDLE;
    while (avdtp.state() != Avdtp::STREAMING && avdtp.state() != Avdtp::FAILED && millis() - t0 < 15000) {
        sdpServer.service(l2); l2.service(); avdtp.service(); delay(10);
        if (avdtp.state() != last) { last = avdtp.state(); CONSOLE.print("avdtp_state="); CONSOLE.println((int)last); }
    }
    s_p2avdtp = (int)avdtp.state(); s_p2mtu = avdtp.mediaMtu();
    if (avdtp.state() == Avdtp::STREAMING) {
        CONSOLE.print("avdtp_caps: rates=0x"); printHex8(avdtp.caps().rates);
        CONSOLE.print(" modes=0x"); printHex8(avdtp.caps().modes);
        CONSOLE.print(" bitpool="); CONSOLE.print(avdtp.caps().minBitpool);
        CONSOLE.print(".."); CONSOLE.println(avdtp.caps().maxBitpool);
        CONSOLE.print("avdtp_start=ok media_mtu="); CONSOLE.println(avdtp.mediaMtu());
        CONSOLE.print("sdp_served="); CONSOLE.print(sdpServer.answered());        // the peer's queries of US, answered
        CONSOLE.print(" delay_report="); CONSOLE.println(avdtp.peerDelayTenthMs());
        CONSOLE.println("B7 DONE");
    } else {
        CONSOLE.print("avdtp=fail state="); CONSOLE.print((int)avdtp.state());
        CONSOLE.print(" error=0x"); printHex8(avdtp.error());
        CONSOLE.println();
    }
}
#endif

// ---------------------------------------------------------------------------
// Assert the card's CTS input, so it is permanently CLEAR TO SEND.
//
// ★ WHY THIS MIGHT BE THE WHOLE PROBLEM.  NXP configure this link WITH
// hardware flow control (`enableRxRTS = 1; enableTxCTS = 1` in their
// controller_hci_uart_get_configuration), and their rework guide for this very
// board -- "Hardware Rework Guide for MIMXRT1170-EVKB and Murata M.2 Module" --
// says to remove R183 and R1816 and fit R404, R1901 AND R1902.  We fitted R404
// and R1901 (worked out from the schematic) but not R1902, and removed neither.
//
// J54 pin 36 is the CARD'S CTS INPUT (the net is named BT_UART_RTS, from the
// host's point of view) and it IS populated -- it reaches GPIO_DISP_B2_13.  We
// have never driven it.  CTS is active LOW, so an undriven or high pad tells
// the card "not clear to send".  That would explain the exact symptom: the
// boot ROM talks (it does not use flow control) and the booted firmware, which
// does, never transmits a byte.
//
// Driving it low costs nothing and needs no rework.  ★ CALL IT ONLY AFTER THE
// MODULE IS OUT OF RESET -- this pin is also CON[7], sampled at reset and
// required to be 1; see the ordering note at the call site.
// ★ SIDE EFFECT, and it is
// real: R1866 ties this net to ETHPHY_RST_B, so holding it low HOLDS THE
// GIGABIT ETHERNET PHY IN RESET.  Fine for a Bluetooth probe, fatal for the
// enet examples -- which is exactly why this board's LPUART2 has no usable
// flow control and why this is a GPIO hack rather than MODIR.
//
// GPIO_DISP_B2_13: mux 0x400E8248, pad 0x400E848C, ALT5 = GPIO5_IO14
// (ALT3 would be LPUART2_RTS_B; confirmed in the reference manual).
#if defined(ARDUINO_MIMXRT1060_EVKB)
// MIMXRT1060-EVKB: the host's RTS -> the card's CTS input is BT_UART_RTS, which
// the RevB1 netlist puts on GPIO_AD_B0_02 (= GPIO1_IO02) through the fitted
// R354.  ★ NOTE IT IS NOT AN LPUART3 PIN.  NXP's own RT1060 wake macro names
// GPIO_AD_B1_05 (LPUART3_RTS_B) -- that is a DIFFERENT board's routing, and on
// this one AD_B1_05 does not reach the card at all.  So here the line is a
// plain GPIO with no UART alternate, which also means hardware flow control is
// impossible on this board, exactly as on the 1170.
#define M2_BT_CTS_MUX IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B0_02
#define M2_BT_CTS_PAD IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_B0_02
#define M2_BT_CTS_BIT 2
#define M2_BT_CTS_GDIR   GPIO6_GDIR
#define M2_BT_CTS_CLEAR  GPIO6_DR_CLEAR
#define M2_BT_CTS_GPIO_ALT 0x5u
#else
#define M2_BT_CTS_MUX (*(volatile uint32_t *)0x400E8248u)
#define M2_BT_CTS_PAD (*(volatile uint32_t *)0x400E848Cu)
#define M2_BT_CTS_BIT 14
#define M2_BT_CTS_GDIR   GPIO5_GDIR
#define M2_BT_CTS_CLEAR  GPIO5_DR_CLEAR
#define M2_BT_CTS_GPIO_ALT 0x5u
#endif

// ---------------------------------------------------------------------------
// Wake the controller from BOOT SLEEP -- the step NXP's loader performs and
// this tree never did.
//
// ★ WHY.  NXP's fw_loader_uart.c calls wakeUpControllerFromBootSleep() from
// uart_fw_download() -- BEFORE the image goes across, not after it.  For the
// RT1170 it names GPIO_DISP_B2_13 (the pad this board wires to J54 pin 36),
// re-muxes it away from LPUART2_RTS_B to a plain GPIO, drives it LOW, holds
// 10 ms, then hands the pad BACK to LPUART2_RTS_B.  Facts taken from that
// file's pin macros, literal level and call order only -- nothing transcribed;
// it is NXP LA_OPT licensed and this is an independent implementation of the
// same documented behaviour, exactly as the V3 loader was.
//
// ★ THIS IS NOT THE CTS EXPERIMENT ALREADY RUN AND REFUTED.  That one held the
// pad LOW indefinitely, from BEFORE the module reset -- which latched CON[7]=0,
// a Reserved configuration -- with the mux left on GPIO and never returned.
// This is a 10 ms PULSE, after the module is up (so CON[7] was sampled as 1),
// with the pad given back to the UART afterwards.  Different level semantics,
// different duration, different mux state, different point in the sequence.
//
// A controller left in boot sleep would do precisely what we measure: take
// every block, checksum it, report no error -- and then never run the image.
//
// GPIO_DISP_B2_13: mux 0x400E8248, pad 0x400E848C (RM 12.4.6.144).  ALT5 =
// GPIO5_IO14, ALT10 = GPIO11_IO14 (the SAME pad through the fast instance --
// NXP use ALT10), ALT3 = LPUART2_RTS_B.  ALT5 here because the core already
// exposes GPIO5; the pin driven is identical.
// ★ Side effect, unavoidable and brief: R1866 ties this net to ETHPHY_RST_B,
// so the pulse also resets the gigabit PHY for 10 ms.  Harmless here; it would
// not be in an Ethernet example.
static void m2WakeFromBootSleep() {
    M2_BT_CTS_MUX = M2_BT_CTS_GPIO_ALT;         // the GPIO alternate (no SION)
    M2_BT_CTS_PAD = 0x02u;                      // NXP's pad config for this pin
    M2_BT_CTS_GDIR |= (1u << M2_BT_CTS_BIT);    // output
    M2_BT_CTS_CLEAR = (1u << M2_BT_CTS_BIT);    // drive LOW
    delay(10);                                  // NXP hold 10 ms
#if defined(ARDUINO_MIMXRT1060_EVKB)
    // No UART alternate exists on this pad here (see the note above), so the
    // pulse is released by handing the pin back as an INPUT rather than by
    // re-muxing to RTS.  Deasserted is what the card should see afterwards.
    M2_BT_CTS_GDIR &= ~(1u << M2_BT_CTS_BIT);
#else
    M2_BT_CTS_MUX = 0x3u;                       // revert: ALT3 = LPUART2_RTS_B
    M2_BT_CTS_PAD = 0x02u;
#endif
}

static void m2AssertBtCts() {
    M2_BT_CTS_MUX = 0x10u | M2_BT_CTS_GPIO_ALT; // SION | the GPIO alternate
    M2_BT_CTS_PAD = 0x0Cu;                      // no pull; we drive it
    M2_BT_CTS_GDIR |= (1u << M2_BT_CTS_BIT);    // output
    M2_BT_CTS_CLEAR = (1u << M2_BT_CTS_BIT);    // LOW = asserted = clear to send
}

// The BT-only UART firmware download.  Called immediately after the card is
// powered up and BEFORE any SDIO work -- see the ordering note in setup().
// Dump whatever Serial2 receives in the next `ms`, as HEX.
//
// ★ WHY THIS EXISTS.  The combo-over-SDIO path had never captured a single
// byte of the BT UART -- its only evidence was the Hci driver's `framing`
// counter, read much later, which can say "something unparseable arrived" but
// not WHAT.  A counter cannot tell the ROM's power-up greeting from noise, and
// cannot answer the actual question: does anything come out of that UART when
// the COMBO image boots the card over SDIO?  Bytes can.
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

static void btFirmwareDownload() {
#if defined(M2_BT_NO_UART_DNLD)
    // u-blox's path: the combo image over SDIO carries the BT core too, so
    // there is nothing to download here.  Printed rather than silent -- a
    // missing download must never be mistaken for a failed one.
    CONSOLE.println("bt_fw_source=combo_over_sdio");
    CONSOLE.println("bt_fw_download=skipped (combo-over-SDIO path)");
    // Nothing has consumed the ROM's power-up greeting on this path, so this
    // is where it should still be sitting.  Printing it is what turns the
    // later `framing=1` from an inference into a reading.
    m2DumpSerial2("bt_uart_preboot:", 300);
    s_btFwSt = BtFwLoader::NO_IMAGE;
    return;
#else
    // ★ THE BT CORE DOES NOT COME UP FROM THE SDIO COMBO DOWNLOAD.  Measured
    // on silicon 2026-08-23: that download stops 8,776 bytes short and no HCI
    // command is ever answered; the core sits in its own UART bootloader
    // sending a V3 start indication (AB 01 72 00 47 -- chipId 0x7201, the same
    // hw_version GET_HW_SPEC reports over SDIO) and waiting for an image.
    // Feed it one before saying a word of HCI.
    //
    // The loader is driven BEFORE the HCI pump is attached, deliberately: both
    // read the same Serial2, and a pump servicing Hci mid-download would eat
    // the bootloader's frames.
#if defined(HAVE_IW416_BT_FW)
    btLoader.setImage(iw416_bt_fw, iw416_bt_fw_len);
#if defined(M2_BT_INJECT_UART_CFG)
    // HYPOTHESIS UNDER TEST (2026-08-24): the download completes perfectly and
    // the controller then says nothing.  NXP's loader always writes a block of
    // UART registers -- clock divisors, MCR/ICR/FCR and a re-init trigger -- as
    // a type-5 header injected ahead of the image, whenever it changes baud.
    // We change no baud, so this passes the CURRENT rate's divisors: it tests
    // ONLY whether the card needs that register block written before its
    // firmware will run.
    btLoader.enableUartConfig(BtFwLoader::CLKDIV_115200, BtFwLoader::UARTDIV_115200);
    CONSOLE.println("bt_uart_cfg=injected");
#else
    CONSOLE.println("bt_uart_cfg=off");
#endif
    s_btFwSt = btLoader.run(3000, 500, 30000, idleMs);
#if defined(BT_FW_IS_SYNTHETIC)
    // Loud on purpose: this build is for QEMU and the image is NOT NXP firmware.
    CONSOLE.println("bt_fw_source=synthetic");
#else
    CONSOLE.println("bt_fw_source=nxp");
#endif
    CONSOLE.print("bt_fw_download=");
    CONSOLE.print(BtFwLoader::errorName(s_btFwSt));
    CONSOLE.print(" chip_id=0x");   printHex16(btLoader.chipId());
    CONSOLE.print(" loader_ver=");  CONSOLE.print(btLoader.loaderVer());
    CONSOLE.print(" start_inds=");  CONSOLE.print(btLoader.startInds());
    CONSOLE.print(" chunks=");      CONSOLE.print(btLoader.chunks());
    CONSOLE.print(" sent=");        CONSOLE.print(btLoader.bytesSent());
    CONSOLE.print("/");             CONSOLE.print(iw416_bt_fw_len);
    CONSOLE.print(" max_off=");     CONSOLE.print(btLoader.maxOffset());
    CONSOLE.print(" retx=");        CONSOLE.print(btLoader.retransmits());
    CONSOLE.print(" crc_err=");     CONSOLE.print(btLoader.crcErrors());
    CONSOLE.print(" card_err=0x");  printHex16(btLoader.lastCardErr());
    CONSOLE.print(" cfg_resends="); CONSOLE.print(btLoader.cfgHdrResends());
    CONSOLE.print(" cfg_unexp_len="); CONSOLE.print(btLoader.cfgUnexpectedLen());
    CONSOLE.print(" presync="); CONSOLE.print(btLoader.preSyncSkipped());
    CONSOLE.println();
    // Request trace: the shape of the download, which is where a subtly wrong
    // one shows itself (a length that never changes, an offset that stops
    // advancing, a final block that does not reach the end of the image).
    CONSOLE.print("bt_req_first:");
    for (uint8_t i = 0; i < btLoader.traceFirstN(); i++) {
        CONSOLE.print(" "); CONSOLE.print(btLoader.traceFirstLen(i));
        CONSOLE.print("@"); CONSOLE.print(btLoader.traceFirstOff(i));
    }
    CONSOLE.println();
    CONSOLE.print("bt_req_last:");
    {
        uint8_t n = btLoader.traceLastN();
        for (uint8_t i = 0; i < n; i++) {
            uint8_t idx = (uint8_t)((BtFwLoader::TRACE_N + i) % BtFwLoader::TRACE_N);
            CONSOLE.print(" "); CONSOLE.print(btLoader.traceLastLen(idx));
            CONSOLE.print("@"); CONSOLE.print(btLoader.traceLastOff(idx));
        }
    }
    CONSOLE.println();

    // The core needs a moment to authenticate the image and start its firmware.
    // DIAGNOSTIC: listen on the raw UART afterwards and hex-dump whatever the
    // card says unprompted.  A booting NXP controller is not required to say
    // anything here, so silence is not itself a fault -- but if it emits a
    // vendor event, another start indication (meaning it rejected the image and
    // went back to its bootloader), or bytes at an unreadable framing, that is
    // the difference between "the image was bad" and "we are asking wrongly".
    if (s_btFwSt == BtFwLoader::OK) {
        for (int w = 0; w < 4; w++) {
            uint8_t buf[64]; uint32_t n = 0;
            uint32_t t0 = millis();
            while (millis() - t0 < 500) {
                while (Serial2.available()) { int c = Serial2.read(); if (n < sizeof buf) buf[n] = (uint8_t)c; n++; }
                delay(1);
            }
            CONSOLE.print("bt_post_dnld["); CONSOLE.print(w); CONSOLE.print("]: n=");
            CONSOLE.print(n); CONSOLE.print(" hex=");
            if (!n) CONSOLE.print("none");
            else { uint32_t s = n < sizeof buf ? n : (uint32_t)sizeof buf;
                   for (uint32_t i = 0; i < s; i++) { if (buf[i] < 0x10) CONSOLE.print('0'); CONSOLE.print(buf[i], HEX); } }
            CONSOLE.println();
        }
        // And ask, RAW, right here -- before any SDIO work touches the card.
        // If HCI answers here but not later, the SDIO sequence is disturbing a
        // controller that had come up fine, which is a completely different
        // bug from "the image did not take".
        static const uint8_t RESET[] = { 0x01, 0x03, 0x0C, 0x00 };
        for (int a = 0; a < 3; a++) {
            while (Serial2.available()) (void)Serial2.read();
            Serial2.write(RESET, sizeof RESET); Serial2.flush();
            uint8_t buf[32]; uint32_t n = 0; uint32_t t0 = millis();
            while (millis() - t0 < 700) {
                while (Serial2.available()) { int c = Serial2.read(); if (n < sizeof buf) buf[n] = (uint8_t)c; n++; }
                delay(1);
            }
            CONSOLE.print("bt_raw_reset["); CONSOLE.print(a); CONSOLE.print("]: n=");
            CONSOLE.print(n); CONSOLE.print(" hex=");
            if (!n) CONSOLE.print("none");
            else { uint32_t s = n < sizeof buf ? n : (uint32_t)sizeof buf;
                   for (uint32_t i = 0; i < s; i++) { if (buf[i] < 0x10) CONSOLE.print('0'); CONSOLE.print(buf[i], HEX); } }
            CONSOLE.println();
            if (n) break;
        }
    }
#else
    CONSOLE.println("bt_fw_source=none");
    CONSOLE.println("bt_fw_download=skipped (no image compiled in)");
#endif
#endif  // M2_BT_NO_UART_DNLD

}

// ---------------------------------------------------------------------------
// BAUD SWEEP -- which rate, if any, answers HCI_Reset?
//
// ★ WHY THIS EXISTS, and it is the strongest untested lead of the whole
// programme.  u-blox's own bring-up for this module (MAYA-W1 system
// integration manual UBX-21010495 R09 §4.4.3 and §4.4.6) is:
//
//     Wi-Fi driver loads the COMBO image sdiouartiw416_combo_v0.bin over SDIO
//         -- "the Wi-Fi/Bluetooth combo firmware image for the MAYA-W1 series"
//     hciattach /dev/ttyUSB0 any 3000000 flow
//
// That is 3 Mbaud with flow control -- NOT the 115200 the boot ROM greets at.
// Every probe this tree has ever run used 115200, and the transcript
// explicitly ruled 3 Mbaud out ("which it cannot, having no usable flow
// control").  That dismissal is wrong for a PROBE: flow control matters for
// sustained A2DP throughput, not for a 4-byte command and a 7-byte reply.
//
// A controller listening at 3 Mbaud cannot decode bytes sent at 115200, so it
// never replies and never transmits -- which is exactly the silence on record
// (n=0 with framing=0; a wrong-rate link that was TALKING would show framing
// errors, and none were ever seen).
//
// Highest first, so the documented operating point is tested before the
// fallbacks, and stop at the first rate that answers.  The port is left AT the
// rate that answered, so the B1/B2 probe that follows runs there; if none
// answers the last entry is 115200, so the old behaviour is what remains.
static const uint32_t BT_SWEEP_BAUDS[] = { 3000000u, 921600u, 460800u, 115200u };
static const uint32_t BT_SWEEP_N = sizeof BT_SWEEP_BAUDS / sizeof BT_SWEEP_BAUDS[0];
static uint32_t s_baudFound = 0;

// Ask HCI_Reset at each candidate rate, stopping at the first that answers.
//
// ★ IT RUNS ONLY IF THE PROBE AT 115200 FAILED, and that ordering is
// deliberate rather than tidy.  Sweeping unconditionally would spend a Reset
// before the main sequence and CHANGE WHAT THE FAKE-CONTROLLER GATE TESTS --
// [garbage] scripts its corruption against the FIRST command it sees, so a
// sweep in front of it would absorb the garbage and the gate would silently
// stop testing the resync it exists to test.  Measured, not feared: with the
// sweep unconditional, [full] went from cmds=7 to cmds=8 immediately.
// As an escalation the behaviour is unchanged whenever the link already works.
//
// Uses the Hci driver rather than raw Serial2 reads because the pump is
// attached by now and would otherwise drain the bytes out from under us.
// begin() per rate is what makes each attempt independent: fresh parser, fresh
// command credit (the credit is assigned absolutely from each reply, so a
// wrong-rate attempt that never replies would otherwise leave it at zero and
// starve every later rate -- the same bug class as the two this driver
// already carries regression coverage for).
static void btBaudSweep() {
    for (uint32_t i = 0; i < BT_SWEEP_N; i++) {
        const uint32_t baud = BT_SWEEP_BAUDS[i];
        hciIo.end();
        hciIo.begin(baud);
        hciCountersFold();
        hci.begin();
        delay(20);                                  // let the receiver settle
        Hci::Reply r;
        const Hci::Error e = hci.run(OP_RESET, nullptr, 0, &r, 400, idleMs);
        CONSOLE.print("bt_baud_try="); CONSOLE.print(baud);
        CONSOLE.print(" st="); CONSOLE.print(e == Hci::OK ? "reset_complete" : Hci::errorName(e));
        printCounters(); CONSOLE.println();
        if (e == Hci::OK) { s_baudFound = baud; break; }
    }
    if (s_baudFound) { CONSOLE.print("bt_baud="); CONSOLE.println(s_baudFound); }
    else {
        CONSOLE.print("bt_baud=none tried="); CONSOLE.println(BT_SWEEP_N);
        hciIo.end(); hciIo.begin(115200);           // leave the port as we found it
        hciCountersFold();
        hci.begin();
    }
}

void setup() {
    CONSOLE.begin(115200);
    delay(50);
    #if defined(ARDUINO_MIMXRT1060_EVKB)
    CONSOLE.println("RT1062 M.2 HCI probe up (MIMXRT1060-EVKB, LPUART3)");
#else
    CONSOLE.println("RT1176 M.2 HCI probe up");
#endif

    // ★ ORDER IS LOAD-BEARING, and getting it wrong is silent.
    // The BT core greets ONCE per power-up -- it sent its V3 start indication
    // three times in ~200 ms on the bench and then went quiet for good.  So
    // Serial2 must be LISTENING BEFORE the card is powered up, and the download
    // must run BEFORE the SDIO work, which takes seconds.
    // Measured 2026-08-24: with begin() after the WLAN download (the obvious
    // order) the loader reported `no_start_indication` on a card that had
    // greeted perfectly well -- the bytes were transmitted before the UART
    // existed.  NXP's own CONFIG_BT_IND_DNLD path has the same shape: power
    // cycle, then the UART download, then everything else.
    // The 1 KB RX ring is what makes this safe: the greeting lands there during
    // the ROM-boot wait inside m2ReleaseWifiReset() and waits for the loader.
#if defined(M2_CONTINUITY_PROBE)
    // ★ BEFORE the transport claims the pad -- this probe re-muxes it to GPIO.
    m2ContinuityProbe();
#endif

    hciIo.begin(115200);
    CONSOLE.println("serial2=up_115200");

    m2ReleaseWifiReset();
    CONSOLE.println("m2_wifi_reset=released");

    // NXP's position for this: inside uart_fw_download(), before the image.
#if defined(M2_BT_WAKE_PULSE)
    m2WakeFromBootSleep();
    #if defined(ARDUINO_MIMXRT1060_EVKB)
    CONSOLE.println("bt_wake=pulsed_10ms_low (GPIO_AD_B0_02, released as input)");
#else
    CONSOLE.println("bt_wake=pulsed_10ms_low (GPIO_DISP_B2_13, mux returned to LPUART2_RTS_B)");
#endif
#else
    CONSOLE.println("bt_wake=off");
#endif

    // ★ CTS IS ASSERTED HERE AND NOT EARLIER, and the ordering is the whole
    // point.  UART_CTSn and UART_RTSn are CONFIGURATION PINS sampled at module
    // reset (MAYA-W1 SIM UBX-21010495 R09 §2.4.5 Table 6: CON[7] = UART_CTSn,
    // CON[8] = UART_RTSn, both "Reserved set to 1"), and their function only
    // becomes the UART's ~1 ms after reset.  Driving CTS LOW across the PDn
    // release therefore latches CON[7] = 0 -- a Reserved configuration.
    // m2ReleaseWifiReset() ends with a 1 s wait, so by here the pins are long
    // since sampled and the module is in its documented "10" configuration.
    // ★ The 2026-08-24 run that "REFUTED" the flow-control hypothesis asserted
    // CTS BEFORE this point, so its "no change whatsoever" cannot tell
    // "flow control is not the problem" apart from "the module booted
    // Reserved".  That refutation is withdrawn; this ordering is what makes a
    // retest mean anything.
#if defined(M2_BT_ASSERT_CTS)
    m2AssertBtCts();
    CONSOLE.println("bt_cts=asserted_after_reset (PHY held in reset -- see the note above)");
#else
    CONSOLE.println("bt_cts=undriven");
#endif

    btFirmwareDownload();

#if M2_HAS_SDIO
    sdio.useIoVoltage1V8(true);
    s_sdioSt = sdio.begin();
    CONSOLE.print("sdio_begin="); CONSOLE.println(statusName(s_sdioSt));
    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        CONSOLE.print("iw416_begin="); CONSOLE.print(statusName(s_iwSt));
        CONSOLE.print(" fw_status=0x"); CONSOLE.println(iw416.fwStatus(), HEX);
        if (s_iwSt == SdioHost::OK) {
#if defined(HAVE_IW416_FW)
            s_fwSt = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
            CONSOLE.print("fw_download="); CONSOLE.println(statusName(s_fwSt));
#else
            s_fwSt = (iw416.fwStatus() == Iw416::FIRMWARE_READY) ? SdioHost::OK : SdioHost::CMD_TIMEOUT;
            CONSOLE.print("fw_download=skipped (no blob supplied) preboot=");
            CONSOLE.println(s_fwSt == SdioHost::OK ? 1 : 0);
#endif
        }
    }
    s_card = (s_fwSt == SdioHost::OK);
    CONSOLE.print("card="); CONSOLE.println(s_card ? 1 : 0);
#else
    // rt1062: no SDIO port, so there is no Wi-Fi half to run.  Say it plainly.
    CONSOLE.println("sdio_begin=not_built (rt1062: SdioHost is RT1176-only)");
    CONSOLE.println("card=0");
#endif

    // ★ THE QUESTION THIS ANSWERS: when the SDIO firmware comes up -- the
    // COMBO image, which u-blox say carries the Bluetooth core too -- does
    // anything appear on the BT UART?  A booting controller that announced
    // itself, or re-greeted from ROM, or emitted anything at all, would show
    // here.  Run on BOTH firmware paths so the two are comparable.
    m2DumpSerial2("bt_uart_postsdio:", 500);

    // The HCI sequence runs WHATEVER the SDIO outcome: on silicon the BT block
    // only answers after the combo download (B0), and the card-absent gate
    // wants the timeout path by name; the [hci] gate's fake controller answers
    // regardless of SDIO.  NXP waits 100 ms + up to 260 ms here.
    hci.begin();
    pump.attach(hci);

    // Reset: up to 10 attempts, because silicon needs an unknown settle after
    // the download (B0 measures it).  In QEMU the [hci] gate's `-serial
    // unix:...,server` holds the guest until the peer is connected, so there
    // attempts>1 is a driver finding, not a timing one.  attempts= is printed
    // so neither is hidden.
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
#if defined(M2_BT_LOOPBACK)
        probeLoopback();
#endif
        probeInquiry();
#if defined(M2_BT_CONNECT)
        probeConnect();
#endif
    } else if (s_hciSt == Hci::TIMEOUT) {
        CONSOLE.print("hci_reset=timeout reason=no_response attempts=10");
        printCounters(); CONSOLE.println();
    } else {
        CONSOLE.print("hci_reset=fail reason="); CONSOLE.print(Hci::errorName(s_hciSt));
        CONSOLE.print(" attempts=10"); printCounters(); CONSOLE.println();
    }
    // ★ ESCALATION -- 115200 is the BOOT ROM's rate, not necessarily the
    // running firmware's.  u-blox attach this module's controller at 3 Mbaud
    // (SIM UBX-21010495 R09 §4.4.6: `hciattach ... any 3000000 flow`, after
    // §4.4.3's combo image has gone over SDIO).  Every probe this tree ran
    // before 2026-08-25 used 115200 and saw silence -- which is what a
    // controller listening at another rate looks like, since it decodes
    // nothing and so answers nothing.  If 115200 failed, ask the other rates
    // before concluding the controller is dead.
    if (s_hciSt != Hci::OK) {
        btBaudSweep();
        if (s_baudFound) {
            CONSOLE.print("hci_reset=ok_after_baud_change baud="); CONSOLE.print(s_baudFound);
            printCounters(); CONSOLE.println();
            s_hciSt = Hci::OK;
            probeIdentity();
            probeInquiry();
        }
    }
    CONSOLE.println("hci_probe_done");
}

void loop() {
    static uint32_t n = 0;
    // Heartbeat: proves the image is still running after the probe rather than
    // having wedged in it, and carries the transport's accounting.
    CONSOLE.print("hb card="); CONSOLE.print(s_card ? 1 : 0);
    CONSOLE.print(" btfw="); CONSOLE.print(BtFwLoader::errorName(s_btFwSt));
    CONSOLE.print(" hci="); CONSOLE.print(s_hciSt == Hci::OK ? "ok" : Hci::errorName(s_hciSt));
    CONSOLE.print(" n="); CONSOLE.print(n++);
    CONSOLE.print(" pump="); CONSOLE.print(pump.passes());
#if defined(M2_BT_CONNECT)
    // Phase-2 outcome, echoed every heartbeat so any capture shows it.
    CONSOLE.print(" conn="); CONSOLE.print(s_p2link);
    CONSOLE.print(" sec="); CONSOLE.print(s_p2sec); CONSOLE.print("/"); CONSOLE.print(s_p2pair);
    CONSOLE.print(" avdtp="); CONSOLE.print(s_p2avdtp); CONSOLE.print(" mtu="); CONSOLE.print(s_p2mtu);
#endif
    printCounters(); CONSOLE.println();
    delay(1000);
}

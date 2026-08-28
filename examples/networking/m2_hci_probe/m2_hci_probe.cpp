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
#if defined(M2_BT_CONNECT)
// B4: ACL connection + SSP Just-Works pairing + encryption (Core 5.2 Vol 4 Part E)
static const uint16_t OP_CREATE_CONNECTION   = 0x0405;
static const uint16_t OP_AUTH_REQUESTED      = 0x0411;
static const uint16_t OP_SET_CONN_ENCRYPTION = 0x0413;
static const uint16_t OP_LINK_KEY_REQ_NEG    = 0x040C;
static const uint16_t OP_IO_CAP_REQ_REPLY    = 0x042B;
static const uint16_t OP_USER_CONF_REQ_REPLY = 0x042C;
static const uint16_t OP_WRITE_SSP_MODE      = 0x0C56;
static const uint16_t OP_SET_EVENT_MASK      = 0x0C01;
static const uint8_t  EV_CONNECTION_COMPLETE = 0x03;
static const uint8_t  EV_AUTH_COMPLETE       = 0x06;
static const uint8_t  EV_ENCRYPTION_CHANGE   = 0x08;
static const uint8_t  EV_LINK_KEY_REQUEST    = 0x17;
static const uint8_t  EV_LINK_KEY_NOTIFY     = 0x18;
static const uint8_t  EV_IO_CAP_REQUEST      = 0x31;
static const uint8_t  EV_IO_CAP_RESPONSE     = 0x32;
static const uint8_t  EV_USER_CONF_REQUEST   = 0x33;
static const uint8_t  EV_SIMPLE_PAIRING_DONE = 0x36;
#endif

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
#if defined(M2_BT_CONNECT)
static volatile bool s_connDone = false;   static uint8_t s_connStatus = 0xFF;
static uint16_t      s_connHandle = 0;
static volatile bool s_pairDone = false;   static uint8_t s_pairStatus = 0xFF;
static volatile bool s_authDone = false;   static uint8_t s_authStatus = 0xFF;
static volatile bool s_encDone  = false;   static uint8_t s_encStatus  = 0xFF;
static uint8_t       s_encEnabled = 0;
static uint8_t       s_linkKey[16];        static volatile bool s_haveLinkKey = false;
#endif

static void onEvent(void *, uint8_t code, const uint8_t *p, uint8_t len) {
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
#if defined(M2_BT_CONNECT)
    else if (code == EV_CONNECTION_COMPLETE && len >= 11) {
        // status(1) handle(2) bd(6) link_type(1) encryption_mode(1)
        s_connStatus = p[0]; s_connHandle = (uint16_t)(p[1] | (p[2] << 8));
        CONSOLE.print("conn_complete: status=0x"); printHex8(p[0]);
        CONSOLE.print(" handle=0x"); printHex16(s_connHandle);
        CONSOLE.print(" bd="); printBd(p + 3);
        CONSOLE.print(" link_type="); CONSOLE.println(p[9]);
        s_connDone = true;
    }
    else if (code == EV_LINK_KEY_REQUEST && len >= 6) {
        CONSOLE.print("link_key_req: bd="); printBd(p); CONSOLE.println(" -> neg_reply (no stored key)");
        hci.submit(OP_LINK_KEY_REQ_NEG, p, 6, nullptr, nullptr);
    }
    else if (code == EV_IO_CAP_REQUEST && len >= 6) {
        uint8_t rp[9]; memcpy(rp, p, 6);
        rp[6] = 0x03;   // IO capability = NoInputNoOutput -> Just Works
        rp[7] = 0x00;   // OOB data not present
        rp[8] = 0x04;   // AuthRequirements = General Bonding, MITM not required
        CONSOLE.print("io_cap_req: bd="); printBd(p); CONSOLE.println(" -> NoInputNoOutput/gen-bonding");
        hci.submit(OP_IO_CAP_REQ_REPLY, rp, 9, nullptr, nullptr);
    }
    else if (code == EV_IO_CAP_RESPONSE && len >= 9) {
        CONSOLE.print("io_cap_rsp: bd="); printBd(p);
        CONSOLE.print(" io_cap="); CONSOLE.print(p[6]);
        CONSOLE.print(" auth="); CONSOLE.println(p[8]);
    }
    else if (code == EV_USER_CONF_REQUEST && len >= 10) {
        uint32_t nv = (uint32_t)p[6] | ((uint32_t)p[7] << 8) | ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
        CONSOLE.print("user_conf_req: bd="); printBd(p);
        CONSOLE.print(" numeric="); CONSOLE.print(nv); CONSOLE.println(" -> accept (Just Works)");
        hci.submit(OP_USER_CONF_REQ_REPLY, p, 6, nullptr, nullptr);
    }
    else if (code == EV_SIMPLE_PAIRING_DONE && len >= 7) {
        s_pairStatus = p[0];
        CONSOLE.print("pairing_complete: status=0x"); printHex8(p[0]);
        CONSOLE.print(" bd="); printBd(p + 1); CONSOLE.println();
        s_pairDone = true;
    }
    else if (code == EV_LINK_KEY_NOTIFY && len >= 23) {
        memcpy(s_linkKey, p + 6, 16); s_haveLinkKey = true;
        CONSOLE.print("link_key: bd="); printBd(p); CONSOLE.print(" type="); CONSOLE.print(p[22]);
        CONSOLE.print(" key="); for (int i = 0; i < 16; i++) printHex8(s_linkKey[i]); CONSOLE.println();
    }
    else if (code == EV_AUTH_COMPLETE && len >= 3) {
        s_authStatus = p[0]; s_authDone = true;
        CONSOLE.print("auth_complete: status=0x"); printHex8(p[0]);
        CONSOLE.print(" handle=0x"); printHex16((uint16_t)(p[1] | (p[2] << 8))); CONSOLE.println();
    }
    else if (code == EV_ENCRYPTION_CHANGE && len >= 4) {
        s_encStatus = p[0]; s_encEnabled = p[3];
        CONSOLE.print("encryption_change: status=0x"); printHex8(p[0]);
        CONSOLE.print(" handle=0x"); printHex16((uint16_t)(p[1] | (p[2] << 8)));
        CONSOLE.print(" enabled="); CONSOLE.println(p[3]);
        s_encDone = true;
    }
#endif
    else {
        CONSOLE.print("hci_event: code=0x"); printHex8(code); CONSOLE.print(" len="); CONSOLE.println(len);
    }
}

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
        CONSOLE.print("hci_buffer: acl_len="); CONSOLE.print(aclLen);
        CONSOLE.print(" acl_num="); CONSOLE.print(r.params[3] | (r.params[4] << 8));
        CONSOLE.print(" sco_len="); CONSOLE.print(r.params[2]);
        CONSOLE.print(" sco_num="); CONSOLE.println(r.params[5] | (r.params[6] << 8));
        hci.setAclMax(aclLen);           // the parser's plausibility bound becomes the card's word
    } else printFail("hci_buffer", e, r, "short_reply");
}

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
// --- B4: connect + SSP Just-Works pairing + encryption to a discovered device ---
// The SSP events (link-key/IO-capability/user-confirmation) are unsolicited, so
// onEvent() answers them via submit() while this flow waits on the state flags --
// the same pump-driven pattern probeInquiry() uses.  Un-fakeable assertion:
// Encryption_Change status=0x00 enabled=1, plus the headset's own "connected".
static void probeConnect() {
    int t = -1;                                     // first inquiry hit that is Audio/Video (major class 4)
    for (uint8_t i = 0; i < s_foundN; i++)
        if (((s_found[i].r.cod >> 8) & 0x1F) == 0x04) { t = i; break; }
    if (t < 0) { CONSOLE.println("connect=skipped (no A/V device in inquiry)"); return; }
    const Found &d = s_found[t];
    CONSOLE.print("connect: target="); printBd(d.r.bd);
    CONSOLE.print(" name=\""); CONSOLE.print(d.named ? d.name.name : "?"); CONSOLE.println("\"");

    Hci::Reply r;
    // Enable ALL HCI events, incl. the SSP request events (0x31-0x36) which sit
    // ABOVE the post-Reset default mask.  Without this the controller cannot ask
    // the host to run Simple Pairing, and Authentication_Requested fails 0x05.
    uint8_t evmask[8]; memset(evmask, 0xFF, sizeof evmask);
    Hci::Error me = hci.run(OP_SET_EVENT_MASK, evmask, sizeof evmask, &r, 1000, idleMs);
    CONSOLE.print("event_mask: st="); CONSOLE.print(me == Hci::OK ? "ok" : Hci::errorName(me));
    CONSOLE.print(" status=0x"); printHex8(r.status); CONSOLE.println();
    uint8_t sspOn = 0x01;
    Hci::Error we = hci.run(OP_WRITE_SSP_MODE, &sspOn, 1, &r, 1000, idleMs);   // enable host SSP support
    CONSOLE.print("ssp_mode: st="); CONSOLE.print(we == Hci::OK ? "ok" : Hci::errorName(we));
    CONSOLE.print(" status=0x"); printHex8(r.status); CONSOLE.println();

    // Create_Connection: bd(6) pkt_type(2)=0xCC18 psrm(1) reserved(1) clk(2,bit15=valid) role_switch(1)
    uint8_t p[13];
    memcpy(p, d.r.bd, 6);
    p[6] = 0x18; p[7] = 0xCC;
    p[8] = d.r.psrm; p[9] = 0x00;
    p[10] = (uint8_t)(d.r.clockOffset & 0xFF);
    p[11] = (uint8_t)((d.r.clockOffset >> 8) | 0x80);
    p[12] = 0x01;
    s_connDone = false; s_connStatus = 0xFF;
    Hci::Error e = hci.run(OP_CREATE_CONNECTION, p, sizeof p, &r, 2000, idleMs);
    if (e != Hci::OK || !r.statusEvent) { printFail("connect", e, r, "not_command_status"); return; }
    uint32_t t0 = millis();
    while (!s_connDone && millis() - t0 < 15000) delay(10);
    if (!s_connDone)          { CONSOLE.println("connect=timeout (no Connection_Complete)"); return; }
    if (s_connStatus != 0x00) { CONSOLE.print("connect=fail status=0x"); printHex8(s_connStatus); CONSOLE.println(); return; }
    CONSOLE.print("connect=ok handle=0x"); printHex16(s_connHandle); CONSOLE.println();

    // Authentication_Requested -> Link_Key_Request(neg) -> SSP -> Link_Key_Notification
    //   -> Authentication_Complete.  Encryption needs the link AUTHENTICATED, so
    //   wait for Auth_Complete (which follows Simple_Pairing_Complete + the link
    //   key), NOT just Simple_Pairing_Complete -- else Set_Connection_Encryption
    //   races ahead and returns 0x2F Insufficient Security.
    s_pairDone = false; s_authDone = false; s_haveLinkKey = false;
    uint8_t hp[2] = { (uint8_t)(s_connHandle & 0xFF), (uint8_t)(s_connHandle >> 8) };
    hci.run(OP_AUTH_REQUESTED, hp, 2, &r, 2000, idleMs);
    t0 = millis();
    while (!s_authDone && millis() - t0 < 25000) delay(10);
    CONSOLE.print("pairing=");  CONSOLE.print(s_pairDone && s_pairStatus == 0x00 ? "ok" : "incomplete");
    CONSOLE.print(" auth=");    CONSOLE.print(s_authDone && s_authStatus == 0x00 ? "ok" : "fail/timeout");
    CONSOLE.print(" link_key="); CONSOLE.println(s_haveLinkKey ? "stored" : "none");
    if (!s_authDone || s_authStatus != 0x00) { CONSOLE.println("connect_secure=fail (auth incomplete)"); return; }

    // Set_Connection_Encryption -> Encryption_Change (the un-fakeable B4 assertion)
    s_encDone = false;
    uint8_t ep[3] = { (uint8_t)(s_connHandle & 0xFF), (uint8_t)(s_connHandle >> 8), 0x01 };
    hci.run(OP_SET_CONN_ENCRYPTION, ep, 3, &r, 2000, idleMs);
    t0 = millis();
    while (!s_encDone && millis() - t0 < 10000) delay(10);
    if (s_encDone && s_encStatus == 0x00 && s_encEnabled)
        CONSOLE.println("connect_secure=ok encryption=on (B4 DONE)");
    else {
        CONSOLE.print("connect_secure=fail status=0x"); printHex8(s_encDone ? s_encStatus : 0xFF);
        CONSOLE.print(" enabled="); CONSOLE.println(s_encEnabled);
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
    printCounters(); CONSOLE.println();
    delay(1000);
}

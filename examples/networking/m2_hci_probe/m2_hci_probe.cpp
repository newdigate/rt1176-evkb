// m2_hci_probe -- BT-1 of the M.2 Bluetooth programme: does the IW416 answer
// HCI, and what does it say it is?
//
// Sequence: board preamble -> SDIO enumerate -> combo blob download (if
// supplied) -> HCI over Serial2 at 115200:
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
#include <SdioHost.h>
#include <SdioFunc.h>
#include <Iw416.h>
#include <Hci.h>
#include <HciEvents.h>
#include <HciTransport.h>
#include <HciPump.h>

static SdioHost sdio;
static SdioFunc func(sdio);
static Iw416 iw416(sdio, func);
static HciTransport hciIo(Serial2);
static Hci hci(hciIo);
static HciPump pump;

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static SdioHost::Status s_sdioSt = SdioHost::CMD5_NO_RESPONSE;
static SdioHost::Status s_iwSt   = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_fwSt   = SdioHost::CMD_TIMEOUT;
static bool       s_card  = false;            // firmware confirmed running on the card
static Hci::Error s_hciSt = Hci::TIMEOUT;     // outcome of the Reset step

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

// --- board preamble -- copied from m2_uap_probe (and WiFi.cpp); keep in step --
// Release SDIO_RST (GPIO_AD_16 = GPIO9.15) then WL_RST/PDn (GPIO_AD_31 =
// GPIO9.30, reaching PDn via the hand-bridged R404), with the 1 s ROM-boot
// wait PDn requires.  Without it the card stays in power-down.
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30

static void m2ReleaseWifiReset() {
    M2_SDIO_RST_MUX = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO15
    M2_WL_RST_MUX   = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO30
    GPIO9_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    GPIO9_DR_SET = (1u << M2_SDIO_RST_BIT);         // SDIO_RST high
    delay(100);
    GPIO9_DR_SET = (1u << M2_WL_RST_BIT);           // then WL_RST / PDn high
    delay(1000);                                    // PDn exit needs ROM boot time
}

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

static void printHex8(uint8_t v)   { if (v < 0x10) Serial1.print('0'); Serial1.print(v, HEX); }
static void printHex16(uint16_t v) { printHex8((uint8_t)(v >> 8)); printHex8((uint8_t)v); }
static void printHex24(uint32_t v) { printHex8((uint8_t)(v >> 16)); printHex16((uint16_t)v); }
static void printBd(const uint8_t *bd) { char s[18]; hciFormatBd(bd, s); Serial1.print(s); }

static void printCounters() {
    Serial1.print(" timeouts="); Serial1.print(hci.timeouts());
    Serial1.print(" framing=");  Serial1.print(hci.framing());
    Serial1.print(" starved=");  Serial1.print(hci.starved());
    Serial1.print(" qfull=");    Serial1.print(hci.queueFull());
    Serial1.print(" late=");     Serial1.print(hci.late());
}
static void printFail(const char *what, Hci::Error e, const Hci::Reply &r, const char *alt) {
    Serial1.print(what); Serial1.print("=fail reason=");
    Serial1.print(e == Hci::OK ? alt : Hci::errorName(e));
    Serial1.print(" status=0x"); printHex8(r.status);
    printCounters(); Serial1.println();
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

static void onEvent(void *, uint8_t code, const uint8_t *p, uint8_t len) {
    if (code == EV_INQUIRY_RESULT) {
        uint8_t n = hciInquiryResultCount(p, len);
        for (uint8_t i = 0; i < n && s_foundN < 8; i++) {
            Found &f = s_found[s_foundN];
            if (!hciParseInquiryResult(p, len, i, &f.r)) break;
            f.named = false; s_foundN++;
            Serial1.print("inq: bd="); printBd(f.r.bd);
            Serial1.print(" cod=0x"); printHex24(f.r.cod);
            Serial1.print(" psrm="); Serial1.print(f.r.psrm);
            Serial1.print(" clk=0x"); printHex16(f.r.clockOffset);
            Serial1.println();
        }
        if (n == 0) { Serial1.print("inq: malformed len="); Serial1.println(len); }
    } else if (code == EV_INQUIRY_COMPLETE && len >= 1) {
        s_inqStatus = p[0]; s_inqDone = true;
    } else if (code == EV_REMOTE_NAME_DONE) {
        HciRemoteName nm;
        if (hciParseRemoteNameComplete(p, len, &nm)) {
            for (uint8_t i = 0; i < s_foundN; i++)
                if (memcmp(s_found[i].r.bd, nm.bd, 6) == 0) { s_found[i].name = nm; s_found[i].named = true; }
        }
        s_nameDone = true;
    } else {
        Serial1.print("hci_event: code=0x"); printHex8(code); Serial1.print(" len="); Serial1.println(len);
    }
}

// --- B1: identity ---------------------------------------------------------------
static void probeIdentity() {
    Hci::Reply r;
    Hci::Error e = hci.run(OP_READ_LOCAL_VER, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 8) {
        // Return params after status: HCI_Version(1) HCI_Revision(2)
        // LMP_Version(1) Manufacturer_Name(2) LMP_Subversion(2)
        Serial1.print("hci_version: hci_ver="); Serial1.print(r.params[0]);
        Serial1.print(" hci_rev=0x");     printHex16((uint16_t)(r.params[1] | (r.params[2] << 8)));
        Serial1.print(" lmp_ver=");       Serial1.print(r.params[3]);
        Serial1.print(" manufacturer=0x"); printHex16((uint16_t)(r.params[4] | (r.params[5] << 8)));
        Serial1.print(" lmp_subver=0x");  printHex16((uint16_t)(r.params[6] | (r.params[7] << 8)));
        Serial1.println();
    } else printFail("hci_version", e, r, "short_reply");

    e = hci.run(OP_READ_BD_ADDR, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 6) { Serial1.print("bd_addr="); printBd(r.params); Serial1.println(); }
    else printFail("bd_addr", e, r, "short_reply");

    e = hci.run(OP_READ_BUFFER_SIZE, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 7) {
        // ACL_Data_Packet_Length(2) Synchronous_Data_Packet_Length(1)
        // Total_Num_ACL_Data_Packets(2) Total_Num_Synchronous_Data_Packets(2)
        uint16_t aclLen = (uint16_t)(r.params[0] | (r.params[1] << 8));
        Serial1.print("hci_buffer: acl_len="); Serial1.print(aclLen);
        Serial1.print(" acl_num="); Serial1.print(r.params[3] | (r.params[4] << 8));
        Serial1.print(" sco_len="); Serial1.print(r.params[2]);
        Serial1.print(" sco_num="); Serial1.println(r.params[5] | (r.params[6] << 8));
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
    Serial1.println("inquiry=started");
    uint32_t t0 = millis();
    while (!s_inqDone && millis() - t0 < 12000) delay(10);     // events arrive via the pump
    Serial1.print("inquiry_complete: status=0x"); printHex8(s_inqDone ? s_inqStatus : 0xFF);
    Serial1.print(" n="); Serial1.print(s_foundN);
    if (!s_inqDone) Serial1.print(" timeout=1");
    Serial1.println();

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
        Serial1.print("inq_name: bd="); printBd(s_found[i].r.bd);
        if (e != Hci::OK)         { Serial1.print(" fail reason="); Serial1.println(Hci::errorName(e)); continue; }
        if (!s_found[i].named)    { Serial1.println(" fail reason=no_name_event"); continue; }
        Serial1.print(" status=0x"); printHex8(s_found[i].name.status);
        Serial1.print(" name=\""); Serial1.print(s_found[i].name.name); Serial1.println("\"");
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 HCI probe up");

    m2ReleaseWifiReset();
    Serial1.println("m2_wifi_reset=released");
    sdio.useIoVoltage1V8(true);
    s_sdioSt = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.println(statusName(s_sdioSt));
    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        Serial1.print("iw416_begin="); Serial1.print(statusName(s_iwSt));
        Serial1.print(" fw_status=0x"); Serial1.println(iw416.fwStatus(), HEX);
        if (s_iwSt == SdioHost::OK) {
#if defined(HAVE_IW416_FW)
            s_fwSt = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
            Serial1.print("fw_download="); Serial1.println(statusName(s_fwSt));
#else
            s_fwSt = (iw416.fwStatus() == Iw416::FIRMWARE_READY) ? SdioHost::OK : SdioHost::CMD_TIMEOUT;
            Serial1.print("fw_download=skipped (no blob supplied) preboot=");
            Serial1.println(s_fwSt == SdioHost::OK ? 1 : 0);
#endif
        }
    }
    s_card = (s_fwSt == SdioHost::OK);
    Serial1.print("card="); Serial1.println(s_card ? 1 : 0);

    // The HCI sequence runs WHATEVER the SDIO outcome: on silicon the BT block
    // only answers after the combo download (B0), and the card-absent gate
    // wants the timeout path by name; the [hci] gate's fake controller answers
    // regardless of SDIO.  NXP waits 100 ms + up to 260 ms here.
    delay(400);
    hciIo.begin(115200);
    hci.begin();
    pump.attach(hci);
    Serial1.println("serial2=up_115200");

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
        Serial1.print("hci_reset=ok attempts="); Serial1.print(attempts);
        printCounters(); Serial1.println();
        probeIdentity();
        probeInquiry();
    } else if (s_hciSt == Hci::TIMEOUT) {
        Serial1.print("hci_reset=timeout reason=no_response attempts=10");
        printCounters(); Serial1.println();
    } else {
        Serial1.print("hci_reset=fail reason="); Serial1.print(Hci::errorName(s_hciSt));
        Serial1.print(" attempts=10"); printCounters(); Serial1.println();
    }
    Serial1.println("hci_probe_done");
}

void loop() {
    static uint32_t n = 0;
    // Heartbeat: proves the image is still running after the probe rather than
    // having wedged in it, and carries the transport's accounting.
    Serial1.print("hb card="); Serial1.print(s_card ? 1 : 0);
    Serial1.print(" hci="); Serial1.print(s_hciSt == Hci::OK ? "ok" : Hci::errorName(s_hciSt));
    Serial1.print(" n="); Serial1.print(n++);
    Serial1.print(" pump="); Serial1.print(pump.passes());
    printCounters(); Serial1.println();
    delay(1000);
}

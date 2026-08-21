// W17 Phase 0: does THIS IW416 firmware blob carry the uAP (AP-mode) layer?
//
// The M2Radio driver has no AP-mode code at all, and every phase of W17 after
// this one is wasted if the running blob cannot host a BSS.  So before any
// design work: ask the firmware.  This is the W6 precedent -- queryPmk()
// settled the embedded-supplicant question in one afternoon by sending one
// command and reading one result field -- and it reuses sendHostCmd's
// machinery unchanged, with only the BSS-addressing variant added (W17).
//
// WHAT IS ASKED, and why it is asked this way.
//
//   mlan routes a host command to an interface through seq_num's HIGH byte
//   (HostCmd_SET_SEQ_NO_BSS_INFO: bss_num 11:8, bss_type 15:12).  The AP
//   commands live on bss_type=1.  Sending an APCMD on bss_type=0 and reading
//   back an error would therefore prove NOTHING about AP support -- it could
//   just as well be the STA command path refusing a command that is not its
//   own.  Every probe below is sent on BOTH interfaces for exactly that
//   reason.
//
//   ★ CONTROLS AT BOTH ENDS.  A result code is only interpretable against
//   what this firmware does with a command it definitely HAS and a command it
//   definitely does NOT.  So the matrix brackets the APCMDs with:
//       positive control  GET_HW_SPEC 0x0003  -- known-good, answered all
//                                                 through W3-W16
//       negative control  0x7FFE              -- reserved/undefined; nothing
//                                                 in mlan_fw.h claims it
//   Without those two rows, `result=1` from 0x00B0 is a number, not evidence.
//   (This project has twice believed an unbracketed single reading and had to
//   retract it -- transcript_hw_evkb.txt, W16 wrong turns 1 and 2.)
//
// HOW TO READ THE OUTPUT.  One `uap_probe` line per cell:
//
//   uap_probe cmd=0x00b0 name=SYS_CONFIGURE bss=1 st=OK resp=0x80b0 result=0x0000 len=...
//
//   st=ok           the card answered, and the answer correlated to our request
//   st=cmd-timeout  the card never flagged a command-port upload -- no reply
//                   at all.  On this firmware that is itself a strong signal;
//                   see the negative control's line to know what "no such
//                   command" looks like here.
//   st=bad-cis      packets came but none was our answer (resp=/result= then
//                   describe the LAST packet seen, which may be an event).
//   result=     HostCmd_RESULT_*: 0=OK 1=ERROR 2=NOT_SUPPORT 4=BUSY.
//
//   The verdict line `uap_verdict=` is computed from the matrix, but the raw
//   bytes are dumped too (`uap_bytes`) so a later reader can re-decide.
//
// WHAT IS DELIBERATELY NOT DONE.  BSS_START actually starts beaconing, with
// whatever SSID/channel the firmware defaults to -- RF emission from an
// unconfigured AP.  It is compiled out unless -DM2_UAP_PROBE_BSS_START=1, and
// even then it is followed immediately by BSS_STOP.  The read-only probes
// (SYS_INFO, SYS_CONFIGURE GET, STA_LIST) answer the question on their own.
//
// QEMU GATE.  run_qemu.sh asserts the card-ABSENT path (QEMU's plain SD memory
// card ignores CMD5).  run_qemu_wifi.sh runs the IW416 model, which answers
// every unmodelled command with HostCmd_RESULT_ERROR -- so it exercises the
// probe's NOT-SUPPORTED path end to end, including both controls.  Neither
// gate can say anything about real AP support: only silicon can, and its
// answer lives in transcript_hw_evkb.txt.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "SdioHost.h"
#include "SdioFunc.h"
#include "Iw416.h"

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
static bool s_haveCard = false;
static uint8_t s_mac[6] = {0};

// Same spelling as m2_lwip_test / m2_rx_demo, so a gate or a transcript
// reader that knows one m2_* example knows this one.
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

// --- the probe matrix --------------------------------------------------------
// `body`/`bodyLen` describe what mlan itself sends for each command:
// wlan_ops_uap_prepare_cmd emits S_DS_GEN (header only, empty body) for
// SYS_INFO / BSS_STOP / STA_LIST / BSS_START, and SYS_CONFIGURE carries
// action(2) + TLVs, of which a GET with no TLVs is the smallest legal form.
struct Probe {
    uint16_t    cmd;
    const char *name;
    uint8_t     body;         // Body, below
    bool        rfEmitting;   // compiled out unless explicitly enabled
    uint8_t     kind;         // Kind, below
    // Run this row on the uAP interface ONLY.  BSS_START/BSS_STOP are
    // meaningless addressed to the STA interface -- mlan sends them on the uAP
    // priv -- and sending a start to bss_type=0 would be asking the firmware to
    // do something undefined while an AP is coming up.  Trailing member so the
    // rows above value-initialise it to false.
    bool        uapOnly;
};
// CTL_POS: a command this firmware definitely has.  CTL_NEG: an id nothing
// defines.  AP: the rows the whole example exists to read.  Only AP rows feed
// the tally and the verdict.
// CTL_UAP is a POSITIVE control on the uAP INTERFACE rather than on the AP
// command family: a generic command, addressed to bss_type=1, that mlan itself
// sends there.  It is deliberately NOT KIND_AP -- it says nothing about whether
// the APCMD handlers exist, so folding it into the tally would inflate the
// Phase 0 verdict with evidence about something else.
enum Kind : uint8_t { KIND_AP = 0, KIND_CTL_POS = 1, KIND_CTL_NEG = 2, KIND_CTL_UAP = 3 };
// BODY_NONE is what mlan emits for SYS_INFO / BSS_STOP / STA_LIST / BSS_START
// (S_DS_GEN and nothing else).  BODY_ACTION is a bare SYS_CONFIGURE GET --
// action(2), no TLVs -- which is ALSO a shape mlan emits (wlan_uap_cmd_sys_
// configure_ext with pioctl_buf and pdata_buf both null sets
// cmd->size = sizeof(HostCmd_DS_SYS_CONFIG) - 1 + S_DS_GEN, i.e. exactly this).
// BODY_ACTION_CHANTLV adds the one TLV mlan pairs with a channel GET,
// TLV_TYPE_UAP_CHAN_BAND_CONFIG 0x012A -- header(4) + band_config(1) +
// channel(1).  The pair exists to bisect ONE variable: if the bare GET wedges
// the command port and the TLV-carrying GET does not, the fault is the empty
// TLV buffer and Phase 1 has its rule.
// BODY_MACADDR_GET is mlan's own GET of a MAC address: action(2) + mac(6),
// cmd->size = sizeof(HostCmd_DS_802_11_MAC_ADDRESS) + S_DS_GEN = 16
// (mlan_glue.c wifi_prepare_get_mac_addr_cmd).
// BODY_SET_MAXSTA is a SET rather than a GET, and that is the point: both
// forms tried so far were GETs, and mlan's own uAP bring-up never starts with
// one -- wlan_start_network SETs the configuration.  action(2)=SET plus
// TLV_TYPE_UAP_MAX_STA_CNT (0x0155) with len=2, exactly the shape
// wlan_uap_cmd_sys_configure_ext emits for a SET.  Non-RF: it caps the client
// count on a BSS that is not started.
// BODY_UAP_CONFIG is the one axis the three earlier shapes did not vary: a
// FULLY POPULATED configuration.  mlan never sends a minimal SYS_CONFIGURE --
// wlan_uap_cmd_sys_configure refuses to build without a configuration at all,
// and wifi_uap_start()'s first uAP command is a full SET -- so "the handler
// wants a populated request" is the standing hypothesis.  Built by the driver
// (Iw416::uapConfigure) rather than here, because it is also Phase 1's
// deliverable, not just this experiment's instrument.
enum Body : uint8_t { BODY_NONE = 0, BODY_ACTION = 1, BODY_ACTION_CHANTLV = 2,
                      BODY_MACADDR_GET = 3, BODY_SET_MAXSTA = 4,
                      BODY_UAP_CONFIG = 5 };

// Open network, and it never reaches the air: BSS_START is what beacons and it
// is compiled out.  No PSK anywhere in this example, deliberately -- an open
// config tests the same axis with nothing to leak.
#ifndef M2_UAP_CONFIG_SSID
#define M2_UAP_CONFIG_SSID "RT1176-UAP-TEST"
#endif
#ifndef M2_UAP_CONFIG_CHANNEL
#define M2_UAP_CONFIG_CHANNEL 6
#endif
// Which TLVs to send. Override to bisect: the boundary between an accepted and
// a rejected set is the finding, if there is one.
#ifndef M2_UAP_CONFIG_MASK
#define M2_UAP_CONFIG_MASK Iw416::UAP_TLV_ALL_OPEN
#endif
static const uint16_t TLV_UAP_MAX_STA_CNT = 0x0155;

// HostCmd_CMD_802_11_MAC_ADDRESS (mlan_fw.h).  Kept local to the example: the
// driver has no use for it, and a probe's constants do not belong in a driver.
static const uint16_t CMD_MAC_ADDRESS = 0x004D;

// ★ THE ORDER IS THE EXPERIMENT, and it was rewritten after the first silicon
// run.  That run put the positive control first and the negative control LAST,
// and got: SYS_INFO answered, then SYS_CONFIGURE, STA_LIST and the negative
// control ALL timed out with no packet of any kind arriving after SYS_INFO.
// Two explanations fit that equally well --
//     (a) this firmware answers a command it does not have with SILENCE, or
//     (b) the first unanswered command wedged the command port, and every
//         later timeout including the control's is an echo of the first.
// -- and the run could not tell them apart, because the control ran after the
// damage.  A single unbracketed reading believed too early is the mistake this
// project has retracted twice (transcript_hw_evkb.txt, W16 wrong turns 1-2).
//
// So: the negative control now runs FIRST, on a port known healthy, which is
// what makes "silence" readable.  A positive control is re-run BETWEEN every
// AP command, which turns "did the port survive that?" into a measurement
// instead of an assumption.  And STA_LIST is asked BEFORE SYS_CONFIGURE --
// STA_LIST is empty-bodied exactly as mlan sends it, while our SYS_CONFIGURE
// GET carries action(2) and no TLVs, which is the likelier of the two to be
// rejected as malformed.  Ordering them this way stops a SYS_CONFIGURE
// problem from being charged to STA_LIST.
static const uint16_t TLV_UAP_CHAN_BAND = 0x012A;   // TLV_TYPE_UAP_CHAN_BAND_CONFIG

static const Probe kProbes[] = {
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.a",  BODY_NONE,   false, KIND_CTL_POS },
    // The negative control on a HEALTHY port: this is the row that defines
    // what "this blob does not have that command" looks like here.
    { 0x7FFE,                         "RSVD.ctl-.a",    BODY_NONE,   false, KIND_CTL_NEG },
    // Did the port survive an unanswered command?  If this row times out, no
    // later row on this run means anything and the verdict says so.
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.b",  BODY_NONE,   false, KIND_CTL_POS },
    { Iw416::CMD_APCMD_SYS_INFO,      "SYS_INFO",       BODY_NONE,   false, KIND_AP      },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.c",  BODY_NONE,   false, KIND_CTL_POS },
    { Iw416::CMD_APCMD_STA_LIST,      "STA_LIST",       BODY_NONE,   false, KIND_AP      },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.d",  BODY_NONE,   false, KIND_CTL_POS },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.e",  BODY_NONE,   false, KIND_CTL_POS },
    // --- W17 FAULT 1 rows: what the first silicon runs could NOT separate ---
    // Every row that ANSWERED above is empty-bodied, and both rows that wedged
    // the port carry a body.  "SYS_CONFIGURE is the killer" and "a bodied
    // command on this port is the killer" fit that evidence equally well, and
    // the W16 rule says a single uncontrolled pair is not a finding.  These two
    // rows separate them, cheapest first.
    //
    // RSVD.body: the reserved id again, now carrying action(2).  The firmware
    // rejects it on the id, BEFORE any body parsing, so an answer here proves
    // exactly one thing -- that a 10-byte bodied command reaches the parser and
    // is replied to on this port, on both interfaces.  It deliberately does not
    // claim the body was UNDERSTOOD; that is the next row's job.
    { 0x7FFE,                         "RSVD.body",      BODY_ACTION, false, KIND_CTL_NEG },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.e2", BODY_NONE,   false, KIND_CTL_POS },
    // MACADDR.uap: a REAL bodied command addressed to the uAP interface, and
    // not one we invented -- wlan_get_mac_addr_uap() (wifi-sdio.c) sends
    // exactly this, action=GET on priv[MLAN_BSS_TYPE_UAP], as part of mlan's
    // OWN init.  It is the only uAP-addressed command in that whole init path,
    // which makes this row do double duty:
    //   (a) a positive control for "bodied command, bss_type=1, understood";
    //   (b) the leading candidate for the prerequisite FAULT 1 might be about.
    //       It runs BEFORE both SYS_CONFIGURE rows.  If SYS_CONFIGURE stops
    //       wedging now, the missing step was this; if it still wedges, this
    //       candidate is eliminated rather than left hanging.
    { CMD_MAC_ADDRESS,                "MACADDR.uap",    BODY_MACADDR_GET, false, KIND_CTL_UAP },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.e3", BODY_NONE,   false, KIND_CTL_POS },
#if defined(M2_UAP_CONFIGURE)
    // FIRST of the SYS_CONFIGURE family, because only the first one of a run is
    // evidence -- it takes the port with it.  Everything below it is expected
    // to be unbracketed when the hypothesis is wrong.
    { Iw416::CMD_APCMD_SYS_CONFIGURE, "SYSCFG.fullopen", BODY_UAP_CONFIG, false, KIND_AP },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.e5", BODY_NONE,   false, KIND_CTL_POS },
#endif
    // ★ SYS_CONFIGURE IS DELIBERATELY LAST OF THE AP ROWS. On silicon
    // (2026-08-20) this exact request -- action=GET, no TLVs -- gets no reply
    // AND takes the command port with it: every command after it, positive
    // control and negative control alike, timed out for the rest of the run.
    // Ordering it last confines that damage to the two trailing controls, so
    // one destructive command cannot cost the reading of every other row. The
    // bracketing rule below is what turns the wedge from a spoiled run into a
    // measurement.
    // The TLV-carrying GET goes FIRST of the two, because the bare one is the
    // known port-killer and would otherwise take this row's reading with it.
#if defined(M2_UAP_SYSCFG_SET_FIRST)
    // Opt-in variant row.  Because the FIRST SYS_CONFIGURE of a run is the only
    // one whose reading survives -- it takes the port with it and every later
    // row is unbracketed -- a variant can only be tested by being put first,
    // one variant per firmware life.  OFF by default so the gated build keeps
    // the matrix the QEMU gates assert.
    { Iw416::CMD_APCMD_SYS_CONFIGURE, "SYSCFG.setmaxsta", BODY_SET_MAXSTA, false, KIND_AP },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.e4", BODY_NONE,   false, KIND_CTL_POS },
#endif
#if !defined(M2_UAP_PROBE_BSS_START)
    // ★ COMPILED OUT when BSS_START is enabled, and this is load-bearing rather
    // than tidy: these two rows are the KNOWN port-killers.  Left in, they would
    // wedge the command port before BSS_START could ever be sent, and the run
    // would report a dead port as if starting the BSS had killed it -- charging
    // one command's fault to another, which is exactly the misreading the
    // bracketing rule exists to prevent.
    { Iw416::CMD_APCMD_SYS_CONFIGURE, "SYSCFG.chantlv", BODY_ACTION_CHANTLV, false, KIND_AP },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.f",  BODY_NONE,   false, KIND_CTL_POS },
    { Iw416::CMD_APCMD_SYS_CONFIGURE, "SYSCFG.bare",    BODY_ACTION, false, KIND_AP      },
    { Iw416::CMD_GET_HW_SPEC,         "HWSPEC.ctl+.g",  BODY_NONE,   false, KIND_CTL_POS },
#endif
    // The negative control again, at the other end.
    { 0x7FFE,                         "RSVD.ctl-.b",    BODY_NONE,   false, KIND_CTL_NEG },
};
static const uint8_t kProbeCount = (uint8_t)(sizeof(kProbes) / sizeof(kProbes[0]));

// One cell of the matrix, kept so the verdict is computed from recorded
// evidence rather than from whatever the last print happened to say.
struct Cell {
    SdioHost::Status st;
    uint16_t respCmd;
    uint16_t respResult;
    uint16_t respLen;
    bool     ran;
};
static Cell s_cells[kProbeCount][2];   // [probe][bssType 0|1]

// Print::print(x, HEX) drops leading zeros and prints upper case, so every
// hex field in this example goes through one of these two -- a gate matching
// `cmd=0x00b0` against a line reading `cmd=0xB0` is a gate that fails on a
// healthy run.
static void printHex16(uint16_t v) {
    if (v < 0x1000) Serial1.print('0');
    if (v < 0x0100) Serial1.print('0');
    if (v < 0x0010) Serial1.print('0');
    Serial1.print(v, HEX);
}

static void dumpBytes(const uint8_t *p, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        if (p[i] < 0x10) Serial1.print('0');
        Serial1.print(p[i], HEX);
    }
}

// --- card-state autopsy ------------------------------------------------------
// The single thing the first FAULT 1 runs could not say: after SYS_CONFIGURE
// kills the command port, is the FIRMWARE dead or is only the command layer
// wedged?  Nothing in a timeout distinguishes those, and they lead to opposite
// Phase 1 designs -- one needs a card reset in the recovery path, the other
// needs a different command.
//
// These are plain CMD52 reads of card state, so they are answered by the SDIO
// interface itself and do not depend on the command port at all.
//
// ★ HOST_INT_STATUS (0x0C) is READ ONLY IN THE FINAL DUMP, and that is not an
// oversight.  begin() configures HOST_INT_RSR so the register CLEARS ON READ:
// sampling it mid-matrix would consume an interrupt the driver is waiting for
// and perturb the very thing being measured.  intStatusSeen() -- the driver's
// accumulated union, printed on every cell -- is the non-destructive reading,
// and it is what the per-cell lines already carry.
//
// ★ The `init` dump is what makes the later ones readable.  W16's lesson was
// that a reference value read in a DIFFERENT PHASE of the card's life is not a
// constant (reg 0x5C answers 0x0D to the boot ROM and 0x40 to running
// firmware), so "0xFEDC means alive" is not assumed here -- the baseline is
// measured on this card, in this phase, in this run, and the comparison is
// against that.
static void dumpCardRegs(const char *tag, bool includeIntStatus) {
    uint8_t cs = 0, him = 0, f0 = 0, f1 = 0, p0 = 0, p1 = 0, p2 = 0, his = 0;
    bool ok = true;
    ok &= (sdio.cmd52Read(1, Iw416::CARD_STATUS_REG,   &cs)  == SdioHost::OK);
    ok &= (sdio.cmd52Read(1, Iw416::HOST_INT_MASK_REG, &him) == SdioHost::OK);
    ok &= (sdio.cmd52Read(1, Iw416::CARD_FW_STATUS0,   &f0)  == SdioHost::OK);
    ok &= (sdio.cmd52Read(1, Iw416::CARD_FW_STATUS1,   &f1)  == SdioHost::OK);
    ok &= (sdio.cmd52Read(1, Iw416::IO_PORT_0_REG,     &p0)  == SdioHost::OK);
    ok &= (sdio.cmd52Read(1, Iw416::IO_PORT_0_REG + 1, &p1)  == SdioHost::OK);
    ok &= (sdio.cmd52Read(1, Iw416::IO_PORT_0_REG + 2, &p2)  == SdioHost::OK);
    if (includeIntStatus) {
        ok &= (sdio.cmd52Read(1, Iw416::HOST_INT_STATUS, &his) == SdioHost::OK);
    }
    Serial1.print("uap_cardreg tag="); Serial1.print(tag);
    Serial1.print(" cmd52=");          Serial1.print(ok ? "ok" : "FAILED");
    Serial1.print(" cardstatus=0x");   printHex16(cs);
    Serial1.print(" intmask=0x");      printHex16(him);
    Serial1.print(" fwstatus=0x");     printHex16((uint16_t)(((uint16_t)f1 << 8) | f0));
    Serial1.print(" ioport=0x");       printHex16((uint16_t)(((uint16_t)p1 << 8) | p0));
    Serial1.print(",0x");              printHex16(p2);
    if (includeIntStatus) { Serial1.print(" intstatus=0x"); printHex16(his); }
    Serial1.println();
}

// Set once, by the first cell that times out, so the autopsy is taken while
// the damage is fresh rather than after fifteen more seconds of commands.
static bool s_autopsyTaken = false;

static void runCell(uint8_t pi, uint8_t bssType) {
    const Probe &pr = kProbes[pi];
    Cell &c = s_cells[pi][bssType];
    c.ran = true;

    static uint8_t rx[Iw416::SDIO_BLOCK_SIZE * 4];
    uint8_t  body[8];
    uint16_t bodyLen = 0;
    uint16_t rxLen = 0;

    if (pr.body != BODY_NONE) {
        body[bodyLen++] = 0x00; body[bodyLen++] = 0x00;      // HostCmd_ACT_GET, LE
    }
    if (pr.body == BODY_MACADDR_GET) {
        for (uint8_t k = 0; k < 6; k++) body[bodyLen++] = 0x00;   // mac_addr, GET
    }
    if (pr.body == BODY_SET_MAXSTA) {
        body[0] = 0x01; body[1] = 0x00;                      // HostCmd_ACT_GEN_SET
        body[bodyLen++] = (uint8_t)(TLV_UAP_MAX_STA_CNT & 0xFF);
        body[bodyLen++] = (uint8_t)(TLV_UAP_MAX_STA_CNT >> 8);
        body[bodyLen++] = 0x02; body[bodyLen++] = 0x00;      // len, as mlan sets it for a SET
        body[bodyLen++] = 0x04; body[bodyLen++] = 0x00;      // max_sta_count = 4
    }
    if (pr.body == BODY_ACTION_CHANTLV) {
        // MrvlIEtypes_channel_band_t: type(2) len(2) band_config(1) channel(1),
        // len counting only the payload -- 2, as mlan computes it
        // (sizeof(struct) - sizeof(header)).
        body[bodyLen++] = (uint8_t)(TLV_UAP_CHAN_BAND & 0xFF);
        body[bodyLen++] = (uint8_t)(TLV_UAP_CHAN_BAND >> 8);
        body[bodyLen++] = 0x02; body[bodyLen++] = 0x00;
        body[bodyLen++] = 0x00;                              // band_config: manual, 20 MHz, 2.4 GHz
        body[bodyLen++] = 0x00;                              // channel: GET, so unset
    }

    memset(rx, 0, sizeof(rx));
    // ★ CARD_TO_HOST_EVENT (0x5C) sampled either side of every cell.  Run 1
    // took ONE healthy reading and one wedged reading and saw 0x08 -> 0x88,
    // bit 7 = DN_LD_CP_RDY.  That is not yet a finding: a register sampled
    // once per phase cannot say whether it OSCILLATES in normal operation, and
    // this project has retracted three conclusions built on exactly that
    // mistake.  Sampling every cell turns it into a distribution -- if healthy
    // cells also read 0x88 before a send, bit 7 at the wedge means nothing.
    // Cheap (two CMD52s per cell) and non-destructive: unlike HOST_INT_STATUS,
    // 0x5C does not clear on read.
    uint8_t csPre = 0, csPost = 0;
    (void)sdio.cmd52Read(1, Iw416::CARD_STATUS_REG, &csPre);
    SdioHost::Status s;
    if (pr.body == BODY_UAP_CONFIG) {
        // The driver builds and sends this one, so what is tested is the thing
        // Phase 1 will actually ship rather than a probe-local imitation of it.
        // bss is forced to the uAP interface inside uapConfigure(), so the
        // bss=0 column of this row is a repeat of bss=1, not a STA request --
        // said here because the printed `bss=` would otherwise mislead.
        Iw416::UapConfig cfg;
        cfg.ssid         = M2_UAP_CONFIG_SSID;
        cfg.channel      = M2_UAP_CONFIG_CHANNEL;
        cfg.mac          = s_mac;
        cfg.beaconPeriod = 100;
        cfg.dtimPeriod   = 1;
        cfg.bcastSsidCtl = 1;
        cfg.tlvMask      = (uint16_t)(M2_UAP_CONFIG_MASK);
        Serial1.print("uap_cfg_req mask=0x"); printHex16(cfg.tlvMask);
        Serial1.print(" ssid="); Serial1.print(cfg.ssid);
        Serial1.print(" chan=");  Serial1.println((int)cfg.channel);
        s = iw416.uapConfigure(cfg);
        rxLen = 0;
        Serial1.print("uap_cfg_bytes len="); Serial1.print(iw416.uapCfgReqLen());
        Serial1.print(" ");
        dumpBytes(iw416.uapCfgReq(), iw416.uapCfgReqLen());
        Serial1.println();
    } else {
        s = iw416.sendHostCmdBss(pr.cmd, bodyLen ? body : nullptr,
                                 bodyLen, bssType, 0);
        if (s == SdioHost::OK) {
            s = iw416.waitCmdResp(pr.cmd, rx, sizeof(rx), &rxLen, 2000);
        }
    }
    (void)sdio.cmd52Read(1, Iw416::CARD_STATUS_REG, &csPost);
    c.st         = s;
    c.respCmd    = iw416.lastRespCmd();
    c.respResult = iw416.lastRespResult();
    c.respLen    = rxLen;

    Serial1.print("uap_probe cmd=0x"); printHex16(pr.cmd);
    Serial1.print(" name=");   Serial1.print(pr.name);
    Serial1.print(" bss=");    Serial1.print((int)bssType);
    Serial1.print(" st=");     Serial1.print(statusName(s));
    Serial1.print(" resp=0x"); printHex16(iw416.lastRespCmd());
    Serial1.print(" type=");   Serial1.print(iw416.lastRespType());
    Serial1.print(" result=0x"); printHex16(iw416.lastRespResult());
    Serial1.print(" len=");    Serial1.print(rxLen);
    Serial1.print(" rdlen=");  Serial1.print(iw416.lastCmdRdLen());
    Serial1.print(" intseen=0x"); Serial1.print(iw416.intStatusSeen(), HEX);
    Serial1.print(" cspre=0x");   printHex16(csPre);
    Serial1.print(" cspost=0x");  printHex16(csPost);
    Serial1.println();

    if (s == SdioHost::CMD_TIMEOUT && !s_autopsyTaken) {
        s_autopsyTaken = true;
        dumpCardRegs("firstfail", false);
    }

    // The reply bytes themselves -- the handoff's success criterion 1 is that
    // the answer is recorded "with the reply bytes quoted", either way.  Dump
    // whatever came back even on a non-OK status: on BAD_CIS the buffer holds
    // the last packet read, which is evidence about what the card said
    // instead.  Capped at 48 bytes: the two headers plus enough body to see a
    // TLV's type/len.
    uint16_t n = rxLen ? rxLen : iw416.lastCmdRdLen();
    if (n > 48) n = 48;
    // BODY_UAP_CONFIG reads its reply into the DRIVER's buffer, not this one,
    // so `rx` here is still zeroed -- dumping it would print 48 zero bytes that
    // a later reader could easily take for the card's answer.  The answer for
    // that row is in the resp=/result= fields of the line above, which come
    // from the driver.
    if (n && s != SdioHost::CMD_TIMEOUT && pr.body != BODY_UAP_CONFIG) {
        Serial1.print("uap_bytes ");
        Serial1.print(pr.name); Serial1.print(".bss"); Serial1.print((int)bssType);
        Serial1.print(" ");
        dumpBytes(rx, n);
        Serial1.println();
    }
}

// The verdict, computed from s_cells.
//
// ★ THE BRACKETING RULE, which the first two silicon runs paid for.  A cell is
// only interpretable if the command port was demonstrably healthy on BOTH
// sides of it -- the nearest positive control BEFORE it answered, and the
// nearest positive control AFTER it answered.  Without that rule the first run
// read a whole column of timeouts as "this firmware answers unknown commands
// with silence", when in fact one command had wedged the port and every later
// timeout, including the control's, was an echo of that one event.  A cell
// that is not bracketed is not evidence, and is reported as such rather than
// folded into a count.
// ★ Health is read from the bss=0 column only, deliberately.  The question
// this answers is "was the COMMAND PORT alive", and the STA-addressed control
// answers it without dragging in the separate question of whether BSS
// addressing works -- if bss=1 alone is broken, that is a driver fault the
// gates assert on directly, and folding it in here would silently turn every
// AP row unbracketed and hide it behind an INVALID verdict instead.
static bool posOkAt(int i) { return i >= 0 && s_cells[i][0].st == SdioHost::OK; }

static void reportVerdict() {
    // Locate the first negative control by KIND rather than by index -- the
    // matrix order has been rewritten twice already and will be again.
    int firstNeg = -1, firstPos = -1;
    for (uint8_t i = 0; i < kProbeCount; i++) {
        if (kProbes[i].kind == KIND_CTL_NEG && firstNeg  < 0) firstNeg  = i;
        if (kProbes[i].kind == KIND_CTL_POS && firstPos  < 0) firstPos  = i;
    }
    if (!posOkAt(firstPos)) {
        Serial1.println("uap_health first_pos=FAILED");
        Serial1.println("uap_verdict=INVALID reason=positive_control_failed_at_start");
        return;
    }
    // The negative control must itself be bracketed, or its signature -- the
    // thing every AP row is compared against -- is not evidence either.
    int negPrev = -1, negNext = -1;
    for (int i = firstNeg - 1; i >= 0; i--) if (kProbes[i].kind == KIND_CTL_POS) { negPrev = i; break; }
    for (int i = firstNeg + 1; i < kProbeCount; i++) if (kProbes[i].kind == KIND_CTL_POS) { negNext = i; break; }
    const bool negBracketed = posOkAt(negPrev) && posOkAt(negNext);
    const Cell &n0 = s_cells[firstNeg][0];
    const Cell &n1 = s_cells[firstNeg][1];
    Serial1.print("uap_control neg_sta_st="); Serial1.print(statusName(n0.st));
    Serial1.print(" neg_sta_result=0x");      printHex16(n0.respResult);
    Serial1.print(" neg_uap_st=");            Serial1.print(statusName(n1.st));
    Serial1.print(" neg_uap_result=0x");      printHex16(n1.respResult);
    Serial1.print(" bracketed=");             Serial1.print(negBracketed ? 1 : 0);
    Serial1.print(" seq_mismatches=");        Serial1.println(iw416.seqMismatches());
    if (!negBracketed) {
        Serial1.println("uap_verdict=INVALID reason=negative_control_not_bracketed");
        return;
    }

    // Every AP cell, classified against that signature.  An AP command counts
    // as SUPPORTED when it behaves DIFFERENTLY from the reserved id -- which
    // includes answering with an error, because an unconfigured BSS_START
    // refusing to start is a real handler giving a real answer.  What is NOT
    // support is being treated exactly like an id nothing defines.
    uint8_t bracketed = 0, distinct = 0, unbracketed = 0;
    for (uint8_t i = 0; i < kProbeCount; i++) {
        if (kProbes[i].kind != KIND_AP) continue;
        int prev = -1, next = -1;
        for (int j = i - 1; j >= 0; j--) if (kProbes[j].kind == KIND_CTL_POS) { prev = j; break; }
        for (int j = i + 1; j < kProbeCount; j++) if (kProbes[j].kind == KIND_CTL_POS) { next = j; break; }
        const bool ok = posOkAt(prev) && posOkAt(next);
        for (uint8_t b = 0; b < 2; b++) {
            const Cell &c = s_cells[i][b];
            if (!c.ran) continue;
            if (!ok) {
                unbracketed++;
                Serial1.print("uap_unbracketed "); Serial1.print(kProbes[i].name);
                Serial1.print(".bss"); Serial1.print((int)b);
                Serial1.print(" st="); Serial1.print(statusName(c.st));
                Serial1.println(" -- the command port was not proven healthy after it; NOT evidence");
                continue;
            }
            bracketed++;
            const Cell &neg = (b == 0) ? n0 : n1;
            const bool sameAsUnknown = (c.st == neg.st) &&
                                       (c.st != SdioHost::OK || c.respResult == neg.respResult);
            if (!sameAsUnknown) distinct++;
        }
    }
    // The uAP-interface controls, reported on their own line.  Kept OUT of the
    // tally above on purpose: these say whether bss_type=1 addressing and
    // bodied commands work, not whether the AP command family exists.
    for (uint8_t i = 0; i < kProbeCount; i++) {
        if (kProbes[i].kind != KIND_CTL_UAP) continue;
        int prev = -1, next = -1;
        for (int j = i - 1; j >= 0; j--) if (kProbes[j].kind == KIND_CTL_POS) { prev = j; break; }
        for (int j = i + 1; j < kProbeCount; j++) if (kProbes[j].kind == KIND_CTL_POS) { next = j; break; }
        Serial1.print("uap_bsscheck ");  Serial1.print(kProbes[i].name);
        Serial1.print(" sta_st=");       Serial1.print(statusName(s_cells[i][0].st));
        Serial1.print(" sta_result=0x"); printHex16(s_cells[i][0].respResult);
        Serial1.print(" uap_st=");       Serial1.print(statusName(s_cells[i][1].st));
        Serial1.print(" uap_result=0x"); printHex16(s_cells[i][1].respResult);
        Serial1.print(" bracketed=");    Serial1.println((posOkAt(prev) && posOkAt(next)) ? 1 : 0);
    }

    Serial1.print("uap_tally bracketed="); Serial1.print((int)bracketed);
    Serial1.print(" distinct_from_neg=");   Serial1.print((int)distinct);
    Serial1.print(" unbracketed=");         Serial1.println((int)unbracketed);

    if (bracketed == 0)   Serial1.println("uap_verdict=INVALID reason=no_bracketed_ap_cells");
    else if (distinct)    Serial1.println("uap_verdict=SUPPORTED");
    else                  Serial1.println("uap_verdict=INDISTINGUISHABLE_FROM_UNKNOWN_CMD");
}

static void m2ReleaseWifiReset();   // defined with the board preamble, below
static bool cmdPortAlive();         // defined with the recovery probe, below

#if defined(M2_UAP_PROBE_BSS_START)
// --- BSS_START: the AP actually goes on air ----------------------------------
// ★ THIS TRANSMITS.  Open network, no upstack behind it (no DHCP server and no
// netif yet, so a client can associate at most and get nothing), on the channel
// the configuration named, and it is stopped again at the end of the sequence.
//
// Ordering is the experiment, as everywhere else in this example: the BSS is
// configured FIRST (SYSCFG.fullopen, the only shape this firmware accepts) and
// only then started.  Starting an unconfigured BSS would beacon with whatever
// the firmware defaults to, which is both less informative and less polite.
//
// The oracle is deliberately NOT our own print.  Three things are asked for:
//   * the firmware's own RESULT for BSS_START,
//   * the card's own EVENT (EVENT_MICRO_AP_BSS_START 0x2E / BSS_ACTIVE 0x44) --
//     un-fakeable by us, since we do not synthesise events,
//   * a positive control afterwards, so "the AP started" is never inferred from
//     a command port that has quietly died.
// The fourth and best oracle is external and belongs to the operator: while the
// hold window runs, ANOTHER DEVICE should see the SSID in a scan.  A radio that
// a second radio can hear is the only proof that really settles it.
static uint32_t s_bssFrames = 0;
static void bssSink(void *, const uint8_t *, uint16_t) { s_bssFrames++; }

#ifndef M2_UAP_BSS_HOLD_MS
#define M2_UAP_BSS_HOLD_MS 60000     // long enough to scan for it from elsewhere
#endif

static void serviceFor(uint32_t ms, const char *tag) {
    bool dropped = false;
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) (void)iw416.serviceLink(bssSink, nullptr, &dropped, 100);
    Serial1.print("uap_bss_service tag="); Serial1.print(tag);
    Serial1.print(" frames=");             Serial1.print((int)s_bssFrames);
    Serial1.print(" lastevent=0x");        printHex16((uint16_t)iw416.lastEvent());
    Serial1.print(" eventinfo=0x");        printHex16((uint16_t)iw416.lastEventInfo());
    Serial1.print(" dropped=");            Serial1.println(dropped ? 1 : 0);
}

static bool oneUapCmd(uint16_t cmd, const char *name) {
    static uint8_t rx[Iw416::SDIO_BLOCK_SIZE * 4];
    uint16_t rxLen = 0;
    uint8_t csPre = 0, csPost = 0;
    (void)sdio.cmd52Read(1, Iw416::CARD_STATUS_REG, &csPre);
    SdioHost::Status s = iw416.sendHostCmdBss(cmd, nullptr, 0, Iw416::BSS_TYPE_UAP, 0);
    if (s == SdioHost::OK) s = iw416.waitCmdResp(cmd, rx, sizeof(rx), &rxLen, 5000);
    (void)sdio.cmd52Read(1, Iw416::CARD_STATUS_REG, &csPost);
    Serial1.print("uap_bss cmd=0x");   printHex16(cmd);
    Serial1.print(" name=");           Serial1.print(name);
    Serial1.print(" st=");             Serial1.print(statusName(s));
    Serial1.print(" result=0x");       printHex16(iw416.lastRespResult());
    Serial1.print(" len=");            Serial1.print(rxLen);
    Serial1.print(" cspre=0x");        printHex16(csPre);
    Serial1.print(" cspost=0x");       printHex16(csPost);
    Serial1.println();
    return s == SdioHost::OK && iw416.lastRespResult() == Iw416::RESULT_OK;
}

static void runBssStartSequence() {
    Serial1.println("uap_bss_seq=begin -- THIS TRANSMITS");
    // The configuration must already have been accepted, or starting is
    // meaningless.  Report it rather than assume it.
    Serial1.print("uap_bss_precheck cfg_ok=");
    Serial1.println(iw416.lastRespResult() == Iw416::RESULT_OK ? 1 : 0);

    const bool started = oneUapCmd(Iw416::CMD_APCMD_BSS_START, "BSS_START");
    serviceFor(3000, "post_start");
    Serial1.print("uap_bss_ctl_after_start=");
    Serial1.println(cmdPortAlive() ? "ok" : "DEAD");

    if (started) {
        Serial1.print("uap_bss=beaconing ssid="); Serial1.print(M2_UAP_CONFIG_SSID);
        Serial1.print(" chan=");                  Serial1.print((int)M2_UAP_CONFIG_CHANNEL);
        Serial1.print(" hold_ms=");               Serial1.println((int)M2_UAP_BSS_HOLD_MS);
        // STA_LIST during the hold is the card's own view of who joined -- the
        // oracle the future join gates will use, exercised here for the first
        // time against a BSS that is actually up.
        serviceFor(M2_UAP_BSS_HOLD_MS, "hold");
        static uint8_t rx[Iw416::SDIO_BLOCK_SIZE];
        uint16_t rxLen = 0;
        if (iw416.sendHostCmdBss(Iw416::CMD_APCMD_STA_LIST, nullptr, 0,
                                 Iw416::BSS_TYPE_UAP, 0) == SdioHost::OK &&
            iw416.waitCmdResp(Iw416::CMD_APCMD_STA_LIST, rx, sizeof(rx), &rxLen, 2000) == SdioHost::OK) {
            Serial1.print("uap_bss_stalist len="); Serial1.print(rxLen);
            Serial1.print(" bytes=");              dumpBytes(rx, rxLen > 24 ? 24 : rxLen);
            Serial1.println();
        } else {
            Serial1.println("uap_bss_stalist=FAILED");
        }
    } else {
        Serial1.println("uap_bss=not_started -- BSS_STOP still sent, so nothing is left on air");
    }

    // ALWAYS, started or not: leaving a radio beaconing because a branch was
    // missed is not a failure mode this example is going to have.
    (void)oneUapCmd(Iw416::CMD_APCMD_BSS_STOP, "BSS_STOP");
    Serial1.print("uap_bss_ctl_after_stop=");
    Serial1.println(cmdPortAlive() ? "ok" : "DEAD");
    Serial1.println("uap_bss_seq=end");
}
#endif  // M2_UAP_PROBE_BSS_START

// Does the command port EVER come back on its own?  The first FAULT 1 runs
// could only say "not within the ~15 s of commands the matrix issues
// afterwards", because nothing waited any longer.  That mattered: if the port
// recovers, Phase 1 can test several SYS_CONFIGURE variants in ONE firmware
// life; if it does not, every variant costs a card reset and the driver needs a
// recovery path before it needs anything else.
//
// Only runs when the port is actually dead -- when the last positive control
// answered, this prints n/a and costs nothing.  That is what keeps it out of
// the QEMU gates' runtime, where nothing wedges.
static const uint8_t  kRecoverTries    = 30;
static const uint32_t kRecoverIntervMs = 1000;


static uint32_t s_drainFrames = 0;
static void drainSink(void *, const uint8_t *, uint16_t) { s_drainFrames++; }

// Ask the command port once, the way the controls do.
static bool cmdPortAlive() {
    uint8_t rx[Iw416::SDIO_BLOCK_SIZE];
    uint16_t rxLen = 0;
    SdioHost::Status s = iw416.sendHostCmdBss(Iw416::CMD_GET_HW_SPEC, nullptr, 0,
                                              Iw416::BSS_TYPE_STA, 0);
    if (s == SdioHost::OK) s = iw416.waitCmdResp(Iw416::CMD_GET_HW_SPEC, rx, sizeof(rx), &rxLen, 2000);
    return s == SdioHost::OK;
}

static void probeRecovery() {
    if (cmdPortAlive()) {
        Serial1.println("uap_recover=n/a reason=command_port_alive_at_end");
        return;
    }
    // ★ THE DATA PORT, BEFORE THE PATIENT RETRY.  A hypothesis that fits every
    // symptom run 1 recorded -- card alive (fwstatus=0xFEDC), ready to accept
    // downloads (DN_LD_CP_RDY set), but never producing another command reply
    // -- is that SYS_CONFIGURE made the firmware post something on the DATA
    // port (an event), and that an UNDRAINED upload holds the command port off.
    // This example never services the data port, so it would never have found
    // out.  Draining here makes it a CAUSAL test rather than another
    // correlation: if the port answers immediately after a drain and not
    // before, the undrained upload IS the mechanism; if the drain finds
    // nothing and changes nothing, that whole family of explanations is dead
    // and Phase 1 stops looking there.
    bool dropped = false;
    s_drainFrames = 0;
    const uint32_t evBefore = iw416.lastEvent();
    for (uint8_t i = 0; i < 30; i++) (void)iw416.serviceLink(drainSink, nullptr, &dropped, 100);
    Serial1.print("uap_drain frames=");   Serial1.print((int)s_drainFrames);
    Serial1.print(" rxdata=");            Serial1.print(iw416.rxDataCount());
    Serial1.print(" dropped=");           Serial1.print(dropped ? 1 : 0);
    Serial1.print(" event_before=0x");    printHex16((uint16_t)evBefore);
    Serial1.print(" event_after=0x");     printHex16((uint16_t)iw416.lastEvent());
    Serial1.print(" event_info=0x");      printHex16((uint16_t)iw416.lastEventInfo());
    Serial1.println();
    dumpCardRegs("postdrain", false);
    if (cmdPortAlive()) {
        Serial1.println("uap_recover=drain reason=command_port_answered_after_data_port_drain");
        return;
    }
    for (uint8_t t = 1; t <= kRecoverTries; t++) {
        delay(kRecoverIntervMs);
        if (cmdPortAlive()) {
            Serial1.print("uap_recover="); Serial1.print((int)t);
            Serial1.println(" unit=tries");
            return;
        }
    }
    Serial1.print("uap_recover=never tries="); Serial1.print((int)kRecoverTries);
    Serial1.print(" interval_ms=");            Serial1.println((int)kRecoverIntervMs);

    // ★ THE LAST OF THE FOUR UNKNOWNS FAULT 1 WAS LEFT WITH: is a CARD reset
    // enough to get the command port back, or does the module need a power
    // cycle?  Every run so far started from a board reset, so recovery was
    // never tested in isolation -- and the answer decides whether Phase 1 can
    // iterate at all.  If a card reset works, a uAP driver can recover in the
    // field and a bench run can try several variants per flash; if only a power
    // cycle does, every SYS_CONFIGURE experiment costs a human at the board.
    //
    // This repeats exactly what setup() does, in the same order, so a failure
    // here is a failure of the reset path and not of some shortcut.
#if defined(HAVE_IW416_FW)
    Serial1.println("uap_reinit=attempting");
    m2ReleaseWifiReset();
    SdioHost::Status rs = sdio.begin();
    Serial1.print("uap_reinit sdio="); Serial1.print(statusName(rs));
    if (rs == SdioHost::OK) {
        rs = iw416.begin();
        Serial1.print(" iw416="); Serial1.print(statusName(rs));
    }
    if (rs == SdioHost::OK) {
        rs = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
        Serial1.print(" fw="); Serial1.print(statusName(rs));
    }
    if (rs == SdioHost::OK) {
        (void)iw416.refreshIoPort();
        delay(50);
        (void)iw416.enableHostInt();
        uint8_t mac[6]; uint32_t rel = 0; uint16_t hw = 0;
        rs = iw416.getHwSpec(mac, &rel, &hw);
        Serial1.print(" hwspec="); Serial1.print(statusName(rs));
    }
    Serial1.println();
    Serial1.print("uap_reinit=");
    Serial1.println(rs == SdioHost::OK ? "recovered" : "failed");
    dumpCardRegs("postreinit", false);
#else
    Serial1.println("uap_reinit=skipped reason=no_blob_to_redownload");
#endif
}

// --- board preamble ----------------------------------------------------------
// Identical to m2_lwip_test / m2_throughput_test: without it the card either
// stays in power-down or holds the previous image's state and never answers
// CMD5.
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

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 uAP probe up");

    m2ReleaseWifiReset();
    Serial1.println("m2_wifi_reset=released");
    sdio.useIoVoltage1V8(true);
    Serial1.println("sdio_io_voltage=1v8_requested");

    s_sdioSt = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.print(statusName(s_sdioSt));
    Serial1.print(" rc="); Serial1.println((int)s_sdioSt);

    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        Serial1.print("iw416_begin="); Serial1.print(statusName(s_iwSt));
        Serial1.print(" ioport=0x");   Serial1.print(iw416.ioPort(), HEX);
        Serial1.print(" fw_status=0x"); Serial1.println(iw416.fwStatus(), HEX);
        if (s_iwSt == SdioHost::OK) {
#if defined(HAVE_IW416_FW)
            // Silicon path: the IW416 has no flash, so the blob goes down
            // every boot.  WHICH blob is the experiment -- see CMakeLists.
            s_fwSt = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
            Serial1.print("fw_download="); Serial1.println(statusName(s_fwSt));
#else
            // No blob compiled in.  The only way there is firmware running is
            // the QEMU model's `fw-preboot=on`, an admitted fiction (its NOTE
            // 7) that stands in for a download no gate may depend on, since
            // the blob is NXP-licensed.  Taking FIRMWARE_READY as permission
            // to talk to the command port is what makes the wifi gate able to
            // exercise the probe at all; on silicon a freshly power-cycled
            // card is in its bootloader here and this branch correctly fails.
            s_fwSt = (iw416.fwStatus() == Iw416::FIRMWARE_READY)
                         ? SdioHost::OK : SdioHost::CMD_TIMEOUT;
            Serial1.print("fw_download=skipped (no blob supplied) preboot=");
            Serial1.println(s_fwSt == SdioHost::OK ? 1 : 0);
#endif
            if (s_fwSt == SdioHost::OK) {
                (void)iw416.refreshIoPort();
                delay(50);
                (void)iw416.enableHostInt();
                uint32_t fwRel = 0; uint16_t hwVer = 0;
                // GET_HW_SPEC also performs FUNC_INIT, which the card wants
                // before anything else -- and it prints the blob's own
                // release, which is what a later reader needs to know WHICH
                // firmware answered the way it did.
                if (iw416.getHwSpec(s_mac, &fwRel, &hwVer) == SdioHost::OK) {
                    Serial1.print("fw_release=0x"); Serial1.print(fwRel, HEX);
                    Serial1.print(" hw_version=0x"); Serial1.print(hwVer, HEX);
                    Serial1.print(" mac=");
                    dumpBytes(s_mac, 6);
                    Serial1.println();
                    s_haveCard = true;
                    // The baseline every later dump is compared against.
                    dumpCardRegs("init", false);
                }
            }
        }
    }
    if (!s_haveCard) {
        Serial1.print("iw416_init="); Serial1.print(statusName(s_iwSt));
        Serial1.print(" fw="); Serial1.println(statusName(s_fwSt));
    }

    if (s_haveCard) {
        // Nothing RF-facing is enabled first: no MAC_CONTROL, no scan, no
        // association.  The question is purely whether the command handlers
        // exist, and every extra step is another way for the run to fail for
        // a reason that has nothing to do with AP mode.
        for (uint8_t i = 0; i < kProbeCount; i++) {
#if !defined(M2_UAP_PROBE_BSS_START)
            if (kProbes[i].rfEmitting) {
                Serial1.print("uap_probe cmd=0x"); printHex16(kProbes[i].cmd);
                Serial1.print(" name="); Serial1.print(kProbes[i].name);
                Serial1.println(" st=SKIPPED reason=rf_emitting");
                continue;
            }
#endif
            for (uint8_t b = kProbes[i].uapOnly ? 1 : 0; b < 2; b++) runCell(i, b);
        }
        reportVerdict();
#if defined(M2_UAP_PROBE_BSS_START)
        runBssStartSequence();
#endif
        probeRecovery();
        // Last read of the run, so this one may safely consume HOST_INT_STATUS.
        dumpCardRegs("final", true);
    }

    Serial1.println("uap_probe_done");
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 2000) {
        last = millis();
        Serial1.print("hb card="); Serial1.println(s_haveCard ? 1 : 0);
    }
}

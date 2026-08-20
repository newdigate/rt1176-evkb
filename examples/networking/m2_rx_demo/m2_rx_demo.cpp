// m2_rx_demo — exercise the M.2 SDIO **data path** against QEMU's IW416 model.
//
// THIS IS NOT A WI-FI APPLICATION. It never scans, never associates, never
// runs the supplicant and never carries a firmware blob or a credential. It
// drives exactly one thing: Iw416::serviceLink() moving frames off the card's
// 32-slot upload ring, plus a periodic sendDataFrame() to consume TX credits.
// For actual Wi-Fi see networking/m2_lwip_test (WPA2 + lwip, hardware only).
//
// WHY IT EXISTS
//   Every other example in this tree gates serviceLink() behind a successful
//   connectStation(), so none of them can reach the data path without a real
//   AP, real credentials and the NXP-licensed blob. The two bugs that cost
//   this subsystem days both live in that data path:
//     * W8  — the 32-slot ring read as a 16-bit bitmap, so every upload
//             landing on slots 16-31 was invisible;
//     * W12 — HOST_INT_STATUS is clear-on-read and a non-draining path
//             consumed the upload bit, stranding the frame forever.
//   qemu2's hw/sd/iw416-sdio.c models both hazards deliberately, and this
//   image is the host side those regression gates need. run_qemu_ring.sh and
//   run_qemu_stranded.sh are the gates; run_qemu.sh asserts the card-absent
//   fallback a default sweep sees.
//
// TWO SERVICE WINDOWS (W15)
//   A run that reaches a card services the link TWICE, back to back:
//     1. `svc` ... `demo_done`   -- POLLED. serviceLink() reads HOST_INT_STATUS
//        once per pass and paces on delay(1): ~1000 CMD52/s on an idle link.
//        This window is byte-for-byte what W14 shipped, which is why the two
//        regression gates above are unaffected by any of this.
//     2. `irq_mode=1` ... `irq_done` -- INTERRUPT. Iw416::setInterruptMode(true)
//        turns on the SDIO card interrupt (DAT1) and the quiet passes stop
//        touching the bus. The two `phase=` lines at the end are the A/B.
//   The point of doing it in one run is that the two windows differ in exactly
//   one thing. run_qemu_irq.sh divides one by the other; see IRQ_RUN_MS below
//   for why this is not two builds or two runs.
//   Note the driver's default is POLLED and stays that way -- DAT1 has never
//   been exercised on this board's silicon, so this example turning it on is a
//   deliberate opt-in, not the library's behaviour.
//
// HOW IT REACHES A RUNNING CARD WITH NO BLOB
//   The model's `fw-preboot=on` property makes it come up with firmware
//   already running. That is an admitted FICTION (a real IW416 has no flash
//   and always needs a download — see NOTE 7 in the model's header), and it is
//   the only way a blob-free test tree can reach the rings at all. Nothing
//   downstream of it is faked: the rings, the clear-on-read interrupt and the
//   command port are the same code the post-download path reaches. So this
//   example proves NOTHING about firmware download; m2_sdio_probe's
//   transcript_hw_evkb.txt is where that lives.
//
// ON SILICON
//   It runs, and it will enumerate a real MAYA-W161 and read its MAC — but
//   with no blob compiled in the card has no firmware, so getHwSpec() and
//   everything after it fail and no frame ever arrives. Use m2_sdio_probe.
#include <Arduino.h>
#include <SdioHost.h>
#include <SdioFunc.h>
#include <Iw416.h>
#include <string.h>

static SdioHost sdio;
static SdioFunc sdioFunc(sdio);
static Iw416    iw416(sdio, sdioFunc);

static const char *statusName(SdioHost::Status s) {
    switch (s) {
    case SdioHost::OK:               return "ok";
    case SdioHost::NO_IO_FUNCTION:   return "no-io-function";
    case SdioHost::CMD_TIMEOUT:      return "cmd-timeout";
    case SdioHost::CMD_CRC:          return "cmd-crc";
    case SdioHost::CLOCK_UNSTABLE:   return "clock-unstable";
    case SdioHost::BAD_CIS:          return "bad-cis";
    case SdioHost::CMD5_NO_RESPONSE: return "cmd5-no-response";
    default:                         return "err";
    }
}

// --- M.2 board bring-up preamble ------------------------------------------
// QEMU does NOT need this: the model is attached to USDHC1 by the machine and
// nothing in it watches a reset line. It is here anyway, and deliberately.
// The reset/1.8 V preamble lives in EXAMPLES rather than in the driver (see
// m2_sdio_probe and m2_throughput_test, which carry the identical block), so
// an example written without it is green in QEMU and dead on silicon — a
// QEMU-only fiction, which is precisely the failure mode the two-gate rule
// exists to prevent. Cost here is a few register writes at boot.
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30

static void m2ReleaseWifiReset() {
    M2_SDIO_RST_MUX = 0x10u | 0xAu;
    M2_WL_RST_MUX   = 0x10u | 0xAu;
    GPIO9_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    GPIO9_DR_SET = (1u << M2_SDIO_RST_BIT);
    delay(100);
    GPIO9_DR_SET = (1u << M2_WL_RST_BIT);
    delay(1000);
}

// How long to service the link (measured from demo_ready) before printing the
// summary. 6 s is ~3x what the gates need: 16
// injected frames at the model's default 100 ms period are all queued by 1.8 s,
// and the driver's bitmap safety net -- the only thing that moves them when the
// upload interrupt is suppressed -- fires every RX_BITMAP_CHECK_PASSES (64)
// quiet serviceLink passes, i.e. every ~64 ms. It is a fixed budget rather than
// "stop at N frames" on purpose: a gate that stops as soon as it is happy can
// never assert that nothing ELSE arrived.
static const uint32_t DEMO_RUN_MS = 6000;

// W15: a SECOND service window of the same length, run with the driver's
// interrupt mode ON, immediately after `demo_done`.
//
// WHY THE RUN IS SPLIT RATHER THAN THE BUILD
//   The measurement that matters is polls-per-frame, and it is only meaningful
//   as a comparison. Two builds would compare two ELFs; two QEMU runs would
//   compare two injection schedules. One run with two windows compares neither
//   -- same image, same card model, same injection cadence, same 6 s budget,
//   and the ONLY difference is Iw416::setInterruptMode(). That is what makes
//   run_qemu_irq.sh's ratio an argument rather than an anecdote.
//   It also keeps every gate on the ONE build a fresh clone makes: a second
//   build directory would not be probed by the sweep runner's SKIP check
//   (it only looks in build/), so a clone that built the usual way would see
//   the new gate FAIL rather than SKIP -- worse than either.
//
// The two W14 regression gates are untouched by construction: this window
// begins only AFTER the `demo_done` line they assert on, and both of them stop
// polling the capture and reap QEMU the moment that line appears.
static const uint32_t IRQ_RUN_MS = 6000;

// W16: how many frames the TX-aggregation burst stages. Kept BELOW the
// driver's AGGR_PKT_LIMIT so the batch is still open when flushTx() is called
// -- that is the path under test, not the auto-flush at the limit.
static const uint8_t  TXAGGR_FRAMES   = 6;
// Long enough for the card's loopback (when enabled) to come back up the RX
// ring and be serviced; short enough that a gate is not waiting on it.
static const uint32_t TXAGGR_DRAIN_MS = 1500;
static uint32_t g_txAggrBase = 0;
static void txAggrBurst();

// 0 = polled window, 1 = interrupt window, 2 = finished (heartbeats).
static uint8_t  g_phase   = 0;
static bool     g_ready   = false;
static uint32_t g_frames  = 0;
static uint32_t g_started = 0;
// Snapshots taken at the phase boundary, so each window's numbers are a delta
// rather than a running total -- the driver's counters are cumulative.
static uint32_t g_polledFrames = 0;
static uint32_t g_polledPolls  = 0;

// Every SERVICE-side register poll, whatever SDIO command carried it: the
// per-pass HOST_INT_STATUS read was a CMD52 until W16 and is a register-port
// CMD53 after it, and the interrupt-mode A/B is about how OFTEN the host asks
// the card for its status -- not about which command it asks with.  Summing is
// what stops a driver satisfying the ratio by moving the cost sideways.
static uint32_t svcPolls() {
    return iw416.cmd52PollsSvc() + iw416.cmd53RegsSvc();
}

// The sink prints the whole 802.3 header plus the model's own payload marker.
// Nothing in this image knows any of these bytes: dst/src/ethertype and the
// [seq][slot] marker are produced by the card model, which is what makes
// `from_slot=` an oracle rather than an echo — it is the slot the CARD chose,
// reported back by a host that had to walk a 32-bit bitmap to find it.
static void rxSink(void *ctx, const uint8_t *f, uint16_t len) {
    (void)ctx;
    g_frames++;
    Serial1.print("rx_frame ");
    Serial1.print(g_frames);
    Serial1.print(": len=");
    Serial1.print(len);
    Serial1.print(" dst=");
    for (int i = 0; i < 6; i++) {
        if (f[i] < 0x10) Serial1.print('0');
        Serial1.print(f[i], HEX);
    }
    Serial1.print(" src=");
    for (int i = 6; i < 12; i++) {
        if (f[i] < 0x10) Serial1.print('0');
        Serial1.print(f[i], HEX);
    }
    Serial1.print(" ethertype=0x");
    uint16_t et = (uint16_t)((f[12] << 8) | f[13]);
    if (et < 0x1000) Serial1.print('0');
    Serial1.print(et, HEX);
    if (len >= 18) {
        Serial1.print(" seq=");
        Serial1.print((uint16_t)((f[14] << 8) | f[15]));
        Serial1.print(" from_slot=");
        Serial1.print(f[16]);
    }
    Serial1.print(" rx_port_now=");
    Serial1.print(iw416.rxPort());
    Serial1.println();
}

static void printCounters(const char *tag) {
    bool bmOk = false;
    uint32_t bm = iw416.probeRdBitmap(&bmOk);
    Serial1.print(tag);
    Serial1.print(" frames=");      Serial1.print(g_frames);
    Serial1.print(" rx_data=");     Serial1.print(iw416.rxDataCount());
    Serial1.print(" rd_bitmap=0x"); Serial1.print(bm, HEX);
    Serial1.print(" wr_bitmap=0x"); Serial1.print(iw416.lastWrBitmap(), HEX);
    Serial1.print(" ring=");        Serial1.print(iw416.txPort());
    Serial1.print("/");             Serial1.print(iw416.rxPort());
    Serial1.print("/");             Serial1.print(iw416.rxRingResyncs());
    Serial1.print(" stranded=");    Serial1.print(iw416.rxStrandedRecovered());
    Serial1.print("/");             Serial1.print(iw416.rxDesyncRecovered());
    Serial1.print(" drainerr=");    Serial1.print(iw416.rxDrainErrors());
    Serial1.print(" notready=");    Serial1.print(iw416.rxSlotNotReady());
    Serial1.print(" int_seen=0x");  Serial1.print(iw416.intStatusSeen(), HEX);
    Serial1.print(" tx=");          Serial1.print(iw416.dataTxCount());
    Serial1.print(" c53=");         Serial1.print(iw416.cmd53Count());
    // W15, appended at the END of the line on purpose: run_qemu_ring.sh and
    // run_qemu_stranded.sh match this line by prefix and by `field=value `
    // fragments, so new fields at the tail cannot perturb them.
    // c52svc is the service-side CMD52 count -- the thing interrupt mode is
    // meant to collapse -- and cardints is the DAT1 assertions serviced, which
    // is 0 for the whole polled window by construction.
    Serial1.print(" c52svc=");      Serial1.print(iw416.cmd52PollsSvc());
    Serial1.print(" cardints=");    Serial1.print(iw416.cardInts());
    // W16, appended at the tail for the same reason W15's fields were: the
    // ring/stranded gates match this line by `field=value ` fragments, so a
    // new field at the end cannot perturb them.  c53regs is the register-port
    // read that replaced serviceLink's per-pass HOST_INT_STATUS CMD52 (and the
    // bitmap/RD_LEN/CMD_RD_LEN reads with it); bus is busCommands(), every
    // SDIO command the data/service path issued, which is the number to
    // normalise per frame when judging a throughput change.
    Serial1.print(" c53regs=");     Serial1.print(iw416.cmd53RegsSvc());
    Serial1.print("/");             Serial1.print(iw416.cmd53RegsTx());
    Serial1.print(" bus=");         Serial1.print(iw416.busCommands());
    // W16 aggregation, as <CMD53s that carried more than one slot>/<slots they
    // carried>.  THE un-fakeable pair: a driver that moves one frame per
    // command reads 0/0 here no matter how many frames it delivers or how
    // fast, so a gate asserting these cannot be satisfied by anything except
    // aggregation actually happening.  c53rx is the data-CMD53 count for the
    // RX direction alone, which is what makes "commands per RX frame"
    // divisible in an image that also transmits.
    Serial1.print(" c53rx=");       Serial1.print(iw416.cmd53Rx());
    Serial1.print(" rxaggr=");      Serial1.print(iw416.rxAggrBatches());
    Serial1.print("/");             Serial1.print(iw416.rxAggrSlots());
    Serial1.print(" txaggr=");      Serial1.print(iw416.txAggrBatches());
    Serial1.print("/");             Serial1.print(iw416.txAggrSlots());
    // W16 register-port health.  mpregs=1 means the multiport register read is
    // in use; 0 means the card failed its sanity check (see mpRegsUsable) and
    // the driver fell back to the pre-W16 CMD52 path -- the link is alive but
    // slow, and `rej=` carries the CARD_STATUS byte that came back so the
    // failure is diagnosable rather than merely visible.  err= counts
    // register-port reads that failed on the bus.
    Serial1.print(" mpregs=");      Serial1.print(iw416.mpRegsUsable() ? 1 : 0);
    Serial1.print(" rej=0x");       Serial1.print(iw416.mpRegsRejected(), HEX);
    Serial1.print(" mperr=");       Serial1.print(iw416.mpRegsErrors());
    Serial1.print(" split=");       Serial1.println(iw416.rxSplitMismatch());
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 RX demo up");

    m2ReleaseWifiReset();
    sdio.useIoVoltage1V8(true);

    SdioHost::Status st = sdio.begin();
    Serial1.print("sdio_begin=");
    Serial1.print(statusName(st));
    Serial1.print(" r4=0x");
    Serial1.println(sdio.lastR4(), HEX);
    if (st != SdioHost::OK) { Serial1.println("demo_result=no-card"); return; }

    st = iw416.begin();
    Serial1.print("iw416_begin=");
    Serial1.print(statusName(st));
    Serial1.print(" ioport=0x");
    Serial1.print(iw416.ioPort(), HEX);
    Serial1.print(" fw_status=0x");
    Serial1.println(iw416.fwStatus(), HEX);
    if (st != SdioHost::OK) { Serial1.println("demo_result=no-function1"); return; }

    // No downloadFirmware(): the card model was told to come up already
    // running (fw-preboot), because the real blob is NXP-licensed.
    (void)iw416.refreshIoPort();
    (void)iw416.enableHostInt();

    uint8_t mac[6] = {0}; uint32_t rel = 0; uint16_t hw = 0;
    SdioHost::Status hs = iw416.getHwSpec(mac, &rel, &hw);
    Serial1.print("hw_spec=");
    Serial1.print(statusName(hs));
    Serial1.print(" mac=");
    for (int i = 0; i < 6; i++) {
        if (mac[i] < 0x10) Serial1.print('0');
        Serial1.print(mac[i], HEX);
        if (i < 5) Serial1.print(':');
    }
    Serial1.print(" fw_release=0x");
    Serial1.print(rel, HEX);
    Serial1.print(" hw_version=0x");
    Serial1.println(hw, HEX);
    if (hs != SdioHost::OK) { Serial1.println("demo_result=no-firmware"); return; }

    (void)iw416.reconfigureTxBuffers(2048);
    (void)iw416.macControl(Iw416::MAC_RX_ON | Iw416::MAC_TX_ON |
                           Iw416::MAC_ETHERNETII | Iw416::MAC_RTS_CTS);
    // ★ W16: the DRIVER now ships RX aggregation OFF -- it costs 4.6x
    // throughput on silicon (see setRxAggregation() in Iw416.h).  This example
    // turns it back ON deliberately, because its whole purpose is to exercise
    // the ring and three of its gates assert on aggregated reads.  A gate that
    // silently followed the driver's default would go from testing the feature
    // to testing nothing, and would keep passing while it did.
    iw416.setRxAggregation(true);
    g_ready   = true;
    g_started = millis();
    Serial1.println("demo_ready");
}

// W16, TX MULTIPORT AGGREGATION.  Six frames staged into one CMD53.
//
// ★ IT RUNS AFTER `irq_done` AND AFTER BOTH phase= LINES, on purpose. Every
// pre-W16 gate on this example stops reading at one of those, so the burst
// cannot perturb a single existing assertion -- the same discipline W15 used
// when it appended the interrupt window after `demo_done`.
//
// The frames carry a distinct ethertype (0x88B6) and an index byte, so that
// with `-global iw416-sdio.tx-loopback=on` the CARD hands each one back up the
// RX ring and the sink prints it. That loopback is what makes this a test of
// the AGGREGATED WRITE rather than of our own bookkeeping: the card has to
// split the run by each packet's own block-padded SDIOPkt size and place each
// piece on its own download slot, so a wrong pad, a wrong port_count or a
// wrong start slot shows up as missing or garbled frames coming back -- none
// of which this image could fake, because it never sees the split.
static void txAggrBurst() {
    const uint32_t c53TxBefore = iw416.cmd53Tx();
    iw416.setTxAggregation(true);
    Serial1.print("txaggr_begin aggr=");
    Serial1.print(iw416.txAggregation() ? 1 : 0);
    Serial1.print(" tx_port=");
    Serial1.println(iw416.txPort());

    uint8_t frame[60];
    uint8_t sent = 0;
    for (uint8_t i = 0; i < TXAGGR_FRAMES; i++) {
        memset(frame, 0, sizeof(frame));
        memset(frame, 0xFF, 6);                       // broadcast
        frame[6] = 0x02; frame[7] = 0x11; frame[8] = 0x22;
        frame[9] = 0x33; frame[10] = 0x44; frame[11] = 0x55;
        frame[12] = 0x88; frame[13] = 0xB6;           // our own ethertype
        // f[14..15] is what rxSink decodes as `seq=`, so a loopback frame
        // reports the index this image gave it -- which is how the gate pairs
        // what was sent with what came back, in order.
        frame[15] = (uint8_t)(i + 1);
        SdioHost::Status ts = iw416.sendDataFrame(frame, sizeof(frame), 50);
        Serial1.print("txaggr_stage i=");
        Serial1.print(i + 1);
        Serial1.print(" st=");
        Serial1.print(statusName(ts));
        Serial1.print(" queued=");
        Serial1.println(iw416.txQueued());
        if (ts != SdioHost::OK) break;
        sent++;
    }
    // The explicit flush is the path a poll loop uses (the netif calls it every
    // iteration); TXAGGR_FRAMES is deliberately BELOW the driver's packet limit
    // so the batch is still open here and this call is what writes it.
    SdioHost::Status fs = iw416.flushTx();
    Serial1.print("txaggr_done sent=");
    Serial1.print(sent);
    Serial1.print(" flush=");
    Serial1.print(statusName(fs));
    Serial1.print(" c53tx=");
    Serial1.print(iw416.cmd53Tx() - c53TxBefore);
    Serial1.print(" batches=");
    Serial1.print(iw416.txAggrBatches());
    Serial1.print(" slots=");
    Serial1.print(iw416.txAggrSlots());
    Serial1.print(" queued=");
    Serial1.println(iw416.txQueued());
}

void loop() {
    static uint32_t beats = 0;
    static uint32_t alive = 0;
    static uint32_t lastAlive = 0;

    if (g_ready && g_phase < 3) {
        bool dropped = false;
        (void)iw416.serviceLink(rxSink, nullptr, &dropped, 100);

        if ((beats % 10) == 0) {
            printCounters(g_phase == 0 ? "svc" : (g_phase == 1 ? "svc_irq"
                                                              : "svc_txaggr"));
        }

        // One TX every 20 passes, to exercise the download credits.  Confined
        // to the first two windows: phase 2 belongs to the aggregation burst,
        // and a stray single-frame send interleaved into its ring run would
        // make the batch's slot span mean something else.
        if (g_phase < 2 && (beats % 20) == 7) {
            static const uint8_t frame[60] = {
                0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0x02,0x11,0x22,0x33,0x44,0x55,
                0x88,0xB5, 'H','O','S','T','T','X'
            };
            SdioHost::Status ts = iw416.sendDataFrame(frame, sizeof(frame), 50);
            Serial1.print("tx=");
            Serial1.print(statusName(ts));
            Serial1.print(" wr_bitmap=0x");
            Serial1.print(iw416.lastWrBitmap(), HEX);
            Serial1.print(" tx_port_now=");
            Serial1.println(iw416.txPort());
        }
        beats++;

        uint32_t budget = (g_phase == 0) ? DEMO_RUN_MS
                        : (g_phase == 1) ? IRQ_RUN_MS : TXAGGR_DRAIN_MS;
        if ((uint32_t)(millis() - g_started) >= budget) {
            if (g_phase == 0) {
                printCounters("demo_done");
                g_polledFrames = g_frames;
                g_polledPolls  = svcPolls();
                // W15: hand the link over to the card interrupt. Both halves
                // are reported rather than assumed -- interruptMode() is the
                // driver's own view and cardIntEnabled() is the uSDHC's, and a
                // gate that only saw one of them could not tell "the mode was
                // requested" from "the mode engaged".
                iw416.setInterruptMode(true);
                Serial1.print("irq_mode=");
                Serial1.print(iw416.interruptMode() ? 1 : 0);
                Serial1.print(" host_cardint=");
                Serial1.println(sdio.cardIntEnabled() ? 1 : 0);
                g_phase   = 1;
                g_started = millis();
            } else if (g_phase == 1) {
                printCounters("irq_done");
                // The A/B, as two deltas the gate can divide. Nothing here is
                // derived: frames come from the sink, svcpolls from the
                // driver's own bus-attribution counters, cardints from the ISR.
                //
                // ★ svcpolls is cmd52PollsSvc() + cmd53RegsSvc(), i.e. EVERY
                // service-side register poll whatever command type carried it.
                // W15 divided cmd52PollsSvc() alone, and W16 then moved that
                // traffic onto a register-port CMD53 -- which made the old
                // ratio collapse while interrupt mode was working perfectly.
                // Summing is not a loosening: a driver cannot satisfy it by
                // moving the cost sideways, which is exactly what defeated the
                // narrower measure.
                Serial1.print("phase=polled ms=");
                Serial1.print(DEMO_RUN_MS);
                Serial1.print(" frames=");
                Serial1.print(g_polledFrames);
                Serial1.print(" svcpolls=");
                Serial1.print(g_polledPolls);
                Serial1.println(" cardints=0");
                Serial1.print("phase=irq ms=");
                Serial1.print(IRQ_RUN_MS);
                Serial1.print(" frames=");
                Serial1.print(g_frames - g_polledFrames);
                Serial1.print(" svcpolls=");
                Serial1.print(svcPolls() - g_polledPolls);
                Serial1.print(" cardints=");
                Serial1.println(iw416.cardInts());
                g_phase = 2;
                g_started = millis();
                // W16's window opens here, after every pre-W16 assertion on
                // this image has already been printed.
                g_txAggrBase = g_frames;
                txAggrBurst();
            } else {
                // What came BACK. With tx-loopback off this is 0 and the gates
                // that run that way ignore the line; with it on, every frame
                // the card accepted out of the aggregated write reappears on
                // its own upload slot and the sink has already printed it.
                Serial1.print("txaggr_loop rx=");
                Serial1.print(g_frames - g_txAggrBase);
                Serial1.print(" c53rx=");
                Serial1.print(iw416.cmd53Rx());
                Serial1.print(" rxbatches=");
                Serial1.print(iw416.rxAggrBatches());
                Serial1.print(" rxslots=");
                Serial1.println(iw416.rxAggrSlots());
                g_phase = 3;
            }
        }
        return;
    }

    // Not ready (no card / no firmware), or finished: heartbeat so a gate can
    // tell "image still running" from "image wedged". A reason code alone
    // cannot: "nothing arrived" is also what a dead image looks like.
    if ((uint32_t)(millis() - lastAlive) >= 500) {
        lastAlive = millis();
        Serial1.print("alive=");
        Serial1.println(++alive);
    }
}

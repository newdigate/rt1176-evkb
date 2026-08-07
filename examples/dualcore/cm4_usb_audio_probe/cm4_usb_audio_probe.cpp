/*
 * cm4_usb_audio_probe -- Phase 7.3 of the CM4 USB arc: does the Cortex-M4 drive
 * an ISOCHRONOUS OUT STREAM to a USB audio device?
 *
 * 7.1 proved the CM4 owns the port and takes IRQ 135 on its own NVIC. 7.2
 * proved the real USBHost_t36 transport core enumerates from a freestanding CM4
 * image. This adds the class driver: claim, control sequence, 32 siTDs armed,
 * a tone streaming out of them -- all on the M4, with the CM7 booting the image
 * and relaying MU tokens and touching no USB register.
 *
 * ★★ WHAT THIS GATE DELIBERATELY DOES NOT ASSERT, and why the file says so
 * twice. Isochronous data does NOT flow against QEMU's emulated usb-audio
 * device: measured 2026-08-06 (docs/KNOWN-BROKEN-GATES.md), an OUT sketch
 * enumerates, claims, selects alt 1, completes the entire control sequence and
 * then sits at pkts/s = 0 forever. Cause unestablished; a split-transaction/TT
 * mismatch is suspected. So packet flow is a WORLD-SPLIT token here -- printed
 * by the firmware, PRESENCE-checked by the QEMU gate, asserted TRUE only by
 * silicon. Precedent: 7.1's PHY_PLL_CM4, 3.2's rdv, 2C's systick.
 *
 * AUDIO_CM4 is therefore the CONTROL-PLANE verdict and stops at "armed without
 * error". That is a real and previously unproven capability -- the CM4 running
 * a class driver's control sequence and allocating a live isochronous schedule
 * -- and conflating it with data flow would make the gate unpassable for a
 * reason that has nothing to do with the CM4. 7.1 made exactly that mistake
 * once already (asserting PHY_PLL_CM4=PASS, which QEMU's PHY model cannot
 * satisfy for either core).
 *
 * Tokens over Serial1, streamed by the CM4 over MU channel 0 as
 * (stage marker, observation) pairs, then results:
 *   s1/dwt      A5B00001 / cycle delta measured over a bounded spin. ZERO means
 *                          DEMCR.TRCENA / DWT_CTRL.CYCCNTENA did not take, every
 *                          clock in the shim is frozen, and so is the audio
 *                          driver's control watchdog -- a stalled configuration
 *                          request would then hang forever, silently.
 *   s2/usbintr  A5B00002 / USBINTR after USBHost::begin(). UAIE (bit 18) must be
 *                          set: every control transfer -- enumeration's AND the
 *                          driver's SET_INTERFACE/SET_CUR -- completes through
 *                          UAI -> followup_Transfer (ehci.cpp:380).
 *   s3/portsc   A5B00003 / PORTSC1 armed and settled. CCS expected 0; the device
 *                          arrives AFTER this marker, which is what the gate
 *                          polls for before it hotplugs.
 *   s4/bound    A5B00004 / USBDriver::operator bool -- the audio driver is bound
 *                          to a device. Then vid/pid, THE ORACLE: the CM4 image
 *                          cannot know them except off the wire.
 *   s5/alt      A5B00005 / the selected alternate setting, or FFFFFFFF for none.
 *                          This is the FORMAT: uac1_find_alt() refuses a rate,
 *                          channel count or width the device never advertised,
 *                          so a valid alt means a real negotiation happened.
 *   s6/ctrl     A5B00006 / queue_fails<<16 | timeouts<<8 | state. All zero is
 *                          the assertion: nothing outstanding, the watchdog
 *                          never had to abandon a request, and no retry outran
 *                          the shared Transfer_t pool.
 *   s7/armed    A5B00007 / beginStreaming() returned true: 32 siTDs allocated,
 *                          filled from the tone FIFO and linked into all 32
 *                          periodic frame slots.
 *   pkts        ........   ★ WORLD-SPLIT. Packets the CONTROLLER reports having
 *                          transmitted, over `elapsed` ms.
 *   xerr        ........   transport errors: xact + babble + buffer + short
 *                          sends. Vacuously 0 when nothing was transmitted.
 *   unders      ........   underruns -- the producer failing to keep the FIFO fed.
 *   queued      ........   FIFO level in samples at the end of the window.
 *   loops       ........   service-loop iterations in the streaming window,
 *   lps         ........   and the rate. ★ THE LATENCY MARGIN, made visible: the
 *                          CM7 budget measurement says the binding constraint is
 *                          contiguous stall length (600 us clean, 850 us fail),
 *                          not throughput. 1/lps is the mean gap on a core 2.5x
 *                          slower.
 *   elapsed     ........   the window actually measured, in ms.
 *   portsc2     ........   PORTSC1 at the end; CCS expected 1, so portsc ->
 *                          portsc2 is a real 0->1 connect edge.
 *   done        D0DE0009
 *
 * ★ THE CM7 STREAMS. recv_show() prints each word as it arrives; do not tidy it
 * back into a batch of reads followed by a batch of prints. The gate polls the
 * UART for s3 and hotplugs the moment it appears, so the connect event lands
 * inside the CM4's armed window. Batch printing emits s3 only after ALL words
 * have arrived -- i.e. after the window has closed -- and the gate would then
 * plug too late and measure a failure that has nothing to do with the stack.
 * Measured 2026-08-06 on 7.1: it looked identical to a real defect.
 */
#include "Arduino.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_usb_audio.h"  /* generated by teensy_add_cm4_image(cm4_usb_audio ...) */

/* Wall-clock, not an iteration count. The CM4's longest silence is its 10 s
 * claim window (it exits early once the format is negotiated), and its second
 * longest is the 4 s streaming window; 30 s outlasts either with room for PHY
 * bring-up and for the emulator running slower than wall-clock. Anything past
 * that is a genuine hang, reported as the missing token rather than by blocking
 * the gate forever. */
static bool wait_recv(uint8_t ch, uint32_t *out) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < 30000u) {
        if (MU.tryReceive(ch, out)) return true;
    }
    return false;
}

static void phex(const char *k, uint32_t v) {
    Serial1.print(k);
    Serial1.print('=');
    for (int s = 28; s >= 0; s -= 4) Serial1.print("0123456789ABCDEF"[(v >> s) & 0xF]);
    Serial1.println();
}

/* Receive one word and print it IMMEDIATELY -- see the ★ note in the header. */
static bool recv_show(const char *k, uint32_t *out) {
    if (!wait_recv(0, out)) return false;
    phex(k, *out);
    return true;
}

void setup() {
    Serial1.begin(115200);
    Serial1.println("CM4USBAUDIO-GATE v1");

    MU.begin();
    Multicore.begin(cm4_usb_audio, sizeof(cm4_usb_audio));

    uint32_t s1 = 0, dwt = 0, s2 = 0, usbintr = 0, s3 = 0, portsc = 0;
    uint32_t s4 = 0, bound = 0, vid = 0, pid = 0;
    uint32_t s5 = 0, alt = 0xFFFFFFFFu, s6 = 0, ctrl = 0, s7 = 0, armed = 0;
    uint32_t pkts = 0, xerr = 0, unders = 0, queued = 0;
    uint32_t loops = 0, lps = 0, elapsed = 0, portsc2 = 0, done = 0;

    bool ok = recv_show("s1",      &s1)      && recv_show("dwt",     &dwt)
           && recv_show("s2",      &s2)      && recv_show("usbintr", &usbintr)
           && recv_show("s3",      &s3)      && recv_show("portsc",  &portsc)
           && recv_show("s4",      &s4)      && recv_show("bound",   &bound)
           && recv_show("vid",     &vid)     && recv_show("pid",     &pid)
           && recv_show("s5",      &s5)      && recv_show("alt",     &alt)
           && recv_show("s6",      &s6)      && recv_show("ctrl",    &ctrl)
           && recv_show("s7",      &s7)      && recv_show("armed",   &armed)
           && recv_show("pkts",    &pkts)    && recv_show("xerr",    &xerr)
           && recv_show("unders",  &unders)  && recv_show("queued",  &queued)
           && recv_show("loops",   &loops)   && recv_show("lps",     &lps)
           && recv_show("elapsed", &elapsed)
           && recv_show("portsc2", &portsc2) && recv_show("done",    &done);

    /* Decoded, so a human reading the transcript does not have to remember bit
     * positions -- and so a wrong decode is visible rather than latent. */
    Serial1.print("decode: uaie=");   Serial1.print((usbintr >> 18) & 1u);
    Serial1.print(" ccs=");           Serial1.print(portsc  & 1u);
    Serial1.print(" ccs2=");          Serial1.print(portsc2 & 1u);
    Serial1.print(" alt=");           Serial1.print((int32_t)alt);
    Serial1.print(" ctrl_state=");    Serial1.print(ctrl & 0xFFu);
    Serial1.print(" ctrl_timeouts="); Serial1.print((ctrl >> 8) & 0xFFu);
    Serial1.print(" ctrl_qfails=");   Serial1.print((ctrl >> 16) & 0xFFFFu);
    Serial1.println();
    Serial1.print("stream: pkts=");   Serial1.print(pkts);
    Serial1.print(" in ");            Serial1.print(elapsed);
    Serial1.print(" ms, loops=");     Serial1.print(loops);
    Serial1.print(" (");              Serial1.print(lps);
    Serial1.print(" loops/s), xerr="); Serial1.print(xerr);
    Serial1.print(" unders=");        Serial1.print(unders);
    Serial1.print(" queued=");        Serial1.print(queued);
    Serial1.println();

    bool stages = (s1 == 0xA5B00001u) && (s2 == 0xA5B00002u) && (s3 == 0xA5B00003u)
               && (s4 == 0xA5B00004u) && (s5 == 0xA5B00005u) && (s6 == 0xA5B00006u)
               && (s7 == 0xA5B00007u) && (done == 0xD0DE0009u);

    /* Each verdict is its own line, ordered so a red run localises itself. A
     * dead clock, an unarmed mask, a device that never arrived, a device that
     * arrived and was never claimed, a format the device would not accept, a
     * control request it never answered, a ring that would not arm, and a
     * stream that carried nothing are EIGHT different faults. Collapsing them
     * into one FAIL is how a phase loses a day. */
    bool clock  = dwt != 0u;
    bool intr   = ((usbintr >> 18) & 1u) != 0u;            /* UAIE */
    bool plug   = ((portsc & 1u) == 0u) && ((portsc2 & 1u) == 1u);
    bool claimed = (bound == 1u) && (vid != 0u);
    bool format = ((int32_t)alt) >= 0;
    bool cseq   = ctrl == 0u;
    bool ready  = armed == 1u;

    Serial1.println(stages  ? "STAGES=PASS"            : "STAGES=FAIL");
    Serial1.println(clock   ? "DWT_ALIVE=PASS"         : "DWT_ALIVE=FAIL");
    Serial1.println(intr    ? "USBINTR_UAIE=PASS"      : "USBINTR_UAIE=FAIL");
    Serial1.println(plug    ? "PLUG_TRANSITION=PASS"   : "PLUG_TRANSITION=FAIL");
    Serial1.println(claimed ? "DEVICE_CLAIMED=PASS"    : "DEVICE_CLAIMED=FAIL");
    Serial1.println(format  ? "FORMAT_NEGOTIATED=PASS" : "FORMAT_NEGOTIATED=FAIL");
    Serial1.println(cseq    ? "CONTROL_SEQUENCE=PASS"  : "CONTROL_SEQUENCE=FAIL");
    Serial1.println(ready   ? "STREAM_ARMED=PASS"      : "STREAM_ARMED=FAIL");

    /* ★★ THE TWO WORLD-SPLIT TOKENS. Both are PRINTED unconditionally and both
     * are PRESENCE-checked, not value-checked, by the QEMU gate.
     *
     * STREAM_PACKETS: 0 in QEMU for the reason at the head of this file.
     * TRANSPORT_CLEAN: trivially PASS in QEMU, because a stream that
     * transmitted nothing cannot have had a transmission error -- asserting it
     * there would be a check that cannot fail, which this tree treats as worse
     * than no check at all. On silicon both mean what they say. */
    Serial1.println(pkts > 0u ? "STREAM_PACKETS=PASS"  : "STREAM_PACKETS=FAIL");
    Serial1.println(xerr == 0u ? "TRANSPORT_CLEAN=PASS" : "TRANSPORT_CLEAN=FAIL");

    /* The control-plane verdict, and the one the QEMU gate asserts. It stops at
     * "armed" ON PURPOSE; see the header. */
    bool pass = ok && stages && clock && intr && plug && claimed && format
             && cseq && ready;
    Serial1.println(pass ? "AUDIO_CM4=PASS" : "AUDIO_CM4=FAIL");
    Serial1.println("CM4USBAUDIO-DONE");
}

void loop() {}

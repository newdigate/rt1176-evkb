/* rk055_panel_test - RT1176 -> RK055HDMIPI4MA0 720x1280 MIPI-DSI panel gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * M1 scope: the SoC chain only -- display clocks, LCDIFv2 scanout, MIPI-DSI
 * host.  The HX8394 panel driver (M2) and the test pattern + checksums (M3)
 * are added to this same gate as those milestones land.
 *
 * Every stage token is emitted UNCONDITIONALLY so the first FAIL pinpoints the
 * broken layer rather than the run simply stopping.
 *
 * Uses Serial1 (the LPUART console run_qemu.sh captures via `-serial file:`),
 * not Serial (native USB CDC, which QEMU would not capture here).
 */
#include <Arduino.h>
#include "Display.h"
// The two diagnostic accessors used only on the PANEL_FAIL path below.
// hx8394.h says WHICH of hx8394Init()'s six early returns fired (and, for the
// tuning table, at which index/opcode); mipi_dsi.h says WHY the DSI host
// refused that packet.  Neither is reached on a passing run.
#include "hx8394.h"
#include "mipi_dsi.h"

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RK055_PANEL_BEGIN");
    Serial1.printf("PANEL=%s\n", PANEL_NAME);
    Serial1.printf("GEOM=%ux%u PIXCLK=%lu LANES=%lu\n",
                   (unsigned)PANEL_WIDTH, (unsigned)PANEL_HEIGHT,
                   (unsigned long)PANEL_PIXEL_CLK_HZ,
                   (unsigned long)PANEL_DSI_LANES);

    Display.begin();
    Serial1.printf("CLK_%s\n",     Display.clkOk()   ? "OK" : "FAIL");
    Serial1.printf("LCDIFV2_%s\n", Display.lcdifOk() ? "OK" : "FAIL");
    Serial1.printf("DSI_%s\n",     Display.dsiOk()   ? "OK" : "FAIL");
    Serial1.printf("PANEL_%s\n", Display.panelOk() ? "OK" : "FAIL");

    // Diagnostics, printed ONLY when the panel stage failed and strictly AFTER
    // the PANEL_ token -- run_qemu.sh greps for "PANEL_OK", so nothing here may
    // precede it or contain that string.  These lines are additive: a passing
    // run's output is byte-identical to before.
    //
    // Two independent questions, answered separately because they have
    // different answers:
    //   PANEL_ERR  -- WHICH of hx8394Init()'s six early returns fired.  For
    //                 TUNING the index into the 21-entry table and the failing
    //                 command's opcode come too; IDX=255 means "not a tuning
    //                 failure" (HX8394_NO_INDEX).
    //   DSI_ERR    -- WHY the DSI host refused that packet, plus the host
    //                 status registers sampled at the failure point.  This is
    //                 still valid here because hx8394Init() bails at the FIRST
    //                 refusal and sends nothing afterwards, so the last
    //                 dsiWrite() recorded IS the one that failed.
    //                 SPINS=entry/start/idle separates the three timeout
    //                 flavours: a full ENTRY count means the previous packet
    //                 never drained, a full START count means this packet never
    //                 left the gate, a full IDLE count means it started and
    //                 never finished.
    // On HX8394_ERR_GPIO no packet was ever sent, so the DSI_ERR line describes
    // whatever the last write was (nothing, on a fresh boot) -- read PANEL_ERR
    // first and only trust DSI_ERR when PANEL_ERR names a packet.
    if (!Display.panelOk()) {
        Serial1.printf("PANEL_ERR=%s IDX=%u OPCODE=0x%02X\n",
                       hx8394ErrorName(hx8394LastError()),
                       (unsigned)hx8394LastErrorIndex(),
                       (unsigned)hx8394LastErrorOpcode());
        const DsiWriteStatus &d = dsiLastWriteStatus();
        Serial1.printf("DSI_ERR=%s DT=0x%02X LEN=%lu\n",
                       dsiWriteErrorName(d.error), (unsigned)d.data_type,
                       (unsigned long)d.len);
        Serial1.printf("DSI_PKT_STATUS=0x%08lX IRQ1=0x%08lX IRQ2=0x%08lX "
                       "RXERR=0x%08lX\n",
                       (unsigned long)d.pkt_status,
                       (unsigned long)d.irq_status,
                       (unsigned long)d.irq_status2,
                       (unsigned long)d.rx_error_status);
        Serial1.printf("DSI_FIFO=wr:%lu rd:%lu LOCK=0x%08lX "
                       "SPINS=entry:%lu start:%lu idle:%lu\n",
                       (unsigned long)d.fifo_wr_level,
                       (unsigned long)d.fifo_rd_level,
                       (unsigned long)d.dphy_lock,
                       (unsigned long)d.entry_spins,
                       (unsigned long)d.start_spins,
                       (unsigned long)d.idle_spins);
    }

    Serial1.println("RK055_PANEL_END");
}

void loop() {}

#include "Arduino.h"
#include "HardwareSerial.h"
#include "SPI.h"
#include "EventResponder.h"

static const int N = 16;
static const uint32_t ASYNC_GUARD = 2000000;  // yield-spin bound so a lost completion fails the stage instead of hanging
static DMAMEM uint8_t txbuf[N];    // DMA-accessed -> OCRAM (DTCM is DMA-unreachable)
static DMAMEM uint8_t rxbuf[N];
static DMAMEM uint8_t rxbuf2[N];
static EventResponder er;
static volatile bool async_cb_fired = false;
static void cb(EventResponderRef e) { async_cb_fired = true; }

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}
	for (int i = 0; i < N; i++) txbuf[i] = (uint8_t)(0xA0 ^ (i * 7));
	SPI.begin();
	SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
	bool ok = true;

	// STAGE_BLOCKING: full-duplex DMA; SDO->SDI loopback echoes tx into rx.
	for (int i = 0; i < N; i++) rxbuf[i] = 0;
	SPI.transfer(txbuf, rxbuf, N);
	bool b = true;
	for (int i = 0; i < N; i++) if (rxbuf[i] != txbuf[i]) b = false;
	Serial1.println(b ? "STAGE_BLOCKING=PASS" : "STAGE_BLOCKING=FAIL");
	if (!b) ok = false;

	// STAGE_ASYNC: full-duplex DMA; EventResponder fires on RX completion.
	for (int i = 0; i < N; i++) rxbuf2[i] = 0;
	er.attach(cb);
	async_cb_fired = false;
	bool started = SPI.transfer(txbuf, rxbuf2, N, er);
	uint32_t guard = 0;
	while (!async_cb_fired && ++guard < ASYNC_GUARD) yield();
	bool a = started && async_cb_fired;
	for (int i = 0; i < N; i++) if (rxbuf2[i] != txbuf[i]) a = false;
	Serial1.println(a ? "STAGE_ASYNC=PASS" : "STAGE_ASYNC=FAIL");
	if (!a) ok = false;

	SPI.endTransaction();
	Serial1.println(ok ? "SPI_DMA_ALL=PASS" : "SPI_DMA_ALL=FAIL");
}
void loop() {}
